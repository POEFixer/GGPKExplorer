#include "NavigationBar.h"
#include <vector>
#include <sstream>

namespace GGPKExp {

// ---------------------------------------------------------------------------
// Navigate — push a new path onto the history stack
// ---------------------------------------------------------------------------

void NavigationBar::Navigate(const std::string& path) {
    // If we got here from an external caller (not from GoBack/GoForward),
    // clear forward history and push the new path.
    if (!m_navigatingFromHistory) {
        // Erase everything after current position (forward stack)
        if (m_historyPos + 1 < static_cast<int>(m_history.size())) {
            m_history.erase(m_history.begin() + (m_historyPos + 1), m_history.end());
        }

        m_history.push_back(path);

        // Cap history size
        while (static_cast<int>(m_history.size()) > MAX_HISTORY) {
            m_history.pop_front();
        }

        m_historyPos = static_cast<int>(m_history.size()) - 1;
    }

    m_currentPath = path;
}

// ---------------------------------------------------------------------------
// GoBack
// ---------------------------------------------------------------------------

void NavigationBar::GoBack() {
    if (!CanGoBack()) return;

    --m_historyPos;
    m_navigatingFromHistory = true;
    m_currentPath = m_history[static_cast<size_t>(m_historyPos)];
    if (m_onNavigate) {
        m_onNavigate(m_currentPath);
    }
    m_navigatingFromHistory = false;
}

// ---------------------------------------------------------------------------
// GoForward
// ---------------------------------------------------------------------------

void NavigationBar::GoForward() {
    if (!CanGoForward()) return;

    ++m_historyPos;
    m_navigatingFromHistory = true;
    m_currentPath = m_history[static_cast<size_t>(m_historyPos)];
    if (m_onNavigate) {
        m_onNavigate(m_currentPath);
    }
    m_navigatingFromHistory = false;
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

void NavigationBar::Clear() {
    m_currentPath.clear();
    m_history.clear();
    m_historyPos = -1;
    m_navigatingFromHistory = false;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void NavigationBar::Render() {
    // Back button
    if (!CanGoBack()) ImGui::BeginDisabled();
    if (ImGui::SmallButton("<##NavBack")) {
        GoBack();
    }
    if (!CanGoBack()) ImGui::EndDisabled();

    ImGui::SameLine(0, 4);

    // Forward button
    if (!CanGoForward()) ImGui::BeginDisabled();
    if (ImGui::SmallButton(">##NavFwd")) {
        GoForward();
    }
    if (!CanGoForward()) ImGui::EndDisabled();

    ImGui::SameLine(0, 8);

    // Breadcrumb trail
    if (m_currentPath.empty()) {
        ImGui::TextDisabled("(no selection)");
        return;
    }

    // Split current path by '/'
    std::vector<std::string> segments;
    {
        std::istringstream stream(m_currentPath);
        std::string segment;
        while (std::getline(stream, segment, '/')) {
            if (!segment.empty())
                segments.push_back(segment);
        }
    }

    if (segments.empty()) {
        ImGui::TextDisabled("(root)");
        return;
    }

    // Render each segment as a clickable SmallButton, except the last one
    // which is rendered as plain text.
    std::string accumulated;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (!accumulated.empty())
            accumulated += '/';
        accumulated += segments[i];

        if (i > 0) {
            ImGui::SameLine(0, 0);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0, 0);
        }

        if (i + 1 < segments.size()) {
            // Clickable breadcrumb segment
            char btnId[512];
            snprintf(btnId, sizeof(btnId), "%s##bc%d", segments[i].c_str(), static_cast<int>(i));
            if (ImGui::SmallButton(btnId)) {
                if (m_onNavigate) {
                    m_onNavigate(accumulated);
                }
            }
        } else {
            // Last segment: non-clickable
            ImGui::Text("%s", segments[i].c_str());
        }
    }
}

} // namespace GGPKExp
