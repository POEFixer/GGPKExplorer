#include "../CellEditorPanel.h"
#include "../DatViewer.h"
#include "../../core/DatParser.h"
#include "../../core/DatSchema.h"
#include "../../core/VirtualTree.h"
#include "../../core/GGPKManager.h"
#include "IconHelpers.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace GGPKExp {

// -----------------------------------------------------------------------------
// Phase 5 / T15 — ForeignRow cell editor.
//
// Opens a searchable row picker for the column's target table (col.refTable).
// The target DAT is decoded on a background std::async task the first time
// the panel binds to a given refTable; subsequent frames poll the future. The
// current value is int64_t (row id; -1 → null).
//
// UI layout:
//   Target: <refTable>.datc64
//   Current: row <n>  |  <null>
//   [ search box                                ]
//   [-------- scrollable row list -------------]     (ImGuiListClipper)
//   [ Clear (null) ] [ Go to current ]  (N rows, K matching)
//
// Clicking a row stores the id into m_buffer, sets dirty; Apply commits via
// the CellEditorPanel ↔ DatViewer contract.
// -----------------------------------------------------------------------------

namespace {

// Case-insensitive lowercase of an ASCII-ish string (sufficient for search).
std::string ToLowerAscii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

// Extract the basename (everything after the last '/') and lowercase it.
std::string LowerBasename(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    return ToLowerAscii(base);
}

// Resolve the GGPK path of a target DAT given its table name (e.g. "Stats").
// Walks the virtual tree and matches the first file whose basename equals
// "<refTable>.datc64" case-insensitively. This handles localized variants
// living under data/balance/<lang>/... — we pick whichever copy the tree
// enumerator yields first (English base path preferred by tree ordering).
std::string ResolveTargetDatPath(VirtualTree* tree, const std::string& refTable) {
    if (!tree || refTable.empty()) return {};
    std::string wantBase = ToLowerAscii(refTable) + ".datc64";
    std::string found;
    tree->ForEachFileUnder("",
        [&](const std::string& fullPath, const std::string& name,
            int64_t /*fileSize*/, void* /*nodePtr*/) {
            if (!found.empty()) return;
            std::string lowerName = ToLowerAscii(name);
            if (lowerName == wantBase) {
                found = fullPath;
            }
        });
    return found;
}

// Decode every row of a parsed DAT using its schema.
std::vector<std::vector<CellValue>> DecodeAll(const DatFile& d, const SchemaTable* t) {
    std::vector<std::vector<CellValue>> rows;
    if (!t) return rows;
    rows.reserve(d.rowCount);
    for (uint32_t r = 0; r < d.rowCount; ++r) {
        std::vector<CellValue> row;
        row.reserve(t->columns.size());
        uint32_t colOff = 0;
        for (const auto& col : t->columns) {
            row.push_back(DecodeCell(d, r, colOff, col));
            colOff += static_cast<uint32_t>(GetColumnByteSize(col));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace

void CellEditorPanel::RenderForeignRow()
{
    if (!m_colDef || !m_owner) return;

    const bool fa6 = m_owner->IsFA6Available();
    DatSchema* schema = m_owner->GetSchema();
    VirtualTree* tree = m_owner->GetTree();
    GGPKManager* mgr  = m_owner->GetGGPKManager();
    if (!schema || !tree || !mgr) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                           "Schema / tree / manager not available");
        return;
    }

    // -------------------------------------------------------------------------
    // 1. Kick off async target-table load whenever refTable changes.
    //    Re-bind to another ForeignRow cell with a different refTable triggers
    //    a fresh load; binding to the same refTable reuses the cached rows.
    // -------------------------------------------------------------------------
    if (m_colDef->refTable != m_targetLastTable) {
        // Wait out any outstanding load from the previous refTable before we
        // stomp state. Drain via .get() to invalidate the future.
        if (m_targetLoadFuture.valid()) {
            m_targetLoadFuture.wait();
            (void)m_targetLoadFuture.get();
        }
        m_targetRows.clear();
        m_targetTable = nullptr;
        m_targetLoading = true;
        m_targetSearchQuery.clear();
        m_targetLastTable = m_colDef->refTable;

        std::string refTable = m_colDef->refTable;

        m_targetLoadFuture = std::async(std::launch::async,
            [schema, tree, mgr, refTable]()
                -> std::pair<std::vector<std::vector<CellValue>>, const SchemaTable*>
            {
                // Resolve full GGPK path (handles localized data/balance/<lang>/ layout).
                std::string path = ResolveTargetDatPath(tree, refTable);
                if (path.empty()) return { {}, nullptr };

                void* nodePtr = mgr->FindNodeByPath(path);
                if (!nodePtr) return { {}, nullptr };

                auto readRes = mgr->ReadFile(nodePtr);
                if (!readRes || readRes.value.empty()) return { {}, nullptr };

                DatFile d = Parse(readRes.value);
                if (!d.valid) return { {}, nullptr };

                // Schema lookup: tables are keyed by stem (e.g. "Stats"), not
                // by filename. Pass refTable bare, not the ".datc64" path.
                const SchemaTable* t = schema->FindTable(refTable);
                if (!t) return { {}, nullptr };

                auto rows = DecodeAll(d, t);
                return { std::move(rows), t };
            });
    }

    // -------------------------------------------------------------------------
    // 2. Poll the future (non-blocking). Commit the result once ready.
    // -------------------------------------------------------------------------
    if (m_targetLoading && m_targetLoadFuture.valid() &&
        m_targetLoadFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        auto result = m_targetLoadFuture.get();
        m_targetRows = std::move(result.first);
        m_targetTable = result.second;
        m_targetLoading = false;
    }

    // -------------------------------------------------------------------------
    // 3. Header: target table name + current row id.
    // -------------------------------------------------------------------------
    const int64_t currentRow = std::holds_alternative<int64_t>(m_buffer)
        ? std::get<int64_t>(m_buffer)
        : int64_t{-1};

    ImGui::Text("Target: %s.datc64", m_colDef->refTable.c_str());
    if (currentRow < 0) {
        ImGui::Text("Current: <null>");
    } else {
        ImGui::Text("Current: row %lld", static_cast<long long>(currentRow));
    }
    ImGui::Separator();

    // -------------------------------------------------------------------------
    // 4. Loading indicator. Animated bar with indeterminate sweep.
    // -------------------------------------------------------------------------
    if (m_targetLoading) {
        ImGui::TextUnformatted("Loading target table...");
        float sweep = std::fmod(static_cast<float>(ImGui::GetTime()) * 0.5f, 1.0f);
        ImGui::ProgressBar(sweep, ImVec2(-FLT_MIN, 0), "");
        return;
    }
    if (!m_targetTable) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                           "Target table not found or schema missing");
        // Still offer Clear so the user can null a broken ref.
        const std::string lblClearFallback = IconLabel(fa6, kIconClear, "Clear (null)");
        if (ImGui::Button(lblClearFallback.c_str())) {
            m_buffer = int64_t{-1};
            m_dirty = true;
        }
        return;
    }

