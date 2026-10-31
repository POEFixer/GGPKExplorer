#pragma once
#include "../../POEFixer/plugin_sdk/PluginSDK.h"
#include "core/GGPKManager.h"
#include "core/VirtualTree.h"
#include "core/DatSchema.h"
#include "core/SearchTypes.h"
#include "core/FileSearcher.h"
#include "core/ContentSearcher.h"
#include "core/DatSearcher.h"
#include "ui/TreePanel.h"
#include "ui/StatusBar.h"
#include "ui/ContentPanel.h"
#include "ui/SearchDialog.h"
#include "ui/SearchResultsPanel.h"
#include "ui/NavigationBar.h"
#include "ui/KeyboardShortcuts.h"
#include "ui/QuickJumpDialog.h"
#include "ui/BookmarkManager.h"
#include "ui/ExtractDialog.h"
#include "ui/SaveAsExporter.h"
#include "core/Result.h"
#include "core/AsyncTaskRunner.h"
#include "core/GGPKExplorerSettings.h"
#include <string>
#include <vector>
#include <memory>
#include <future>
#include <cstdint>

class GGPKExplorer : public PluginSDK::Plugin {
public:
    void OnEnable(bool isGameAttached) override;
    void OnDisable() override;
    void DrawSettings() override;
    void DrawUI() override;
    void SaveSettings() override;
    const char* GetName() const override { return "GGPK Explorer"; }
    bool WantsOverlay() const override { return false; }

    // Phase 7 gate: true if the host's ImGui font atlas has FontAwesome6 glyphs
    // merged. Probed lazily on first RenderMainContent() so the atlas is built.
    bool IsFA6Available() const { return m_fa6Available; }

    // Phase 7 / T23 — persisted width of the DatViewer CellEditorPanel. Written
    // to config/settings.json alongside bookmarks/splitter. Clamped to the
    // panel's [kMinWidth, kMaxWidth] range on set to mirror the runtime splitter.
    float GetDatEditorPanelWidth() const { return m_datEditorPanelWidth; }
    void  SetDatEditorPanelWidth(float w) {
        if (w < 300.0f) w = 300.0f;
        if (w > 800.0f) w = 800.0f;
        m_datEditorPanelWidth = w;
    }

private:
    // Core
    GGPKExp::GGPKManager m_Manager;
    GGPKExp::VirtualTree m_Tree;

    // The DAT schema (Resources/dat-schema/schema.min.json, game data the host installs and
    // updates: PSDK_GAMEDATA_GROUP_DAT_SCHEMA). Every schema loaded this session is kept, newest
    // last, until the plugin is destroyed: viewers and workers hold raw pointers into a schema
    // (DatViewer's table, a running search or export), so a reload never frees one in use.
    // Declared before the panels, so it outlives them.
    std::vector<std::unique_ptr<GGPKExp::DatSchema>> m_datSchemas;
    std::future<std::unique_ptr<GGPKExp::DatSchema>> m_datSchemaLoad;   // a load in flight
    uint64_t m_datSchemaRevision = UINT64_MAX;   // the game-data revision of the last attempt

    // UI panels
    GGPKExp::TreePanel m_TreePanel;
    GGPKExp::StatusBarState m_StatusBar;
    GGPKExp::ContentPanel m_ContentPanel;
    GGPKExp::FileSearcher m_FileSearcher;
    GGPKExp::ContentSearcher m_ContentSearcher;
    GGPKExp::DatSearcher m_DatSearcher;
    GGPKExp::SearchDialog m_SearchDialog;
    GGPKExp::SearchResultsPanel m_ResultsPanel;
    GGPKExp::NavigationBar m_NavBar;
    std::unique_ptr<GGPKExp::KeyboardShortcuts> m_shortcuts;
    std::unique_ptr<GGPKExp::QuickJumpDialog> m_quickJump;
    std::unique_ptr<GGPKExp::BookmarkManager> m_bookmarkMgr;
    std::unique_ptr<GGPKExp::ExtractDialog> m_extractDialog;
    std::unique_ptr<GGPKExp::SaveAsExporter> m_saveAsExporter;

    // Settings
    float m_SplitterRatio = 0.25f;
    std::string m_GgpkPath;
    bool m_isLoaded = false;

    // State
    bool m_OodleInitialized = false;
    bool m_ggpkLoadPending = false;  // deferred load: show spinner 1 frame, then load
    bool m_savePending = false;      // async save: show spinner while flushing writes
    std::string m_SelectedFilePath;
    bool m_SelectedIsDirectory = false;

    // FA6 atlas probe (Phase 7 gate). Deferred to first render so the host's
    // font atlas is guaranteed built before probing, and repeated whenever the
    // host rebuilds its atlas (a language change reloads the fonts).
    bool m_fa6Available = false;
    bool m_fa6Checked = false;
    const void* m_fa6Atlas = nullptr;      // the atlas probed ...
    unsigned int m_fa6AtlasFontId = 0;     // ... and its first font's FontId then

    // Phase 7 / T23 — persisted width of the DatViewer right-hand editor panel.
    // Loaded from settings.json in LoadSettings; written back in SaveSettings
    // after querying the current value from ContentPanel.
    float m_datEditorPanelWidth = 400.0f;

    // Async task runner (for non-blocking GGPK open)
    std::unique_ptr<GGPKExp::AsyncTaskRunner> m_taskRunner;
    std::future<GGPKExp::Result<void>> m_openFuture;
    std::future<std::string> m_saveFuture;   // the auto-flush: empty on success, else why

    // UI settings (panel state, speed) shared across animation viewers.
    GGPKExp::GGPKExplorerUiSettings m_uiSettings;

    // Sentinel: true once we've committed to managing ui.json this session
    // (OnEnable ran, or the user made the first change). Guards against
    // SaveSettings() overwriting a valid ui.json with defaults when a loaded
    // plugin never had OnEnable fire (disabled in metadata, but SaveAll still
    // calls SaveSettings on it).
    bool m_uiSettingsLoaded = false;

    // Debounced save: dirty tick-count written when something changes;
    // 0 means "nothing pending".
    uint32_t m_uiSettingsDirtyTickMs = 0;
    static constexpr uint32_t kSaveDebounceMs = 500;

    void RenderMainContent();
    GGPKExp::DatSchema* CurrentDatSchema();
    void PollDatSchema();
    void UseDatSchema(GGPKExp::DatSchema* schema);
    void LoadSettings();
    std::string ReadSmootherGGPKPath();

    // Archive lifecycle. Every background job (extraction, Save As, content search and
    // replace, the DAT viewer's reference scan, the auto-flush) holds records of the index
    // or nodes of the tree: StopBackgroundJobs cancels and joins them all before anything
    // flushes or closes the archive (Load, Unload, F5, OnDisable go through ResetLoadedState).
    void StartLoad();
    void FinishLoad(const GGPKExp::Result<void>& result);
    void StopBackgroundJobs();
    void FlushPendingWrites();
    void ResetLoadedState();
    bool AnyJobRunning() const;
    void HandleContextMenuActions();
    void NavigateToPath(const std::string& path);
    void ExpandAllRecursive(const std::string& path);
    void CollapseAllRecursive(const std::string& path);
};
