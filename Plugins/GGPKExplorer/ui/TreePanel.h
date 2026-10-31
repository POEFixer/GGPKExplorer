#pragma once
#include "../core/VirtualTree.h"
#include "SaveFormat.h"
#include <string>
#include <cstdint>
#include <set>
#include <vector>
#include <functional>

namespace GGPKExp {

class TreePanel {
public:
    using SelectCallback = std::function<void(const TreeEntry& entry)>;

    void SetTree(VirtualTree* tree) { m_tree = tree; }
    void SetOnSelect(SelectCallback cb) { m_onSelect = std::move(cb); }
    void Render(float width);

    const std::string& GetSelectedPath() const { return m_selectedPath; }
    void SetSelectedPath(const std::string& path);
    std::vector<std::string> GetSelectedPaths() const;
    /// Nothing selected (the archive closed: the indices named entries of its tree).
    void ClearSelection();

    /// Request that the next render scroll to the currently selected path and
    /// flash-highlight that row for ~1.5 seconds. Call AFTER SetSelectedPath().
    void RevealSelected();
    bool HasMultipleSelected() const { return m_selectedIndices.size() > 1; }
    bool HasAnyDirSelected() const;
    bool HasAnyFileSelected() const;
    const std::set<int>& GetSelectedIndices() const { return m_selectedIndices; }

    bool wantsExtractFile = false;
    bool wantsReplaceFile = false;
    bool wantsCopyPath = false;
    bool wantsCopyName = false;
    bool wantsRemoveBookmark = false;
    bool wantsAddBookmark = false;
    int bookmarkIconChoice = 0;
    bool wantsExtractDir = false;
    bool wantsExtractSelectedFiles = false;
    bool wantsExpandAll = false;
    bool wantsCollapseAll = false;
    bool wantsSearch = false;
    bool wantsSearchFilesTab = false;
    bool wantsSaveAs = false;
    SaveFormat saveAsFormat = SaveFormat::Json;   // valid only when wantsSaveAs

private:
    VirtualTree* m_tree = nullptr;  // non-owning, lifetime guaranteed by GGPKExplorer
    SelectCallback m_onSelect;
    std::string m_selectedPath;
    std::set<int> m_selectedIndices;
    int m_lastClickedIndex = -1;
    uint32_t m_contextMenuIndex = UINT32_MAX;

    int    m_pendingRevealRow = -1;   // flat index to scroll to next render, -1 = none
    int    m_highlightRow     = -1;   // row currently highlighted, -1 = none
    double m_highlightEndTime = 0.0;  // ImGui::GetTime() epoch when highlight ends

    void RenderContextMenu();
};

} // namespace GGPKExp
