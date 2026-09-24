#pragma once
#include "IViewer.h"
#include "../core/parsers/MtpParser.h"
#include <imgui.h>
#include <set>
#include <string>
#include <vector>

namespace GGPKExp {

class MtpViewer : public IViewer {
public:
    void LoadData(std::vector<uint8_t> data, const std::string& path) override {
        auto parseResult = Parsers::ParseMtp(data.data(), data.size());
        m_filename = path;
        m_error.clear();
        m_loaded = true;
        if (!parseResult) {
            m_error = parseResult.error;
            m_mtpData = {};
            return;
        }
        m_mtpData = std::move(parseResult.value);
    }

    void Clear() override { m_mtpData = {}; m_loaded = false; m_filename.clear(); pendingNavPath.clear(); m_error.clear(); }

    // Navigation support
    bool HasPendingNavigation() const override { return !pendingNavPath.empty(); }
    std::string TakePendingNavigation() override {
        std::string p = std::move(pendingNavPath);
        pendingNavPath.clear();
        return p;
    }

    void Render() override {
        if (!m_loaded) { ImGui::TextDisabled("No data loaded"); return; }
        if (!m_error.empty()) {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Parse error: %s", m_error.c_str());
            return;
        }

        // Header info section
        ImGui::SeparatorText("Header");
        if (ImGui::BeginTable("##MtpHeader", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg,
                              ImVec2(400, 0))) {
            ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Version");
            ImGui::TableNextColumn(); ImGui::Text("%u", m_mtpData.version);

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Entry Count");
            ImGui::TableNextColumn(); ImGui::Text("%u", m_mtpData.entryCount);

            // Count unique tile IDs
            {
                std::set<uint32_t> uniqueTiles;
                for (const auto& e : m_mtpData.entries)
                    uniqueTiles.insert(e.tileId);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted("Unique Tile IDs");
                ImGui::TableNextColumn(); ImGui::Text("%u", static_cast<uint32_t>(uniqueTiles.size()));
            }

            ImGui::EndTable();
        }

        // Referenced files (clickable links)
        ImGui::SeparatorText("Referenced Files");
        if (!m_mtpData.minimapPath.empty()) {
            ImGui::Text("Minimap:");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
            if (ImGui::SmallButton(m_mtpData.minimapPath.c_str()))
                pendingNavPath = m_mtpData.minimapPath;
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("Minimap: (none found)");
        }

        if (!m_mtpData.terrainDataPath.empty()) {
            ImGui::Text("Terrain:");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
            if (ImGui::SmallButton(m_mtpData.terrainDataPath.c_str()))
                pendingNavPath = m_mtpData.terrainDataPath;
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("Terrain: (none found)");
        }

        // Tile entries table
        if (!m_mtpData.entries.empty()) {
            ImGui::SeparatorText("Tile Entries");
            if (ImGui::BeginTable("##MtpEntries", 8,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable,
                ImVec2(0, ImGui::GetContentRegionAvail().y)))
            {
                ImGui::TableSetupColumn("Tile ID",   ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn("Variant",    ImGuiTableColumnFlags_WidthFixed, 55);
                ImGui::TableSetupColumn("Width",      ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn("Height",     ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn("Atlas X",    ImGuiTableColumnFlags_WidthFixed, 55);
                ImGui::TableSetupColumn("Atlas Y",    ImGuiTableColumnFlags_WidthFixed, 55);
                ImGui::TableSetupColumn("Atlas XEnd", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Atlas YEnd", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableHeadersRow();
                ImGui::TableSetupScrollFreeze(0, 1);

                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(m_mtpData.entries.size()));
                while (clipper.Step()) {
                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                        const auto& e = m_mtpData.entries[row];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::Text("%u", e.tileId);
                        ImGui::TableNextColumn(); ImGui::Text("%u", e.variant);
                        ImGui::TableNextColumn(); ImGui::Text("%u", e.tileWidth);
                        ImGui::TableNextColumn(); ImGui::Text("%u", e.pixelHeight());
                        ImGui::TableNextColumn(); ImGui::Text("%u", e.atlasX);
                        ImGui::TableNextColumn(); ImGui::Text("%u", e.atlasY);
                        ImGui::TableNextColumn(); ImGui::Text("%u", e.atlasXEnd);
                        ImGui::TableNextColumn(); ImGui::Text("%u", e.atlasYEnd);
                    }
                }
                ImGui::EndTable();
            }
        }
    }

    // Navigation: set by Render(), consumed by ContentPanel
    std::string pendingNavPath;

private:
    Parsers::MtpData m_mtpData;
    std::string m_filename;
};

} // namespace GGPKExp
