#include "MatTableViewer.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cctype>

namespace GGPKExp {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string FormatWithCommas(uint64_t value) {
    std::string raw = std::to_string(value);
    std::string result;
    int count = 0;
    for (int i = static_cast<int>(raw.size()) - 1; i >= 0; --i) {
        if (count > 0 && count % 3 == 0)
            result.insert(result.begin(), ',');
        result.insert(result.begin(), raw[static_cast<size_t>(i)]);
        ++count;
    }
    return result;
}

// Keep only hex digits [0-9A-F] from user input so we can match against
// the uppercase hex representation of any u64 hash regardless of case /
// '0x' prefix.
static std::string HexNeedle(const char* input) {
    std::string needle;
    for (const char* p = input; *p; ++p) {
        char c = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))
            needle.push_back(c);
    }
    return needle;
}

static std::string HashHex64(uint64_t hash) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// LoadData / Clear
// ---------------------------------------------------------------------------

void MatTableViewer::LoadData(std::vector<uint8_t> data, const std::string& path) {
    Clear();

    if (data.empty()) {
        m_error = "No data provided";
        return;
    }

    auto parseResult = Parsers::ParseMatTable(data.data(), data.size());
    if (!parseResult) {
        m_error = parseResult.error;
        return;
    }
    m_tableData = std::move(parseResult.value);

    m_filename = path;
    m_loaded = true;
    m_matsDirty = true;
    m_combosDirty = true;
}

void MatTableViewer::Clear() {
    m_tableData = Parsers::MatTableData{};
    m_loaded = false;
    m_filename.clear();
    m_error.clear();

    m_matsSearch[0] = '\0';
    m_matsFiltered.clear();
    m_matsDirty = true;
    m_matsSortCol = 0;
    m_matsSortDir = ImGuiSortDirection_Ascending;

    m_combosSearch[0] = '\0';
    m_combosFiltered.clear();
    m_combosDirty = true;
    m_combosSortCol = 0;
    m_combosSortDir = ImGuiSortDirection_Ascending;
    m_hideNullCombos = true;
}

// ---------------------------------------------------------------------------
// Filter rebuild
// ---------------------------------------------------------------------------

void MatTableViewer::RebuildMatsFilter() {
    m_matsDirty = false;
    m_matsFiltered.clear();

    const auto& mats = m_tableData.mats;
    const std::string needle = HexNeedle(m_matsSearch);

    if (needle.empty()) {
        m_matsFiltered.resize(mats.size());
        for (int i = 0; i < static_cast<int>(mats.size()); ++i)
            m_matsFiltered[i] = i;
        return;
    }

    m_matsFiltered.reserve(mats.size());
    for (int i = 0; i < static_cast<int>(mats.size()); ++i) {
        const std::string hexKey = HashHex64(mats[static_cast<size_t>(i)].keyHash);
        const std::string hexVal = HashHex64(mats[static_cast<size_t>(i)].valueHash);
        if (hexKey.find(needle) != std::string::npos ||
            hexVal.find(needle) != std::string::npos)
            m_matsFiltered.push_back(i);
    }
}

