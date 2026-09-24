#include "TerrainViewer.h"
#include <algorithm>
#include <cstdio>

namespace GGPKExp {

void TerrainViewer::Clear() {
    m_tdt = {};
    m_loaded = false;
    m_error.clear();
    m_pendingNav.clear();
}

void TerrainViewer::LoadData(std::vector<uint8_t> data, const std::string& /*path*/) {
    m_error.clear();
    m_loaded = false;
    m_pendingNav.clear();

    if (data.empty()) {
        m_error = "Empty file data";
        return;
    }

    auto result = Parsers::ParseTdt(data.data(), data.size());
    if (!result) {
        m_error = result.error;
        return;
    }

    m_tdt = std::move(result.value);
    m_loaded = true;
}

void TerrainViewer::RenderPathLink(const char* label, const std::string& path) {
    ImGui::Text("%s", label);
    ImGui::SameLine();
    if (!path.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
        if (ImGui::SmallButton(path.c_str()))
            m_pendingNav = path;
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("(none)");
    }
}

void TerrainViewer::Render() {
    if (!m_loaded) { ImGui::TextDisabled("No data loaded"); return; }
    if (!m_error.empty()) {
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Parse error: %s", m_error.c_str());
        return;
    }

    // -- Header --
    ImGui::SeparatorText("Header");
    if (ImGui::BeginTable("##TdtHeader", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg,
                          ImVec2(450, 0))) {
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Version");
        ImGui::TableNextColumn(); ImGui::Text("%u", m_tdt.version);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("File Size");
        ImGui::TableNextColumn(); ImGui::Text("%u bytes", static_cast<uint32_t>(m_tdt.rawData.size()));

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Grid Size");
        ImGui::TableNextColumn(); ImGui::Text("%u x %u", m_tdt.gridWidth, m_tdt.gridHeight);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Tile Size");
        ImGui::TableNextColumn(); ImGui::Text("%u x %u", m_tdt.tileWidth, m_tdt.tileHeight);

        ImGui::EndTable();
    }

    // -- Referenced Files --
    ImGui::SeparatorText("Referenced Files");
    RenderPathLink("Geometry:", m_tdt.tgtPath);
    if (!m_tdt.tileName.empty()) {
        ImGui::Text("Name: %s", m_tdt.tileName.c_str());
    }
    RenderPathLink("Ground Type:", m_tdt.groundTypePath);

    // -- Ground Type Refs --
    ImGui::SeparatorText("Ground Type References");
    if (ImGui::BeginTable("##TdtGround", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg,
                          ImVec2(300, 0))) {
        ImGui::TableSetupColumn("Variant", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int i = 0; i < 4; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", i);
            ImGui::TableNextColumn();
            if (m_tdt.groundTypeRefs[i] == 0xFFFFFFFF)
                ImGui::TextDisabled("none (0xFFFFFFFF)");
            else
                ImGui::Text("%u (0x%X)", m_tdt.groundTypeRefs[i], m_tdt.groundTypeRefs[i]);
        }
        ImGui::EndTable();
    }

    // -- Edge Data --
    ImGui::SeparatorText("Edge Data");
    {
        char buf[128];
        int pos = 0;
        for (int i = 0; i < 14 && pos < 120; ++i) {
            if (i > 0) buf[pos++] = ' ';
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X", m_tdt.edgeData[i]);
        }
        buf[pos] = 0;
        ImGui::TextUnformatted(buf);
    }

    // -- Raw Data Hex View --
    if (m_tdt.dataSectionOffset > 0 && m_tdt.dataSectionOffset < m_tdt.rawData.size()) {
        ImGui::SeparatorText("Data Section (hex)");

        size_t dataStart = m_tdt.dataSectionOffset;
        size_t dataSize = m_tdt.rawData.size() - dataStart;
        ImGui::Text("Offset: 0x%X | Size: %u bytes", static_cast<uint32_t>(dataStart),
                    static_cast<uint32_t>(dataSize));

        ImGui::BeginChild("##TdtHex", ImVec2(0, ImGui::GetContentRegionAvail().y), false,
                          ImGuiWindowFlags_HorizontalScrollbar);

        constexpr int bytesPerRow = 16;
        int totalRows = static_cast<int>((dataSize + bytesPerRow - 1) / bytesPerRow);

        ImGuiListClipper clipper;
        clipper.Begin(totalRows);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                size_t rowOffset = static_cast<size_t>(row) * bytesPerRow;
                size_t absOffset = dataStart + rowOffset;
                int count = static_cast<int>((std::min)(static_cast<size_t>(bytesPerRow), dataSize - rowOffset));

                // Address
                char line[256];
                int p = snprintf(line, sizeof(line), "%06X  ", static_cast<uint32_t>(absOffset));

                // Hex bytes
                for (int i = 0; i < bytesPerRow; ++i) {
                    if (i == 8) line[p++] = ' ';
                    if (i < count)
                        p += snprintf(line + p, sizeof(line) - p, "%02X ", m_tdt.rawData[dataStart + rowOffset + i]);
                    else
                        p += snprintf(line + p, sizeof(line) - p, "   ");
                }

                // ASCII
                line[p++] = ' ';
                line[p++] = '|';
                for (int i = 0; i < count; ++i) {
                    uint8_t b = m_tdt.rawData[dataStart + rowOffset + i];
                    line[p++] = (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.';
                }
                line[p++] = '|';
                line[p] = 0;

                ImGui::TextUnformatted(line);
            }
        }

        ImGui::EndChild();
    }
}

} // namespace GGPKExp