    // -------------------------------------------------------------------------
    // 5. Search box. Case-insensitive substring match against the first
    //    non-array String column of the target table (common display label).
    // -------------------------------------------------------------------------
    char q[256];
    strncpy_s(q, sizeof(q), m_targetSearchQuery.c_str(), sizeof(q) - 1);
    q[sizeof(q) - 1] = '\0';
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##target_search", "Filter rows...", q, sizeof(q))) {
        m_targetSearchQuery = q;
    }

    // Identify the first String (non-array) column for display/filter.
    int labelCol = -1;
    for (size_t c = 0; c < m_targetTable->columns.size(); ++c) {
        const auto& cd = m_targetTable->columns[c];
        if (cd.type == ColumnType::String && !cd.isArray) {
            labelCol = static_cast<int>(c);
            break;
        }
    }

    const std::string lq = ToLowerAscii(m_targetSearchQuery);

    // Build the filtered index list. For an empty query we avoid populating
    // a vector of every row id; we iterate m_targetRows directly.
    std::vector<int> filtered;
    if (!lq.empty()) {
        filtered.reserve(m_targetRows.size());
        for (int r = 0; r < static_cast<int>(m_targetRows.size()); ++r) {
            std::string label;
            if (labelCol >= 0 && static_cast<size_t>(labelCol) < m_targetRows[r].size()) {
                const auto& v = m_targetRows[r][static_cast<size_t>(labelCol)];
                if (std::holds_alternative<std::string>(v)) {
                    label = std::get<std::string>(v);
                }
            }
            if (ToLowerAscii(label).find(lq) != std::string::npos) {
                filtered.push_back(r);
            }
        }
    }
    const bool filtering = !lq.empty();
    const int listCount = filtering
        ? static_cast<int>(filtered.size())
        : static_cast<int>(m_targetRows.size());

    // -------------------------------------------------------------------------
    // 6. Scrollable row list with ImGuiListClipper. Reserves vertical space
    //    for the footer (two buttons + stats line).
    // -------------------------------------------------------------------------
    const float footerH = ImGui::GetFrameHeightWithSpacing() + 8.0f;
    ImGui::BeginChild("##target_rows", ImVec2(0, -footerH), true);
    {
        ImGuiListClipper clipper;
        clipper.Begin(listCount);
        while (clipper.Step()) {
            for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; ++k) {
                const int r = filtering ? filtered[static_cast<size_t>(k)] : k;
                std::string label;
                if (labelCol >= 0 &&
                    static_cast<size_t>(labelCol) < m_targetRows[static_cast<size_t>(r)].size())
                {
                    const auto& v = m_targetRows[static_cast<size_t>(r)][static_cast<size_t>(labelCol)];
                    if (std::holds_alternative<std::string>(v)) {
                        label = std::get<std::string>(v);
                    }
                }

                char line[512];
                if (label.empty()) {
                    std::snprintf(line, sizeof(line), "%d", r);
                } else {
                    std::snprintf(line, sizeof(line), "%d  %s", r, label.c_str());
                }

                const bool sel = (static_cast<int64_t>(r) == currentRow);
                ImGui::PushID(r);
                if (ImGui::Selectable(line, sel)) {
                    m_buffer = static_cast<int64_t>(r);
                    m_dirty = true;
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();

    // -------------------------------------------------------------------------
    // 7. Footer: Clear, Go-to-current, stats.
    // -------------------------------------------------------------------------
    const std::string lblClearFR = IconLabel(fa6, kIconClear, "Clear (null)");
    if (ImGui::Button(lblClearFR.c_str())) {
        m_buffer = int64_t{-1};
        m_dirty = true;
    }
    ImGui::SameLine();
    const bool canNav = (currentRow >= 0) && !m_colDef->refTable.empty();
    if (!canNav) ImGui::BeginDisabled();
    const std::string lblGoto = IconLabel(fa6, kIconNavigate, "Go to current");
    if (ImGui::Button(lblGoto.c_str())) {
        m_owner->pendingNavPath = m_colDef->refTable + ".datc64";
        m_owner->pendingNavRow  = static_cast<int>(currentRow);
    }
    if (!canNav) ImGui::EndDisabled();
    ImGui::SameLine();
    if (filtering) {
        ImGui::TextDisabled("(%zu rows, %zu matching)",
                            m_targetRows.size(), filtered.size());
    } else {
        ImGui::TextDisabled("(%zu rows)", m_targetRows.size());
    }
}

} // namespace GGPKExp
