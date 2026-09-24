#pragma once
#include "IViewer.h"
#include <string>
#include <vector>
#include <cstdint>
#include <imgui.h>
#include "../core/parsers/MatTableParser.h"

namespace GGPKExp {

class MatTableViewer : public IViewer {
public:
    void LoadData(std::vector<uint8_t> data, const std::string& path) override;
    void Clear() override;
    void Render() override;

private:
    Parsers::MatTableData m_tableData;
    std::string m_filename;

    // -------------------- Section 1: mats --------------------
    char m_matsSearch[128] = {};
    std::vector<int> m_matsFiltered;          // indices into m_tableData.mats
    bool m_matsDirty = true;
    int m_matsSortCol = 0;
    ImGuiSortDirection m_matsSortDir = ImGuiSortDirection_Ascending;

    // -------------------- Section 2: combos --------------------
    char m_combosSearch[128] = {};
    std::vector<int> m_combosFiltered;        // indices into m_tableData.combos
    bool m_combosDirty = true;
    int m_combosSortCol = 0;
    ImGuiSortDirection m_combosSortDir = ImGuiSortDirection_Ascending;
    bool m_hideNullCombos = true;              // hide paramCount == 0 markers

    void RebuildMatsFilter();
    void RebuildCombosFilter();

    void RenderOverview();
    void RenderMatsTab();
    void RenderCombosTab();
    void RenderParamsTab();
};

} // namespace GGPKExp
