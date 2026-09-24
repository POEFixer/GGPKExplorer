#include "PsgViewer.h"
#include "../core/GGPKManager.h"
#include "../core/DatSchema.h"
#include "../core/DatParser.h"
#include "../core/StatFormatter.h"
#include "../core/parsers/CsdParser.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cmath>

namespace GGPKExp {

// ---------------------------------------------------------------------------
// Local helpers for schema/cell decoding
// ---------------------------------------------------------------------------

namespace {

struct ColumnMap {
    std::unordered_map<std::string, int> offsets;   // column name -> byte offset
    std::unordered_map<std::string, const ColumnDef*> defs;  // column name -> ColumnDef*
};

ColumnMap BuildColumnMap(const SchemaTable& table) {
    ColumnMap out;
    int offset = 0;
    for (const auto& col : table.columns) {
        if (!col.name.empty()) {
            out.offsets[col.name] = offset;
            out.defs[col.name] = &col;
        }
        offset += GetColumnByteSize(col);
    }
    return out;
}

// Convenience: decode a single cell by column name; returns monostate if
// the column wasn't found in the map.
CellValue DecodeByName(const DatFile& dat, uint32_t row, const ColumnMap& map, const std::string& name) {
    auto offIt = map.offsets.find(name);
    auto defIt = map.defs.find(name);
    if (offIt == map.offsets.end() || defIt == map.defs.end()) return std::monostate{};
    return DecodeCell(dat, row, static_cast<uint32_t>(offIt->second), *defIt->second);
}

std::string GetString(const CellValue& v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    return {};
}

bool GetBool(const CellValue& v) {
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v);
    return false;
}

int32_t GetI32(const CellValue& v) {
    if (std::holds_alternative<int32_t>(v)) return std::get<int32_t>(v);
    return 0;
}

uint16_t GetU16(const CellValue& v) {
    if (std::holds_alternative<uint16_t>(v)) return std::get<uint16_t>(v);
    return 0;
}

int64_t GetForeignRow(const CellValue& v) {
    if (std::holds_alternative<int64_t>(v)) return std::get<int64_t>(v);
    return -1;
}

std::vector<int64_t> GetForeignRowArray(const CellValue& v) {
    std::vector<int64_t> out;
    if (!std::holds_alternative<CellArray>(v)) return out;
    const auto& arr = std::get<CellArray>(v);
    out.reserve(arr.items.size());
    for (const auto& item : arr.items) {
        if (std::holds_alternative<int64_t>(item)) out.push_back(std::get<int64_t>(item));
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// LoadData / Clear
// ---------------------------------------------------------------------------

void PsgViewer::LoadData(std::vector<uint8_t> data, const std::string& path) {
    // Minimal kickoff: stash the PSG bytes and enter Preparing. The heavy
    // synchronous GGPK read work runs in DoMainThreadPrep on the second
    // Preparing frame (after the spinner has drawn once), so the UI never
    // freezes before the spinner appears.
    Clear();
    m_loadingPath = path;
    m_pending.data = std::move(data);
    m_pending.path = path;
    m_prepareFrameDrawn = false;
    m_loadState = LoadState::Preparing;
}

void PsgViewer::DoMainThreadPrep() {
    if (m_pending.data.empty()) {
        m_error = "Empty file data";
        m_loadState = LoadState::Error;
        m_pending = {};
        return;
    }
    if (!m_manager || !m_schema || !m_taskRunner) {
        m_error = "Viewer context not fully initialized";
        m_loadState = LoadState::Error;
        m_pending = {};
        return;
    }

    // --------- Gather all GGPK bytes on the main thread ---------
    // The worker thread only sees bytes + schema copies; it never touches
    // the (thread-unsafe) GGPKManager.
    PsgAsyncInput input;
    input.psgBytes = std::move(m_pending.data);

    auto readGgpk = [this](const std::string& p, std::vector<uint8_t>& out) -> bool {
        void* np = m_manager->FindNodeByPath(p);
        if (!np) return false;
        auto fr = m_manager->ReadFile(np);
        if (!fr) return false;
        out = std::move(fr.value);
        return true;
    };

    // Stats.datc64
    if (!readGgpk("Data/Balance/Stats.datc64", input.statsBytes))
        readGgpk("Data/Stats.datc64", input.statsBytes);

    // PassiveSkills.datc64
    if (!readGgpk("Data/Balance/PassiveSkills.datc64", input.passiveBytes))
        readGgpk("Data/PassiveSkills.datc64", input.passiveBytes);

    // All .csd files under Data/StatDescriptions/
    {
        const FileEntry& root = m_manager->GetRoot();
        const FileEntry* dataDir = nullptr;
        for (const auto& c : root.children) {
            if (c.isDirectory && !c.name.empty()) {
                std::string n; n.reserve(c.name.size());
                for (char ch : c.name) n.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
                if (n == "data") { dataDir = &c; break; }
            }
        }
        const FileEntry* statDir = nullptr;
        if (dataDir) {
            for (const auto& c : dataDir->children) {
                if (c.isDirectory && !c.name.empty()) {
                    std::string n; n.reserve(c.name.size());
                    for (char ch : c.name) n.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
                    if (n == "statdescriptions") { statDir = &c; break; }
                }
            }
        }
        if (statDir) {
            for (const auto& f : statDir->children) {
                if (f.isDirectory) continue;
                std::string lower; lower.reserve(f.name.size());
                for (char ch : f.name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
                if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".csd") == 0) {
                    std::vector<uint8_t> bytes;
                    if (readGgpk(f.fullPath, bytes))
                        input.csdBytes.push_back(std::move(bytes));
                }
            }
        }
    }

    // --------- Schema copies (cheap) ---------
    if (const SchemaTable* t = m_schema->FindTable("Stats"))  input.statsTable  = *t;
    if (const SchemaTable* t = m_schema->FindTable("PassiveSkills")) input.passiveTable = *t;

    // --------- Launch worker ---------
    m_loadState = LoadState::Loading;
    m_loadFuture = m_taskRunner->Submit<PsgAsyncResult>(
        [payload = std::move(input)](std::atomic<bool>& /*cancel*/,
                                     AsyncTaskRunner::ProgressCallback /*progress*/) -> Result<PsgAsyncResult> {
            return PsgViewer::DoParseWork(payload);
        });

    m_pending = {};
}

void PsgViewer::Clear() {
    m_psg = {};
    m_skillInfo.clear();
    m_skillInfoLoaded = false;
    m_icons.Clear();
    m_iconsLoaded = false;
    m_iconQueue.clear();
    m_iconLoadState = IconLoadState::Idle;
    m_iconsLoadedThisLoad = 0;
    m_iconsTotalThisLoad = 0;
    m_ascNodeToClass.clear();
    m_selectedClassIdx = -1;
    m_loadState = LoadState::Idle;
    m_loadingPath.clear();
    // Worker's promise is held alive by a shared_ptr captured in the task
    // lambda (see AsyncTaskRunner::Submit). Dropping our future just abandons
    // our half of the shared state; the worker still completes cleanly and
    // the result is destroyed along with the shared_ptr.
    m_loadFuture = {};
    m_loaded = false;
    m_error.clear();
    m_selectedGroup = -1;
    m_zoom = 1.0f;
    m_pan = {0, 0};
    m_dragging = false;
    m_selectedNodeHash = 0;
    m_hoveredNodeHash = 0;
    m_needsFit = true;
    m_minX = m_maxX = m_minY = m_maxY = 0;
    m_pending = {};
    m_prepareFrameDrawn = false;
    m_searchBuf[0] = '\0';
    m_searchMatches.clear();
    m_searchMatchSet.clear();
    m_searchIndex = -1;
    m_searchableTextCache.clear();
    m_canvasLastSize = ImVec2(0, 0);

    // Language state (Task 3.5): reset per-language maps; keep
    // m_currentLang so the user's last choice survives reload.
    m_availableLangs.clear();
    m_statFormatsByLang.clear();
    m_statRowToId.clear();
    m_settingsDirty = false;
}

// ---------------------------------------------------------------------------
// Render — default delegate to Visual view
// ---------------------------------------------------------------------------

void PsgViewer::Render() {
    // Default delegate: visual is the main view. ContentPanel calls
    // RenderVisual()/RenderMetadata() directly for the split tabs; any
    // generic IViewer caller gets the tree.
    RenderVisual();
}

// ---------------------------------------------------------------------------
// ComputeBounds
// ---------------------------------------------------------------------------

void PsgViewer::ComputeBounds() {
    if (m_psg.groups.empty()) return;

    m_minX = 1e9f;  m_maxX = -1e9f;
    m_minY = 1e9f;  m_maxY = -1e9f;

    for (const auto& group : m_psg.groups) {
        for (const auto& node : group.nodes) {
            if (node.worldX < m_minX) m_minX = node.worldX;
            if (node.worldX > m_maxX) m_maxX = node.worldX;
            if (node.worldY < m_minY) m_minY = node.worldY;
            if (node.worldY > m_maxY) m_maxY = node.worldY;
        }
    }
}

// ---------------------------------------------------------------------------
// FitToView
// ---------------------------------------------------------------------------

void PsgViewer::FitToView(ImVec2 availSize) {
    float rangeX = m_maxX - m_minX;
    float rangeY = m_maxY - m_minY;
    if (rangeX < 1.0f) rangeX = 1.0f;
    if (rangeY < 1.0f) rangeY = 1.0f;

    float fitW = availSize.x * 0.9f;
    float fitH = availSize.y * 0.9f;

    m_zoom = (std::min)(fitW / rangeX, fitH / rangeY);

    float centreX = (m_minX + m_maxX) * 0.5f;
    float centreY = (m_minY + m_maxY) * 0.5f;

    m_pan.x = availSize.x * 0.5f - centreX * m_zoom;
    m_pan.y = availSize.y * 0.5f - centreY * m_zoom;
    m_needsFit = false;
}

// ---------------------------------------------------------------------------
// WorldToScreen / IsOnScreen
// ---------------------------------------------------------------------------

ImVec2 PsgViewer::WorldToScreen(float wx, float wy, ImVec2 origin, ImVec2 /*size*/) const {
    return ImVec2(origin.x + wx * m_zoom + m_pan.x,
                  origin.y + wy * m_zoom + m_pan.y);
}

bool PsgViewer::IsOnScreen(float wx, float wy, ImVec2 origin, ImVec2 size, float margin) const {
    ImVec2 sp = WorldToScreen(wx, wy, origin, size);
    return sp.x >= origin.x - margin && sp.x <= origin.x + size.x + margin &&
           sp.y >= origin.y - margin && sp.y <= origin.y + size.y + margin;
}

// ---------------------------------------------------------------------------
// RenderMetadata
// ---------------------------------------------------------------------------

void PsgViewer::RenderMetadata() {
    if (PollLoadState()) {
        DrawLoadingSpinner("Loading skill tree...");
        return;
    }
    if (m_loadState == LoadState::Error) {
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Load error: %s", m_error.c_str());
        return;
    }
    if (m_loadState == LoadState::Idle || !m_loaded) {
        ImGui::TextDisabled("No data loaded");
        return;
    }

    if (ImGui::BeginTable("##PsgHeader", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        auto row = [](const char* label, const char* fmt, ...) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            va_list args;
            va_start(args, fmt);
            ImGui::TextV(fmt, args);
            va_end(args);
        };

        row("Version", "%u", m_psg.version);
        row("Type", "%s (%u)", m_psg.type == 0 ? "Character" : (m_psg.type == 1 ? "Atlas" : "Unknown"), m_psg.type);

        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("Orbits");
            ImGui::TableSetColumnIndex(1);
            std::string orbitStr;
            for (size_t i = 0; i < m_psg.orbits.size(); ++i) {
                if (i > 0) orbitStr += ", ";
                orbitStr += std::to_string(m_psg.orbits[i]);
            }
            ImGui::TextWrapped("%s", orbitStr.c_str());
        }

        row("Root Nodes", "%u", static_cast<uint32_t>(m_psg.rootNodes.size()));
        row("Total Groups", "%u", static_cast<uint32_t>(m_psg.groups.size()));
        row("Total Nodes", "%u", m_psg.totalNodes);
        row("Total Edges", "%u", m_psg.totalEdges);
        row("Skill Info Loaded", "%s (%u entries)", m_skillInfoLoaded ? "yes" : "no",
            static_cast<uint32_t>(m_skillInfo.size()));
        row("Icons Loaded", "%s (%u textures)", m_iconsLoaded ? "yes" : "no",
            static_cast<uint32_t>(m_icons.LoadedCount()));

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Groups (%u) — click to expand:", static_cast<uint32_t>(m_psg.groups.size()));
    ImGui::Spacing();

    int totalRows = 0;
    for (int gi = 0; gi < static_cast<int>(m_psg.groups.size()); ++gi) {
        totalRows++;
        if (m_selectedGroup == gi)
            totalRows += static_cast<int>(m_psg.groups[static_cast<size_t>(gi)].nodes.size());
    }

    if (ImGui::BeginTable("##PsgGroups", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn("#",        ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Centre",   ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("AssocKey", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("BgOvr",    ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Jewel",    ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Nodes",    ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(totalRows);
        while (clipper.Step()) {
            int currentRow = 0;
            for (int gi = 0; gi < static_cast<int>(m_psg.groups.size()); ++gi) {
                const auto& group = m_psg.groups[static_cast<size_t>(gi)];
                bool expanded = (m_selectedGroup == gi);
                int groupRows = 1 + (expanded ? static_cast<int>(group.nodes.size()) : 0);

                if (currentRow + groupRows <= clipper.DisplayStart) {
                    currentRow += groupRows;
                    continue;
                }
                if (currentRow >= clipper.DisplayEnd) break;

                if (currentRow >= clipper.DisplayStart && currentRow < clipper.DisplayEnd) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    char label[32];
                    std::snprintf(label, sizeof(label), "%s %d", expanded ? "-" : "+", gi);
                    if (ImGui::Selectable(label, false, ImGuiSelectableFlags_SpanAllColumns)) {
                        m_selectedGroup = expanded ? -1 : gi;
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.1f, %.1f", group.centreX, group.centreY);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("0x%X", group.assocKey);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%u", group.bgOverride);
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(group.isJewel ? "Yes" : "");
                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%u", static_cast<uint32_t>(group.nodes.size()));
                }
                currentRow++;

                if (expanded) {
                    for (size_t ni = 0; ni < group.nodes.size(); ++ni) {
                        if (currentRow >= clipper.DisplayEnd) break;
                        if (currentRow >= clipper.DisplayStart) {
                            const auto& node = group.nodes[ni];
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextUnformatted("");
                            ImGui::TableSetColumnIndex(1);
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
                            ImGui::Text("  0x%08X", node.hash);
                            ImGui::PopStyleColor();
                            ImGui::TableSetColumnIndex(2);
                            ImGui::Text("%.1f, %.1f", node.worldX, node.worldY);
                            ImGui::TableSetColumnIndex(3);
                            ImGui::Text("o%u", node.orbit);
                            ImGui::TableSetColumnIndex(4);
                            ImGui::Text("i%u", node.orbitIndex);
                            ImGui::TableSetColumnIndex(5);
                            ImGui::Text("%u", static_cast<uint32_t>(node.edges.size()));
                        }
                        currentRow++;
                    }
                }
            }
        }
        clipper.End();
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
// DoParseWork — pure worker-thread parse of PSG + PassiveSkills + Stats + .csd
// ---------------------------------------------------------------------------

Result<PsgAsyncResult> PsgViewer::DoParseWork(const PsgAsyncInput& input) {
    PsgAsyncResult out;

    // --------- 1. Parse PSG ---------
    auto psgResult = Parsers::ParsePsg(input.psgBytes.data(), input.psgBytes.size());
    if (!psgResult)
        return Result<PsgAsyncResult>::Fail(psgResult.error);
    out.psg = std::move(psgResult.value);

    // --------- 2. Column maps ---------
    if (input.statsTable.columns.empty() || input.passiveTable.columns.empty())
        return Result<PsgAsyncResult>::Fail("Schema not provided");

    ColumnMap statsCols = BuildColumnMap(input.statsTable);
    ColumnMap passiveCols = BuildColumnMap(input.passiveTable);
    if (passiveCols.offsets.find("PassiveSkillGraphId") == passiveCols.offsets.end() ||
        passiveCols.offsets.find("Name") == passiveCols.offsets.end() ||
        statsCols.offsets.find("Id") == statsCols.offsets.end()) {
        return Result<PsgAsyncResult>::Fail("Required columns missing from schema");
    }

    // --------- 3. Stats.datc64 — rowIdx -> statId ---------
    std::unordered_map<int64_t, std::string> statRowToId;
    {
        DatFile dat = Parse(input.statsBytes);
        if (dat.valid) {
            for (uint32_t row = 0; row < dat.rowCount; ++row) {
                CellValue v = DecodeByName(dat, row, statsCols, "Id");
                auto id = GetString(v);
                if (!id.empty()) statRowToId[static_cast<int64_t>(row)] = std::move(id);
            }
        }
    }

    // --------- 4. All .csd files -> per-language statId -> format ---------
    Parsers::CsdMultiLangFormatMap statFormatsByLang;
    for (const auto& bytes : input.csdBytes) {
        std::vector<std::string> includes;
        Parsers::ParseCsdMultiLang(bytes.data(), bytes.size(), statFormatsByLang, includes);
        // Includes are already walked by virtue of loading every .csd file in
        // Data/StatDescriptions/, so we drop them here.
    }

    // --------- 5. PassiveSkills.datc64 ---------
    DatFile passives = Parse(input.passiveBytes);
    if (!passives.valid)
        return Result<PsgAsyncResult>::Fail("PassiveSkills.datc64 parse failed");

    std::vector<std::string> iconPaths;
    iconPaths.reserve(1024);

    for (uint32_t row = 0; row < passives.rowCount; ++row) {
        uint16_t graphId = GetU16(DecodeByName(passives, row, passiveCols, "PassiveSkillGraphId"));
        if (graphId == 0) continue;

        PassiveSkillInfo info;
        info.id = GetString(DecodeByName(passives, row, passiveCols, "Id"));
        info.name = GetString(DecodeByName(passives, row, passiveCols, "Name"));
        info.iconPath = GetString(DecodeByName(passives, row, passiveCols, "Icon_DDSFile"));
        info.flavourText = GetString(DecodeByName(passives, row, passiveCols, "FlavourText"));
        info.statRows = GetForeignRowArray(DecodeByName(passives, row, passiveCols, "Stats"));

        info.statValues[0] = GetI32(DecodeByName(passives, row, passiveCols, "Stat1Value"));
        info.statValues[1] = GetI32(DecodeByName(passives, row, passiveCols, "Stat2Value"));
        info.statValues[2] = GetI32(DecodeByName(passives, row, passiveCols, "Stat3Value"));
        info.statValues[3] = GetI32(DecodeByName(passives, row, passiveCols, "Stat4Value"));
        info.statValues[4] = GetI32(DecodeByName(passives, row, passiveCols, "Stat5Value"));

        info.isKeystone = GetBool(DecodeByName(passives, row, passiveCols, "IsKeystone"));
        info.isNotable = GetBool(DecodeByName(passives, row, passiveCols, "IsNotable"));
        info.isJewelSocket = GetBool(DecodeByName(passives, row, passiveCols, "IsJewelSocket"));
        info.isAttribute = GetBool(DecodeByName(passives, row, passiveCols, "IsAttribute"));
        info.isAscendancyStartingNode = GetBool(DecodeByName(passives, row, passiveCols, "IsAscendancyStartingNode"));
        info.isJustIcon = GetBool(DecodeByName(passives, row, passiveCols, "IsJustIcon"));
        info.isMultipleChoice = GetBool(DecodeByName(passives, row, passiveCols, "IsMultipleChoice"));
        info.isMultipleChoiceOption = GetBool(DecodeByName(passives, row, passiveCols, "IsMultipleChoiceOption"));
        info.isExpansion = GetBool(DecodeByName(passives, row, passiveCols, "IsExpansion"));
        info.isProxyPassive = GetBool(DecodeByName(passives, row, passiveCols, "IsProxyPassive"));
        info.isAnointmentOnly = GetBool(DecodeByName(passives, row, passiveCols, "IsAnointmentOnly"));

        info.skillType = GetI32(DecodeByName(passives, row, passiveCols, "SkillType"));
        info.skillPointsGranted = GetI32(DecodeByName(passives, row, passiveCols, "SkillPointsGranted"));
        info.weaponPointsGranted = GetI32(DecodeByName(passives, row, passiveCols, "WeaponPointsGranted"));
        info.ascendancy = GetForeignRow(DecodeByName(passives, row, passiveCols, "Ascendancy"));
        info.masteryGroup = GetForeignRow(DecodeByName(passives, row, passiveCols, "MasteryGroup"));
        info.groupRow = GetForeignRow(DecodeByName(passives, row, passiveCols, "Group"));
        info.keystoneId = GetForeignRow(DecodeByName(passives, row, passiveCols, "KeystoneId"));
        info.atlasSubTree = GetForeignRow(DecodeByName(passives, row, passiveCols, "AtlasSubTree"));
        info.characters = GetForeignRowArray(DecodeByName(passives, row, passiveCols, "Characters"));
        info.buffs = GetForeignRowArray(DecodeByName(passives, row, passiveCols, "PassiveSkillBuffs"));
        info.reminderStrings = GetForeignRowArray(DecodeByName(passives, row, passiveCols, "ReminderStrings"));

        if (!info.iconPath.empty()) iconPaths.push_back(info.iconPath);
        out.skillInfo[graphId] = std::move(info);
    }

    // --------- 6. Build class -> ascendancy-subtree map ---------
    // Build undirected adjacency from PSG edges. Mirrors BuildClassAscendancyMap
    // but works on the local `out.psg` and `out.skillInfo` (no member state).
    std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
    adj.reserve(out.psg.totalNodes);
    for (const auto& group : out.psg.groups) {
        for (const auto& node : group.nodes) {
            for (const auto& edge : node.edges) {
                adj[node.hash].push_back(edge.targetHash);
                adj[edge.targetHash].push_back(node.hash);
            }
        }
    }
    for (uint32_t rootHash : out.psg.rootNodes) {
        std::unordered_set<uint32_t> visited;
        visited.insert(rootHash);
        std::vector<uint32_t> stack{rootHash};
        while (!stack.empty()) {
            uint32_t cur = stack.back();
            stack.pop_back();
            auto it = adj.find(cur);
            if (it == adj.end()) continue;
            for (uint32_t nb : it->second) {
                if (!visited.insert(nb).second) continue;
                auto infoIt = out.skillInfo.find(nb);
                if (infoIt == out.skillInfo.end()) continue;
                if (infoIt->second.ascendancy < 0) continue;
                out.ascNodeToClass[nb] = rootHash;
                stack.push_back(nb);
            }
        }
    }

    // --------- 7. Deduplicate icon paths ---------
    std::sort(iconPaths.begin(), iconPaths.end());
    iconPaths.erase(std::unique(iconPaths.begin(), iconPaths.end()), iconPaths.end());
    out.iconPaths = std::move(iconPaths);

    out.statFormatsByLang = std::move(statFormatsByLang);
    out.statRowToId = std::move(statRowToId);

    return Result<PsgAsyncResult>::Ok(std::move(out));
}

// ---------------------------------------------------------------------------
// LoadIconsSync — main-thread D3D texture creation for parsed icon paths
// ---------------------------------------------------------------------------

void PsgViewer::LoadIconsSync(const std::vector<std::string>& iconPaths) {
    // Note: despite the name, this no longer blocks. It seeds the icon queue
    // which PumpIconQueue() drains a handful at a time from PollLoadState,
    // rate-limiting DDS decode to keep the NVIDIA driver from crashing.
    if (!m_device) {
        m_iconsLoaded = false;
        return;
    }
    m_icons.Initialize(m_device, m_manager);

    m_iconQueue.clear();
    for (const auto& p : iconPaths) {
        if (!p.empty()) m_iconQueue.push_back(p);
    }
    m_iconsLoadedThisLoad = 0;
    m_iconsTotalThisLoad = static_cast<int>(m_iconQueue.size());
    if (m_iconQueue.empty()) {
        m_iconLoadState = IconLoadState::Done;
        m_iconsLoaded = true;
    } else {
        m_iconLoadState = IconLoadState::Loading;
        m_iconsLoaded = false;
    }
}

// ---------------------------------------------------------------------------
// PumpIconQueue — drains up to kIconsPerFrame icons each call, wrapping every
// load in SEH to survive NVIDIA driver crashes in CreateDDSTextureFromMemory.
// ---------------------------------------------------------------------------

void PsgViewer::PumpIconQueue() {
    if (m_iconLoadState != IconLoadState::Loading) return;
    int budget = kIconsPerFrame;
    while (budget-- > 0 && !m_iconQueue.empty()) {
        std::string path = std::move(m_iconQueue.front());
        m_iconQueue.pop_front();
        m_icons.LoadOneSafe(path);
        m_iconsLoadedThisLoad++;
    }
    if (m_iconQueue.empty()) {
        m_iconLoadState = IconLoadState::Done;
        m_iconsLoaded = true;
    }
}

// ---------------------------------------------------------------------------
// PollLoadState — drives the state machine from the render thread
// ---------------------------------------------------------------------------

bool PsgViewer::PollLoadState() {
    // Preparing: the first frame draws the spinner with no work done, the
    // second frame does the synchronous GGPK reads + worker dispatch. This
    // keeps the UI responsive so users see the spinner immediately instead
    // of a ~1s freeze before loading visibly starts.
    if (m_loadState == LoadState::Preparing) {
        if (!m_prepareFrameDrawn) {
            m_prepareFrameDrawn = true;
            return true;   // first frame — just draw spinner
        }
        DoMainThreadPrep();   // second frame — GGPK reads + launch worker
        return true;          // state is now Loading or Error; spinner anyway
    }
    if (m_loadState == LoadState::Error) {
        return false;   // caller renders the error message itself
    }
    if (m_loadState == LoadState::Idle) {
        return false;   // nothing loaded yet — caller shows its empty state
    }
    if (m_loadState == LoadState::Ready) {
        // Drain a budget of icon loads each frame; NVIDIA-driver-safe via SEH.
        PumpIconQueue();
        return false;
    }

    // Loading: check the future.
    if (!m_loadFuture.valid()) {
        // Defensive: shouldn't happen, but treat as error if it does.
        m_error = "Async load future lost";
        m_loadState = LoadState::Error;
        return false;
    }
    if (m_loadFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return true;   // spinner
    }

    Result<PsgAsyncResult> res = m_loadFuture.get();
    if (!res) {
        m_error = res.error;
        m_loadState = LoadState::Error;
        return false;
    }

    // Success: move results into members.
    PsgAsyncResult& r = res.value;
    m_psg = std::move(r.psg);
    m_skillInfo = std::move(r.skillInfo);
    m_ascNodeToClass = std::move(r.ascNodeToClass);
    m_statFormatsByLang = std::move(r.statFormatsByLang);
    m_statRowToId = std::move(r.statRowToId);

    // Build available-languages list: English first, rest alphabetical.
    m_availableLangs.clear();
    m_availableLangs.reserve(m_statFormatsByLang.size());
    for (auto& [lang, _unused] : m_statFormatsByLang) {
        m_availableLangs.push_back(lang);
    }
    std::sort(m_availableLangs.begin(), m_availableLangs.end());
    auto itEn = std::find(m_availableLangs.begin(), m_availableLangs.end(),
                          std::string("English"));
    if (itEn != m_availableLangs.end() && itEn != m_availableLangs.begin()) {
        std::rotate(m_availableLangs.begin(), itEn, itEn + 1);
    }

    // Initialize active language from settings (fallback: "English").
    if (m_settings) {
        m_currentLang = m_settings->statDescLanguage;
    }
    if (m_currentLang.empty() ||
        m_statFormatsByLang.find(m_currentLang) == m_statFormatsByLang.end()) {
        m_currentLang = "English";
    }

    m_skillInfoLoaded = !m_skillInfo.empty();

    // Recompute bounds off the freshly moved psg.
    ComputeBounds();

    // Main-thread icon upload — short freeze, no spinner during this window
    // (acceptable per the spec).
    LoadIconsSync(r.iconPaths);

    m_loaded = true;
    m_needsFit = true;
    m_loadState = LoadState::Ready;
    BuildSearchableCache();
    return false;
}

// ---------------------------------------------------------------------------
// Search helpers
// ---------------------------------------------------------------------------

void PsgViewer::BuildSearchableCache() {
    m_searchableTextCache.clear();
    for (const auto& group : m_psg.groups) {
        for (const auto& node : group.nodes) {
            auto it = m_skillInfo.find(node.hash);
            if (it == m_skillInfo.end()) continue;
            std::string s = it->second.name;
            // Format stats on demand in English (language-neutral search
            // avoids rebuilding the index on language switch). Mirrors the
            // tooltip's on-demand format path.
            const Parsers::CsdFormatMap* englishMap = nullptr;
            {
                auto itEn = m_statFormatsByLang.find("English");
                if (itEn != m_statFormatsByLang.end()) englishMap = &itEn->second;
            }
            if (englishMap) {
                const auto& info = it->second;
                for (size_t i = 0; i < info.statRows.size(); ++i) {
                    const int64_t statRow = info.statRows[i];
                    auto idIt = m_statRowToId.find(statRow);
                    if (idIt == m_statRowToId.end()) continue;
                    auto fmtIt = englishMap->find(idIt->second);
                    if (fmtIt == englishMap->end()) continue;
                    const int32_t value = (i < info.statValues.size())
                                              ? info.statValues[i] : 0;
                    std::string line = FormatStat(fmtIt->second, {value});
                    if (!line.empty()) {
                        s += ' ';
                        s += line;
                    }
                }
            }
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            m_searchableTextCache.emplace(node.hash, std::move(s));
        }
    }
}

void PsgViewer::RebuildSearchMatches() {
    m_searchMatches.clear();
    m_searchMatchSet.clear();
    m_searchIndex = -1;
    std::string needle = m_searchBuf;
    for (auto& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (needle.empty()) return;
    for (const auto& group : m_psg.groups) {
        for (const auto& node : group.nodes) {
            auto it = m_searchableTextCache.find(node.hash);
            if (it == m_searchableTextCache.end()) continue;
            if (it->second.find(needle) != std::string::npos) {
                m_searchMatches.push_back(node.hash);
                m_searchMatchSet.insert(node.hash);
            }
        }
    }
    if (!m_searchMatches.empty()) m_searchIndex = 0;
}

void PsgViewer::NavigateNext() {
    if (m_searchMatches.empty()) return;
    m_searchIndex = (m_searchIndex + 1) % static_cast<int>(m_searchMatches.size());
    PanToNode(m_searchMatches[m_searchIndex]);
}

void PsgViewer::NavigatePrev() {
    if (m_searchMatches.empty()) return;
    int n = static_cast<int>(m_searchMatches.size());
    m_searchIndex = (m_searchIndex - 1 + n) % n;
    PanToNode(m_searchMatches[m_searchIndex]);
}

void PsgViewer::PanToNode(uint32_t nodeHash) {
    auto it = m_psg.nodeIndex.find(nodeHash);
    if (it == m_psg.nodeIndex.end()) return;
    if (it->second.first >= m_psg.groups.size()) return;
    const auto& group = m_psg.groups[it->second.first];
    if (it->second.second >= group.nodes.size()) return;
    const auto& node = group.nodes[it->second.second];

    if (m_canvasLastSize.x > 1.0f && m_canvasLastSize.y > 1.0f) {
        m_pan.x = m_canvasLastSize.x * 0.5f - node.worldX * m_zoom;
        m_pan.y = m_canvasLastSize.y * 0.5f - node.worldY * m_zoom;
    }
    m_selectedNodeHash = nodeHash;
}

// ---------------------------------------------------------------------------
// DrawLoadingSpinner — centered spinner + dim overlay + status text
// ---------------------------------------------------------------------------

void PsgViewer::DrawLoadingSpinner(const char* statusText) {
    ImVec2 regionMin = ImGui::GetCursorScreenPos();
    ImVec2 regionSize = ImGui::GetContentRegionAvail();
    if (regionSize.x < 1.0f || regionSize.y < 1.0f) return;

    ImVec2 regionMax(regionMin.x + regionSize.x, regionMin.y + regionSize.y);
    ImVec2 center(regionMin.x + regionSize.x * 0.5f, regionMin.y + regionSize.y * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(regionMin, regionMax, IM_COL32(0, 0, 0, 180));

    const float radius = 22.0f;
    const int segments = 12;
    float t = static_cast<float>(ImGui::GetTime());
    for (int i = 0; i < segments; ++i) {
        float angle = t * 5.0f + static_cast<float>(i) * 3.14159265f * 2.0f / static_cast<float>(segments);
        float alpha = static_cast<float>(segments - i) / static_cast<float>(segments);
        ImVec2 p(center.x + cosf(angle) * radius, center.y + sinf(angle) * radius);
        dl->AddCircleFilled(p, 3.5f, IM_COL32(255, 255, 255, static_cast<int>(alpha * 255)));
    }

    ImVec2 textSize = ImGui::CalcTextSize(statusText);
    dl->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y + radius + 12.0f),
                IM_COL32(255, 255, 255, 255), statusText);

    // Reserve the space in ImGui layout so subsequent widgets don't overlap.
    ImGui::Dummy(regionSize);
}

} // namespace GGPKExp
