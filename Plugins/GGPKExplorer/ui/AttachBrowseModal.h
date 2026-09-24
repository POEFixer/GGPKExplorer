#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ImGuiTableSortSpecs;
struct ID3D11Device;

namespace GGPKExp {

class GGPKManager;
class VirtualTree;
class DatSchema;

namespace Aoc {
class ItemVisualIdentityCatalog;
class ItemIconCache;
} // namespace Aoc

// Phase 5.1.5a — Modal dialog for browsing and attaching .sm / .aoc files
// to an AOC socket. Opened from Sockets tab right-click menu. The caller
// (AocViewer) supplies a callback invoked on Attach-button click.
class AttachBrowseModal {
public:
    // Signature: void(const std::string& socket,
    //                 const std::string& sourcePath,
    //                 const std::vector<uint8_t>& sourceBytes).
    // Callback is responsible for all error reporting (toasts, logging).
    // Modal closes after callback returns.
    using AttachCallback = std::function<void(const std::string&,
                                              const std::string&,
                                              const std::vector<uint8_t>&)>;

    void SetManager(GGPKManager* mgr) { m_mgr = mgr; }
    void SetTree(VirtualTree* tree)   { m_tree = tree; }
    void SetSchema(DatSchema* schema)              { m_schema = schema; }
    void SetParentAocPath(const std::string& p)    { m_parentAocPath = p; }
    void SetDevice(ID3D11Device* device)           { m_device = device; }

    AttachBrowseModal();
    ~AttachBrowseModal();

    // Open the modal targeted at a specific socket. Safe to call repeatedly;
    // re-opens with fresh filter/selection state. Triggers file-cache rebuild
    // on first open of the session (or if tree/manager changed).
    void Open(const std::string& socketName, AttachCallback onAttach);
    void Close();
    bool IsOpen() const { return m_showModal; }

    // Call every frame from AocViewer::Render(). No-op if not open.
    void Render();

private:
    enum class Tab : int { Files = 0, ByItemName = 1 };
    Tab m_activeTab = Tab::Files;

    void RenderFilesTab();              // extracted from Render() in Task 2 Step 2

    // --- DAT tab ("By Item Name") state ------------------------------------
    enum class DatLoadState { Idle, Loading, Ready, Failed };

    void RenderDatTab();
    void RenderDatFilterRow();
    void RenderDatResultsTable();
    void RenderDatSelectionPanel();
    void RenderDatButtonsRow();

    void EnsureDatCatalog();
    void ApplyDatFilter();
    void ApplyDatSort(ImGuiTableSortSpecs* specs);
    void OnDatRowClicked(int filteredRow);
    void TriggerDatAttach();

    void RebuildFileCache();
    void ApplyFilter();
    void TriggerAttach();

    GGPKManager* m_mgr  = nullptr;      // non-owning
    VirtualTree* m_tree = nullptr;      // non-owning

    bool                m_showModal     = false;
    bool                m_needsFocus    = false;
    std::string         m_targetSocket;
    AttachCallback      m_onAttach;

    char                m_filterBuf[256] = {};
    bool                m_showSm        = true;
    bool                m_showAoc       = true;

    // Cached on first Open(); rebuilt if m_tree pointer changes.
    const VirtualTree*  m_cachedTreePtr = nullptr;
    std::vector<std::string> m_allMeshPaths;

    // Indices into m_allMeshPaths passing the current filter.
    std::vector<std::size_t> m_filteredIndices;

    int                 m_selectedIndex = -1;  // Index into m_filteredIndices, -1 = none.
    std::string         m_lastError;           // Inline error shown in modal on ReadFile failure.

    // --- DAT tab ("By Item Name") state ------------------------------------
    DatSchema*                                              m_schema = nullptr;
    std::string                                             m_parentAocPath;
    std::unique_ptr<GGPKExp::Aoc::ItemVisualIdentityCatalog> m_catalog;
    const GGPKManager*                                      m_cachedMgrPtrForDat = nullptr;

    DatLoadState                                            m_datLoadState = DatLoadState::Idle;
    std::string                                             m_datLoadError;
    std::string                                             m_datLastError;

    char                                                    m_datFilterBuf[256] = {};
    std::vector<std::size_t>                                m_datFilteredIndices;
    int                                                     m_datSelectedIndex   = -1;
    int                                                     m_datSelectedFileIdx = -1;
    bool                                                    m_datNeedsFocus = false;

    // Phase 5.1.5c — Device handle + icon cache for thumbnails.
    ID3D11Device*                                               m_device    = nullptr;
    std::unique_ptr<GGPKExp::Aoc::ItemIconCache>                m_iconCache;
};

} // namespace GGPKExp