void MatTableViewer::RebuildCombosFilter() {
    m_combosDirty = false;
    m_combosFiltered.clear();

    const auto& combos = m_tableData.combos;
    const std::string needle = HexNeedle(m_combosSearch);

    m_combosFiltered.reserve(combos.size());
    for (int i = 0; i < static_cast<int>(combos.size()); ++i) {
        const auto& c = combos[static_cast<size_t>(i)];
        if (m_hideNullCombos && c.paramCount == 0)
            continue;
        if (!needle.empty()) {
            const std::string hexKey = HashHex64(c.comboKey);
            if (hexKey.find(needle) == std::string::npos)
                continue;
        }
        m_combosFiltered.push_back(i);
    }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void MatTableViewer::Render() {
    if (!m_loaded) {
        if (!m_error.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: %s", m_error.c_str());
        else
            ImGui::TextDisabled("No material table loaded");
        return;
    }

    RenderOverview();
    ImGui::Separator();

    if (ImGui::BeginTabBar("##MatTableTabs")) {
        const std::string tab1 = "Mats (" + FormatWithCommas(m_tableData.mats.size()) + ")";
        if (ImGui::BeginTabItem(tab1.c_str())) {
            RenderMatsTab();
            ImGui::EndTabItem();
        }
        const std::string tab2 = "Graph combos (" + FormatWithCommas(m_tableData.combos.size()) + ")";
        if (ImGui::BeginTabItem(tab2.c_str())) {
            RenderCombosTab();
            ImGui::EndTabItem();
        }
        const std::string tab3 = "Params (" + FormatWithCommas(m_tableData.paramIds.size()) + ")";
        if (ImGui::BeginTabItem(tab3.c_str())) {
            RenderParamsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void MatTableViewer::RenderOverview() {
    const uint32_t validCombos = m_tableData.ValidCombosCount();
    const std::string s1 = FormatWithCommas(m_tableData.mats.size());
    const std::string s2 = FormatWithCommas(m_tableData.combos.size());
    const std::string s2valid = FormatWithCommas(validCombos);
    const std::string s3 = FormatWithCommas(m_tableData.paramIds.size());

    ImGui::Text("%s | Version %u", m_filename.c_str(), m_tableData.version);
    ImGui::Text("Mats: %s   Graph combos: %s (%s with inline params)   Params: %s",
                s1.c_str(), s2.c_str(), s2valid.c_str(), s3.c_str());
    ImGui::TextDisabled(
        "Precompiled shader-graph manifest. Absence/wrong version makes PoE2 "
        "recompile shaders on the fly (stutters).");
}

// ---------------------------------------------------------------------------
// Section 1 tab
// ---------------------------------------------------------------------------

void MatTableViewer::RenderMatsTab() {
    if (ImGui::InputText("Search (hex)##mats", m_matsSearch, sizeof(m_matsSearch)))
        m_matsDirty = true;

    if (m_matsDirty)
        RebuildMatsFilter();

    const std::string filteredStr = FormatWithCommas(m_matsFiltered.size());
    const std::string totalStr    = FormatWithCommas(m_tableData.mats.size());
    ImGui::Text("Showing %s / %s", filteredStr.c_str(), totalStr.c_str());

    ImGui::Separator();

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable;

    if (!ImGui::BeginTable("##Mats", 3, flags, ImVec2(0.0f, 0.0f)))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#",         ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("keyHash",   ImGuiTableColumnFlags_WidthFixed, 180.0f);
    ImGui::TableSetupColumn("valueHash", ImGuiTableColumnFlags_WidthFixed, 180.0f);
    ImGui::TableHeadersRow();

    const auto& mats = m_tableData.mats;

    if (ImGuiTableSortSpecs* spec = ImGui::TableGetSortSpecs()) {
        if (spec->SpecsDirty && spec->SpecsCount > 0) {
            m_matsSortCol = spec->Specs[0].ColumnIndex;
            m_matsSortDir = spec->Specs[0].SortDirection;

            const int col = m_matsSortCol;
            const bool asc = (m_matsSortDir == ImGuiSortDirection_Ascending);
            std::sort(m_matsFiltered.begin(), m_matsFiltered.end(),
                [&mats, col, asc](int a, int b) {
                    int cmp = 0;
                    switch (col) {
                    case 0:
                        cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
                        break;
                    case 1:
                        if (mats[static_cast<size_t>(a)].keyHash < mats[static_cast<size_t>(b)].keyHash) cmp = -1;
                        else if (mats[static_cast<size_t>(a)].keyHash > mats[static_cast<size_t>(b)].keyHash) cmp = 1;
                        break;
                    case 2:
                        if (mats[static_cast<size_t>(a)].valueHash < mats[static_cast<size_t>(b)].valueHash) cmp = -1;
                        else if (mats[static_cast<size_t>(a)].valueHash > mats[static_cast<size_t>(b)].valueHash) cmp = 1;
                        break;
                    default:
                        break;
                    }
                    return asc ? (cmp < 0) : (cmp > 0);
                });
            spec->SpecsDirty = false;
        }
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_matsFiltered.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const int idx = m_matsFiltered[static_cast<size_t>(row)];
            const auto& e = mats[static_cast<size_t>(idx)];

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", idx);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("0x%016llX", static_cast<unsigned long long>(e.keyHash));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("0x%016llX", static_cast<unsigned long long>(e.valueHash));
        }
    }

    ImGui::EndTable();
}

// ---------------------------------------------------------------------------
// Section 2 tab
// ---------------------------------------------------------------------------

void MatTableViewer::RenderCombosTab() {
    if (ImGui::Checkbox("Hide null markers (paramCount == 0)", &m_hideNullCombos))
        m_combosDirty = true;
    ImGui::SameLine();
    if (ImGui::InputText("Search (hex)##combos", m_combosSearch, sizeof(m_combosSearch)))
        m_combosDirty = true;

    if (m_combosDirty)
        RebuildCombosFilter();

    const std::string filteredStr = FormatWithCommas(m_combosFiltered.size());
    const std::string totalStr    = FormatWithCommas(m_tableData.combos.size());
    ImGui::Text("Showing %s / %s", filteredStr.c_str(), totalStr.c_str());

    ImGui::Separator();

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable;

    if (!ImGui::BeginTable("##Combos", 5, flags, ImVec2(0.0f, 0.0f)))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#",             ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("comboKey",      ImGuiTableColumnFlags_WidthFixed, 180.0f);
    ImGui::TableSetupColumn("paramOffset",   ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("paramCount",    ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("inline params", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    const auto& combos = m_tableData.combos;
    const auto& params = m_tableData.paramIds;

    if (ImGuiTableSortSpecs* spec = ImGui::TableGetSortSpecs()) {
        if (spec->SpecsDirty && spec->SpecsCount > 0) {
            m_combosSortCol = spec->Specs[0].ColumnIndex;
            m_combosSortDir = spec->Specs[0].SortDirection;

            const int col = m_combosSortCol;
            const bool asc = (m_combosSortDir == ImGuiSortDirection_Ascending);
            std::sort(m_combosFiltered.begin(), m_combosFiltered.end(),
                [&combos, col, asc](int a, int b) {
                    int cmp = 0;
                    switch (col) {
                    case 0:
                        cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
                        break;
                    case 1:
                        if (combos[static_cast<size_t>(a)].comboKey < combos[static_cast<size_t>(b)].comboKey) cmp = -1;
                        else if (combos[static_cast<size_t>(a)].comboKey > combos[static_cast<size_t>(b)].comboKey) cmp = 1;
                        break;
                    case 2:
                        if (combos[static_cast<size_t>(a)].paramOffset < combos[static_cast<size_t>(b)].paramOffset) cmp = -1;
                        else if (combos[static_cast<size_t>(a)].paramOffset > combos[static_cast<size_t>(b)].paramOffset) cmp = 1;
                        break;
                    case 3:
                        cmp = (combos[static_cast<size_t>(a)].paramCount < combos[static_cast<size_t>(b)].paramCount) ? -1 :
                              (combos[static_cast<size_t>(a)].paramCount > combos[static_cast<size_t>(b)].paramCount) ?  1 : 0;
                        break;
                    default:
                        break;
                    }
                    return asc ? (cmp < 0) : (cmp > 0);
                });
            spec->SpecsDirty = false;
        }
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_combosFiltered.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const int idx = m_combosFiltered[static_cast<size_t>(row)];
            const auto& c = combos[static_cast<size_t>(idx)];

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", idx);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("0x%016llX", static_cast<unsigned long long>(c.comboKey));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u", c.paramOffset);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u", c.paramCount);
            ImGui::TableSetColumnIndex(4);

            if (c.paramCount == 0) {
                ImGui::TextDisabled("(null marker)");
                continue;
            }

            const size_t endIdx = static_cast<size_t>(c.paramOffset) + c.paramCount;
            if (endIdx > params.size()) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                    "(out of range: offset %u + count %u > %zu)",
                    c.paramOffset, c.paramCount, params.size());
                continue;
            }

            // Inline preview — cap long lists, full dump available via tooltip.
            constexpr size_t kMaxShow = 32;
            const size_t show = std::min<size_t>(c.paramCount, kMaxShow);
            std::string preview;
            for (size_t i = 0; i < show; ++i) {
                char tmp[16];
                std::snprintf(tmp, sizeof(tmp), "%u",
                              static_cast<unsigned>(params[c.paramOffset + i]));
                if (i > 0) preview += ", ";
                preview += tmp;
            }
            if (c.paramCount > kMaxShow) preview += ", ...";
            ImGui::TextUnformatted(preview.c_str());

            if (c.paramCount > kMaxShow && ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("All %u params:", c.paramCount);
                ImGui::Separator();
                std::string full;
                for (uint32_t i = 0; i < c.paramCount; ++i) {
                    char tmp[16];
                    std::snprintf(tmp, sizeof(tmp), "%u",
                                  static_cast<unsigned>(params[c.paramOffset + i]));
                    if (i > 0) full += (i % 16 == 0) ? ",\n" : ", ";
                    full += tmp;
                }
                ImGui::TextUnformatted(full.c_str());
                ImGui::EndTooltip();
            }
        }
    }

    ImGui::EndTable();
}

// ---------------------------------------------------------------------------
// Section 3 tab
// ---------------------------------------------------------------------------

void MatTableViewer::RenderParamsTab() {
    const std::string totalStr = FormatWithCommas(m_tableData.paramIds.size());
    ImGui::Text("%s u16 parameter IDs", totalStr.c_str());
    ImGui::TextDisabled("Referenced by Graph combos via (paramOffset, paramCount).");
    ImGui::Separator();

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY;

    if (!ImGui::BeginTable("##Params", 2, flags, ImVec2(0.0f, 0.0f)))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Offset",  ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("paramId", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    const auto& params = m_tableData.paramIds;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(params.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", row);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", static_cast<unsigned>(params[static_cast<size_t>(row)]));
        }
    }

    ImGui::EndTable();
}

} // namespace GGPKExp
