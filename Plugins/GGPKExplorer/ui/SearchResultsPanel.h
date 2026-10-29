// Plugins/GGPKExplorer/ui/SearchResultsPanel.h
#pragma once
#include "../core/SearchTypes.h"
#include "UiText.h"     // SizeText, translated texts
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
    // The header's two counts, drawn in the program language by Render (per mode: files and
    // folders found, matches and files searched, occurrences and files replaced).
    int m_headerFirst = 0;
    int m_headerSecond = 0;

    // Expand/Collapse all (one-shot flags)
    int m_forceExpandState = 0;  // 0=none, 1=expand all, -1=collapse all

    void RenderFileResults();
    void RenderContentResults();
    void RenderReplaceLog();
};

} // namespace GGPKExp
