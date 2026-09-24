#pragma once
#include "../core/VirtualTree.h"
#include <string>
#include <functional>
#include <imgui.h>

namespace GGPKExp {

class BookmarkManager {
public:
    using NavigateCallback = std::function<void(const std::string&)>;

    void SetOnNavigate(NavigateCallback cb) { m_onNavigate = std::move(cb); }

    void Open() { m_showBookmarkManager = true; }
    void Close() { m_showBookmarkManager = false; }
    bool IsOpen() const { return m_showBookmarkManager; }

    /// Render the bookmark manager window.
    /// Returns a non-empty path if the user requested navigation (Go button).
    std::string Render(VirtualTree& tree);

private:
    bool m_showBookmarkManager = false;
    bool m_bookmarkClearConfirm = false;
    char m_bookmarkFilter[256] = {};
    NavigateCallback m_onNavigate;
};

} // namespace GGPKExp
