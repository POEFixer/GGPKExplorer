#pragma once
#include <string>
#include <deque>
#include <functional>
#include <imgui.h>

namespace GGPKExp {

class NavigationBar {
public:
    using NavigateCallback = std::function<void(const std::string& path)>;
    void SetOnNavigate(NavigateCallback cb) { m_onNavigate = std::move(cb); }
    void Navigate(const std::string& path);
    void Render();
    bool CanGoBack() const { return m_historyPos > 0; }
    bool CanGoForward() const { return m_historyPos + 1 < static_cast<int>(m_history.size()); }
    void GoBack();
    void GoForward();
    const std::string& GetCurrentPath() const { return m_currentPath; }
    void Clear();

private:
    NavigateCallback m_onNavigate;
    std::string m_currentPath;
    std::deque<std::string> m_history;
    int m_historyPos = -1;
    bool m_navigatingFromHistory = false;
    static constexpr int MAX_HISTORY = 100;
};

} // namespace GGPKExp
