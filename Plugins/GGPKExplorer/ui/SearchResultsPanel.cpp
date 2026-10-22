// Plugins/GGPKExplorer/ui/SearchResultsPanel.cpp
#include "SearchResultsPanel.h"
#include <imgui.h>
#include <cstdio>

namespace GGPKExp {

void SearchResultsPanel::ShowSearchResults(const SearchResult& result) {
    m_result = result;
    m_replaceLog.clear();
    m_visible = true;
    m_collapsed = false;

    if (!result.fileMatches.empty()) {
        m_mode = Mode::FileSearch;
        int dirs = 0, files = 0;
        for (const auto& fm : result.fileMatches) {
            if (fm.isDirectory) dirs++; else files++;
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "Found %d file(s), %d folder(s)", files, dirs);
        m_headerText = buf;
    } else {
        m_mode = Mode::ContentSearch;
        char buf[128];
        snprintf(buf, sizeof(buf), "Search Results \xe2\x80\x94 %d match(es) in %d file(s)",
                 result.totalMatches, result.totalFiles);
        m_headerText = buf;
    }
}

void SearchResultsPanel::ShowReplaceLog(const std::vector<ReplaceResult>& results) {
    m_replaceLog = results;
    m_result = SearchResult{};
    m_mode = Mode::ReplaceLog;
    m_visible = true;
    m_collapsed = false;

    int total = 0;
    for (const auto& r : results) total += r.count;
    char buf[128];
    snprintf(buf, sizeof(buf), "Replaced %d occurrence(s) in %d file(s)",
             total, static_cast<int>(results.size()));
    m_headerText = buf;
}

void SearchResultsPanel::Clear() {
    m_visible = false;
    m_mode = Mode::None;
    m_result = SearchResult{};
    m_replaceLog.clear();
    m_headerText.clear();
}

float SearchResultsPanel::GetHeight() const {
    if (!m_visible) return 0.0f;
    if (m_collapsed) return ImGui::GetFrameHeightWithSpacing() + 4.0f;
    return m_height;
}

void SearchResultsPanel::Render() {
    if (!m_visible) return;

    // Drag splitter for resizing
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.6f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.6f, 0.7f, 1.0f));
    ImGui::Button("##ResultsSplitter", ImVec2(-1, 3.0f));
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemActive()) {
        m_height -= ImGui::GetIO().MouseDelta.y;
        if (m_height < 60.0f) m_height = 60.0f;
        if (m_height > 600.0f) m_height = 600.0f;
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    // Header bar (no child window — render inline for correct layout)
    {
        float indent = 8.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
        ImGui::Spacing();

        const char* icon = (m_mode == Mode::ReplaceLog) ? "\xe2\x9c\x85 " : "\xef\x80\x82 ";
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "%s%s", icon, m_headerText.c_str());

        ImGui::SameLine();
        // Right-align buttons
        float btnWidth = ImGui::CalcTextSize("\xef\x81\xb7").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float totalBtns = btnWidth * 4 + ImGui::GetStyle().ItemSpacing.x * 3;
        float windowRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        ImGui::SameLine(windowRight - totalBtns);

        // Expand All
        if (ImGui::SmallButton("\xef\x81\xbc")) {
            m_forceExpandState = 1;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Expand All");
        ImGui::SameLine();

        // Collapse All
        if (ImGui::SmallButton("\xef\x85\x86")) {
            m_forceExpandState = -1;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Collapse All");
        ImGui::SameLine();

        // Panel collapse
        if (ImGui::SmallButton(m_collapsed ? "\xef\x81\xb8" : "\xef\x81\xb7")) {
            m_collapsed = !m_collapsed;
        }
        ImGui::SameLine();
        // Close
        if (ImGui::SmallButton("\xef\x80\x8d")) {
            Clear();
        }
    }

    if (m_collapsed) return;

    // Results list
    float headerH = ImGui::GetFrameHeightWithSpacing();
    float listH = m_height - headerH - 8.0f;
    if (listH < 30.0f) listH = 30.0f;

    ImGui::BeginChild("##ResultsList", ImVec2(0, listH), ImGuiChildFlags_Borders);

    switch (m_mode) {
    case Mode::FileSearch:    RenderFileResults(); break;
    case Mode::ContentSearch: RenderContentResults(); break;
    case Mode::ReplaceLog:    RenderReplaceLog(); break;
    default: break;
    }

    ImGui::EndChild();
}

void SearchResultsPanel::RenderFileResults() {
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_result.fileMatches.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto& fm = m_result.fileMatches[i];
            ImGui::PushID(i);

            const char* icon = fm.isDirectory ? "\xef\x81\xbc " : "\xef\x85\x9c ";
            ImVec4 color = fm.isDirectory
                ? ImVec4(0.3f, 0.6f, 1.0f, 1.0f)
                : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::PushStyleColor(ImGuiCol_Text, color);

            char label[512];
            if (fm.isDirectory) {
                snprintf(label, sizeof(label), "%s%s/", icon, fm.fullPath.c_str());
            } else {
                char sizeBuf[64];
                FormatSize(static_cast<int64_t>(fm.fileSize), sizeBuf, sizeof(sizeBuf));
                snprintf(label, sizeof(label), "%s%s  (%s)", icon, fm.fullPath.c_str(), sizeBuf);
            }

            if (ImGui::Selectable(label)) {
                if (m_onNavigate)
                    m_onNavigate(fm.fullPath, -1, false);
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
}

void SearchResultsPanel::RenderContentResults() {
    for (size_t fi = 0; fi < m_result.fileResults.size(); ++fi) {
        const auto& fsr = m_result.fileResults[fi];
        ImGui::PushID(static_cast<int>(fi));

        // File header as TreeNode
        const char* icon = fsr.isDatc64 ? "\xef\x83\x8e " : "\xef\x85\x9c ";
        int matchCount = static_cast<int>(fsr.contentMatches.size() + fsr.datMatches.size());
        char fileLabel[512];
        snprintf(fileLabel, sizeof(fileLabel), "%s%s (%d match%s)",
                 icon, fsr.fullPath.c_str(), matchCount, matchCount == 1 ? "" : "es");

        // Apply force expand/collapse
        if (m_forceExpandState != 0) {
            ImGui::SetNextItemOpen(m_forceExpandState > 0);
        }

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
        bool nodeOpen = ImGui::TreeNodeEx(fileLabel, ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor();

        if (nodeOpen) {
            // Content matches
            for (size_t mi = 0; mi < fsr.contentMatches.size(); ++mi) {
                const auto& cm = fsr.contentMatches[mi];
                ImGui::PushID(static_cast<int>(mi));

                std::string lineText = cm.lineText;
                if (lineText.size() > 200) lineText = lineText.substr(0, 200) + "...";

                char label[1024];
                snprintf(label, sizeof(label), "  Line %d: %s", cm.lineNumber, lineText.c_str());

                if (ImGui::Selectable(label)) {
                    if (m_onNavigate)
                        m_onNavigate(cm.fullPath, cm.lineNumber, false);
                }

                ImGui::PopID();
            }

            // Dat matches
            for (size_t mi = 0; mi < fsr.datMatches.size(); ++mi) {
                const auto& dm = fsr.datMatches[mi];
                ImGui::PushID(static_cast<int>(fsr.contentMatches.size() + mi));

                char label[1024];
                snprintf(label, sizeof(label), "  Row %d, \"%s\": %s",
                         dm.row, dm.columnName.c_str(), dm.cellValue.c_str());

                if (ImGui::Selectable(label)) {
                    if (m_onNavigate)
                        m_onNavigate(dm.fullPath, dm.row, true);
                }

                ImGui::PopID();
            }

            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    // Reset one-shot flag after all nodes rendered
    m_forceExpandState = 0;
}

void SearchResultsPanel::RenderReplaceLog() {
    for (size_t i = 0; i < m_replaceLog.size(); ++i) {
        const auto& r = m_replaceLog[i];
        ImGui::PushID(static_cast<int>(i));

        char label[512];
        snprintf(label, sizeof(label), "\xef\x85\x9c %s \xe2\x80\x94 %d replacement(s)",
                 r.path.c_str(), r.count);
        if (ImGui::Selectable(label)) {
            if (m_onNavigate)
                m_onNavigate(r.path, -1, false);
        }

        ImGui::PopID();
    }
}

} // namespace GGPKExp
