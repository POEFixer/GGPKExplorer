#include "QuickJumpDialog.h"
#include <algorithm>
#include <cctype>

namespace GGPKExp {

void QuickJumpDialog::Open() {
    m_showQuickJump = true;
    m_quickJumpBuf[0] = '\0';
    m_quickJumpResults.clear();
    m_quickJumpSelected = 0;
    m_quickJumpNeedsFocus = true;
}

void QuickJumpDialog::UpdateResults(const char* query) {
    m_quickJumpResults.clear();

    if (!query || query[0] == '\0') return;
    if (!m_tree) return;

    // Convert query to lowercase for case-insensitive matching
    std::string queryLower;
    for (const char* p = query; *p; ++p)
        queryLower += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));

    const auto& flatList = m_tree->GetFlatList();

    for (const auto& entry : flatList) {
        if (entry.isDirectory) continue;

        // Convert path to lowercase
        std::string pathLower;
        pathLower.reserve(entry.fullPath.size());
        for (char c : entry.fullPath)
            pathLower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        // Fuzzy match: try to find each query character in order in the path
        int score = 0;
        size_t pathIdx = 0;
        int consecutiveBonus = 0;
        bool matched = true;

        for (size_t qi = 0; qi < queryLower.size(); ++qi) {
            bool found = false;
            while (pathIdx < pathLower.size()) {
                if (pathLower[pathIdx] == queryLower[qi]) {
                    score += 1;
                    // Bonus for consecutive matches
                    if (consecutiveBonus > 0)
                        score += consecutiveBonus;
                    consecutiveBonus++;
                    // Bonus for matching at start of segment (after '/')
                    if (pathIdx == 0 || pathLower[pathIdx - 1] == '/')
                        score += 3;
                    ++pathIdx;
                    found = true;
                    break;
                }
                consecutiveBonus = 0;
                ++pathIdx;
            }
            if (!found) {
                matched = false;
                break;
            }
        }

        if (matched && score > 0) {
            // Bonus for shorter paths (more specific matches rank higher)
            int lengthPenalty = static_cast<int>(entry.fullPath.size()) / 10;
            score = score * 10 - lengthPenalty;

            m_quickJumpResults.push_back({ entry.fullPath, entry.name, score });
        }
    }

    // Sort by score descending
    std::sort(m_quickJumpResults.begin(), m_quickJumpResults.end(),
        [](const QuickJumpResult& a, const QuickJumpResult& b) {
            return a.score > b.score;
        });

    // Keep top 20
    if (m_quickJumpResults.size() > 20)
        m_quickJumpResults.resize(20);
}

void QuickJumpDialog::Render() {
    if (!m_showQuickJump) return;

    ImGui::OpenPopup("Quick Jump##GGPKExp");
    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Quick Jump##GGPKExp", &m_showQuickJump, ImGuiWindowFlags_NoResize)) {
        // Auto-focus input on first frame
        if (m_quickJumpNeedsFocus) {
            ImGui::SetKeyboardFocusHere();
            m_quickJumpNeedsFocus = false;
        }

        bool textChanged = ImGui::InputTextWithHint("##QJInput", "Type to search files...",
                                                     m_quickJumpBuf, sizeof(m_quickJumpBuf));

        if (textChanged) {
            UpdateResults(m_quickJumpBuf);
            m_quickJumpSelected = 0;
        }

        // Handle keyboard navigation in the results
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            m_quickJumpSelected++;
            if (m_quickJumpSelected >= static_cast<int>(m_quickJumpResults.size()))
                m_quickJumpSelected = static_cast<int>(m_quickJumpResults.size()) - 1;
            if (m_quickJumpSelected < 0) m_quickJumpSelected = 0;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            m_quickJumpSelected--;
            if (m_quickJumpSelected < 0) m_quickJumpSelected = 0;
        }

        // Enter: navigate to selected item
        bool enterPressed = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
        if (enterPressed && !m_quickJumpResults.empty() &&
            m_quickJumpSelected >= 0 && m_quickJumpSelected < static_cast<int>(m_quickJumpResults.size()))
        {
            if (m_onNavigate) {
                m_onNavigate(m_quickJumpResults[static_cast<size_t>(m_quickJumpSelected)].path);
            }
            m_showQuickJump = false;
            ImGui::CloseCurrentPopup();
        }

        // Escape handled by ImGui modal close (m_showQuickJump = false)

        ImGui::Separator();

        // Results list
        ImGui::BeginChild("##QJResults", ImVec2(0, 0), ImGuiChildFlags_None);
        for (int i = 0; i < static_cast<int>(m_quickJumpResults.size()); ++i) {
            const auto& r = m_quickJumpResults[static_cast<size_t>(i)];
            bool isSelected = (i == m_quickJumpSelected);

            ImGui::PushID(i);
            if (ImGui::Selectable("##QJSel", isSelected, ImGuiSelectableFlags_None, ImVec2(0, 0))) {
                if (m_onNavigate) {
                    m_onNavigate(r.path);
                }
                m_showQuickJump = false;
                ImGui::CloseCurrentPopup();
                ImGui::PopID();
                break;
            }
            // Draw text on top of Selectable
            ImGui::SameLine(0, 0);
            ImGui::Text("%s", r.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", r.path.c_str());
            ImGui::PopID();

            // Ensure selected item is visible
            if (isSelected && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow))) {
                ImGui::SetScrollHereY(0.5f);
            }
        }

        if (m_quickJumpResults.empty() && m_quickJumpBuf[0] != '\0') {
            ImGui::TextDisabled("No results found");
        } else if (m_quickJumpBuf[0] == '\0') {
            ImGui::TextDisabled("Start typing to search...");
        }

        ImGui::EndChild();
        ImGui::EndPopup();
    }
}

} // namespace GGPKExp
