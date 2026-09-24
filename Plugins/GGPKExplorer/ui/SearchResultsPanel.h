// Plugins/GGPKExplorer/ui/SearchResultsPanel.h
#pragma once
#include "../core/SearchTypes.h"
#include "StatusBar.h"  // FormatSize
#include <string>
#include <vector>
#include <functional>

namespace GGPKExp {

class SearchResultsPanel {
public:
    using NavigateCallback = std::function<void(const std::string& path, int line, bool isDatRow)>;

    void SetOnNavigate(NavigateCallback cb) { m_onNavigate = std::move(cb); }

    // Show results from a search
    void ShowSearchResults(const SearchResult& result);
    // Show replace log
    void ShowReplaceLog(const std::vector<ReplaceResult>& results);
    // Clear
    void Clear();

    bool IsVisible() const { return m_visible; }
    float GetHeight() const;
    void Render();

private:
    NavigateCallback m_onNavigate;
    bool m_visible = false;
    bool m_collapsed = false;
    float m_height = 200.0f;

    // Results data
    enum class Mode { None, FileSearch, ContentSearch, ReplaceLog };
    Mode m_mode = Mode::None;
    SearchResult m_result;
    std::vector<ReplaceResult> m_replaceLog;
    std::string m_headerText;

    // Expand/Collapse all (one-shot flags)
    int m_forceExpandState = 0;  // 0=none, 1=expand all, -1=collapse all

    void RenderFileResults();
    void RenderContentResults();
    void RenderReplaceLog();
};

} // namespace GGPKExp
