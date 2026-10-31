#pragma once
#include <d3d11.h>
#include "../core/FileTypeDetector.h"
#include "../core/GGPKManager.h"
#include "../core/DatSchema.h"
#include "../core/GGPKExplorerSettings.h"
#include "../viewers/IViewer.h"
#include "../viewers/PropertiesViewer.h"
#include "../core/VirtualTree.h"
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <memory>

namespace GGPKExp {

// Forward declarations — concrete viewers are only needed in the .cpp
class HexViewer;
class TextViewer;
class ImageViewer;
class DatViewer;
class AudioViewer;
class TerrainViewer;
class MtpViewer;
class MatTableViewer;
class MeshViewer;
class PsgViewer;

class ContentPanel {
public:
    void SetManager(GGPKManager* mgr) { m_manager = mgr; }
    void SetD3DDevice(ID3D11Device* device);
    void SetVirtualTree(VirtualTree* tree);
    void SetDatSchema(DatSchema* schema);
    void SetTaskRunner(AsyncTaskRunner* runner) { m_taskRunner = runner; }
    // Phase 7: propagate host FA6 atlas availability to DatViewer.
    void SetFA6Available(bool available);

    // Phase 7 / T23: persisted DatViewer CellEditorPanel width. Plugin pushes
    // the loaded value via SetDatEditorPanelWidth (cached until DatViewer is
    // lazily created in InitViewers). GetDatEditorPanelWidth returns either
    // the live viewer width or the cached value if the viewer does not yet
    // exist (e.g. plugin shutdown before any file was opened).
    void  SetDatEditorPanelWidth(float w);
    float GetDatEditorPanelWidth() const;

    /// Call when user selects a new file in the tree.
    void LoadFile(const std::string& path, const std::string& name,
                  int64_t size, FileType type, void* nodePtr);
    void Clear();
    void Render();
    // The DAT viewer's Find references scan is reading the archive (Clear stops it).
    bool IsDatReferenceScanRunning() const;

    void ScrollToLine(int line);
    void ScrollToRow(int row);

    // For DatViewer foreign key navigation
    std::string pendingNavigatePath;
    int pendingNavigateRow = -1;
    std::string navNotFoundPath;    // set by GGPKExplorer when NavigateToPath lookup fails

    // Forwarders to the model viewer (MeshViewer). These cache state until
    // the viewers are lazily created in InitViewers(), then apply it.
    void SetAnimPanelState(GGPKExp::AnimationPanelState* state);
    void SetPlaybackSpeedPtr(float* ptr);
    void SetUiSettings(GGPKExp::GGPKExplorerUiSettings* settings);
    void SetViewSettings(GGPKExp::MeshViewSettings* state);
    bool ConsumeUiSettingsDirty();
    void SetInitialPlaybackSpeed(float s);

private:
    GGPKManager* m_manager = nullptr;  // non-owning, lifetime guaranteed by GGPKExplorer
    std::string m_currentPath;
    std::string m_currentName;
    FileType m_currentType = FileType::Unknown;
    void* m_currentNodePtr = nullptr;
    int64_t m_currentSize = 0;

    std::vector<uint8_t> m_fileData;
    bool m_dataLoaded = false;
    std::string m_loadError;              // non-empty = failed: the reader's (English) detail
    const char* m_loadErrorKey = nullptr; // our own message instead: a lang/ key, drawn translated

    // Fails the load with our own message (m_loadError holds the key, so it reads as failed).
    void SetOwnLoadError(const char* key) { m_loadErrorKey = key; m_loadError = key; }

    FileProperties m_properties;

    // --- Viewer map: FileType -> IViewer (all viewers including MeshViewer) ---
    std::unordered_map<FileType, std::unique_ptr<IViewer>> m_viewers;
    // Per-viewer loaded flags, keyed by FileType (lazy loading per tab)
    std::unordered_map<FileType, bool> m_viewerLoaded;

    VirtualTree* m_tree = nullptr;           // non-owning, lifetime guaranteed by GGPKExplorer
    DatSchema* m_schema = nullptr;          // non-owning, lifetime guaranteed by GGPKExplorer
    AsyncTaskRunner* m_taskRunner = nullptr; // non-owning, lifetime guaranteed by GGPKExplorer
    ID3D11Device* m_device = nullptr;        // non-owning, lifetime guaranteed by host (ImGui D3D device)
    bool m_fa6Available = false;             // Phase 7 gate: cached until DatViewer is lazily created
    float m_cachedDatEditorPanelWidth = 400.0f; // Phase 7 / T23: cached until DatViewer exists

    bool m_viewersInitialized = false;
    bool m_isLikelyText = false;
    bool m_isLoading = false;

    // Cached animation settings; applied to viewers when they exist (either
    // immediately if initialized, or via InitViewers() on first creation).
    GGPKExp::AnimationPanelState* m_animPanelState = nullptr;
    GGPKExp::MeshViewSettings*    m_viewSettings = nullptr;
    float m_initialPlaybackSpeed = 1.0f;
    bool m_hasInitialPlaybackSpeed = false;
    float* m_playbackSpeedPtr = nullptr;
    GGPKExp::GGPKExplorerUiSettings* m_uiSettings = nullptr;

    enum class DefaultTab { None, Text, Image, Dat, Audio, Hex,
                            Mesh, Terrain, TerrainMeta, MatTable, SkillTree };
    DefaultTab m_defaultTab = DefaultTab::None;
    bool m_tabSelectionApplied = false;

    // Deferred scroll — applied after lazy data loading
    int m_deferredScrollLine = -1;
    int m_deferredScrollRow = -1;

    // Unsaved changes prompt
    bool m_showSavePrompt = false;
    std::string m_pendingPath;
    std::string m_pendingName;
    int64_t m_pendingSize = 0;
    FileType m_pendingType = FileType::Unknown;
    void* m_pendingNodePtr = nullptr;

    void DoLoadFile(const std::string& path, const std::string& name,
                    int64_t size, FileType type, void* nodePtr);

    void EnsureDataLoaded();
    void InitViewers();
    // The DAT schema is loaded (game data the host may still be downloading).
    bool SchemaReady() const;

    // Typed accessors for viewers with extra methods beyond IViewer
    TextViewer* GetTextViewer();
    DatViewer* GetDatViewer();

    // Helper: get viewer for a FileType, returns nullptr if not found
    IViewer* GetViewer(FileType type);
};

} // namespace GGPKExp
