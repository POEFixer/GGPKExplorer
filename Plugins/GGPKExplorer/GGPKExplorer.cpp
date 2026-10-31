// Texts in the program language (PluginI18n.h): lang/<code>.json next to the DLL, English
// compiled in through lang_en.generated.h (python tools/plugin_i18n/gen.py Plugins/GGPKExplorer).
// Only the chrome (this file and ui/) is translated; viewers/, core/ and the log stay English.
#include "../../POEFixer/plugin_sdk/PluginSDK.h"
#include "lang_en.generated.h"   // the embedded English: once per DLL, after PluginSDK.h
#include "GGPKExplorer.h"
#include "core/PathUtils.h"
#include "../../POEFixer/imgui/imgui.h"
#include "lib/json.hpp"
#include "../../POEFixer/imgui/IconsFontAwesome6.h"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <future>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <commdlg.h>
#include <shlobj.h>

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace I18n = PluginSDK::I18n;

// Helper: format-then-log against the v6 LogService::Log(level, message).
static void PluginLog(const PluginSDK::Context* ctx, const char* category, const char* fmt, ...) {
    if (!ctx) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ctx->Log.Log(category, buf);
}

// The full-window busy overlay (loading, saving): a spinner and its text.
static void DrawBusyOverlay(const char* text) {
    ImVec2 winPos = ImGui::GetWindowPos();
    ImVec2 winSize = ImGui::GetWindowSize();
    ImVec2 center(winPos.x + winSize.x * 0.5f, winPos.y + winSize.y * 0.5f);
    float radius = 24.0f;
    float t = static_cast<float>(ImGui::GetTime());
    auto* drawList = ImGui::GetForegroundDrawList();
    drawList->AddRectFilled(winPos, ImVec2(winPos.x + winSize.x, winPos.y + winSize.y),
        IM_COL32(0, 0, 0, 100));
    int segments = 12;
    for (int i = 0; i < segments; ++i) {
        float angle = t * 5.0f + static_cast<float>(i) * 3.14159265f * 2.0f / static_cast<float>(segments);
        float alpha = static_cast<float>(segments - i) / static_cast<float>(segments);
        ImVec2 p(center.x + cosf(angle) * radius, center.y + sinf(angle) * radius);
        drawList->AddCircleFilled(p, 3.5f, IM_COL32(255, 255, 255, static_cast<int>(alpha * 255)));
    }
    ImVec2 textSize = ImGui::CalcTextSize(text);
    drawList->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y + radius + 12.0f),
        IM_COL32(255, 255, 255, 255), text);
}

// ---------------------------------------------------------------------------
// Plugin lifecycle (v6 SDK: host calls PluginSDK_AttachHost between
// CreatePlugin() and OnEnable(); ctx() and Directory() are populated by then).
// ---------------------------------------------------------------------------

void GGPKExplorer::OnEnable(bool /*isGameAttached*/) {
    if (ctx()->ImGuiContext)
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));
    LoadSettings();

    // Load persisted UI settings (panel state + playback speed). First-run
    // miss is harmless — struct keeps its default-constructed values.
    //
    // Host calls SetContext BEFORE SetPluginDirectory, so loading here (in
    // OnEnable) guarantees Directory() is populated.
    GGPKExp::LoadUiSettings(DirectoryPath(), m_uiSettings);

    // Mark as loaded regardless of whether ui.json existed — we're now
    // committing to persist UI settings for this session. Without this flag,
    // SaveSettings() would refuse to write and overwrite would be prevented.
    m_uiSettingsLoaded = true;

    // Propagate to ContentPanel — which caches and forwards to the model
    // viewer at viewer-init time (it doesn't exist until SetD3DDevice).
    m_ContentPanel.SetAnimPanelState(&m_uiSettings.animPanel);
    m_ContentPanel.SetPlaybackSpeedPtr(&m_uiSettings.playbackSpeed);
    m_ContentPanel.SetUiSettings(&m_uiSettings);
    m_ContentPanel.SetViewSettings(&m_uiSettings.meshView);
    m_ContentPanel.SetInitialPlaybackSpeed(m_uiSettings.playbackSpeed);

    // Phase 7 / T23 — push persisted editor panel width. ContentPanel caches
    // it until DatViewer is lazily created (InitViewers picks it up there).
    m_ContentPanel.SetDatEditorPanelWidth(m_datEditorPanelWidth);

    // Create async task runner for non-blocking operations
    if (!m_taskRunner) {
        m_taskRunner = std::make_unique<GGPKExp::AsyncTaskRunner>(2);
    }

    // Pass schema and task runner to DatViewer inside ContentPanel. The DAT schema itself loads
    // in the background (PollDatSchema, started at the end of OnEnable and polled every frame);
    // until it is there the DAT views show "Waiting for game data".
    m_ContentPanel.SetDatSchema(CurrentDatSchema());
    if (m_taskRunner) {
        m_ContentPanel.SetTaskRunner(m_taskRunner.get());
    }

    // Wire up NavigationBar callback
    m_NavBar.SetOnNavigate([this](const std::string& path) {
        NavigateToPath(path);
    });

    // Register keyboard shortcuts
    m_shortcuts = std::make_unique<GGPKExp::KeyboardShortcuts>();
    // Quick Jump dialog
    m_quickJump = std::make_unique<GGPKExp::QuickJumpDialog>();
    m_quickJump->SetOnNavigate([this](const std::string& path) {
        NavigateToPath(path);
    });

    // Bookmark manager
    m_bookmarkMgr = std::make_unique<GGPKExp::BookmarkManager>();

    // Extract dialog
    m_extractDialog = std::make_unique<GGPKExp::ExtractDialog>();
    m_extractDialog->SetManager(&m_Manager);
    m_extractDialog->SetTree(&m_Tree);
    m_extractDialog->SetTreePanel(&m_TreePanel);
    m_extractDialog->SetContentPanel(&m_ContentPanel);
    m_extractDialog->SetStatusBar(&m_StatusBar);
    m_extractDialog->SetLogCallback([this](const char* category, const char* message) {
        ctx()->Log.Log(category, message);
    });

    // Save As exporter (tree-level .datc64/.dds format conversion)
    m_saveAsExporter = std::make_unique<GGPKExp::SaveAsExporter>();
    m_saveAsExporter->SetManager(&m_Manager);
    m_saveAsExporter->SetTree(&m_Tree);
    m_saveAsExporter->SetTreePanel(&m_TreePanel);
    m_saveAsExporter->SetStatusBar(&m_StatusBar);
    m_saveAsExporter->SetSchema(CurrentDatSchema());
    m_saveAsExporter->SetLogCallback([this](const char* category, const char* message) {
        ctx()->Log.Log(category, message);
    });

    // Ctrl+P: Quick Jump
    m_shortcuts->Register(ImGuiKey_P, true, false, false, [this]() {
        if (m_quickJump) m_quickJump->Open();
    });
    // Ctrl+Shift+F: Global search
    m_shortcuts->Register(ImGuiKey_F, true, true, false, [this]() {
        m_SearchDialog.Open({}, false);
    });
    // Ctrl+F: Find in context
    m_shortcuts->Register(ImGuiKey_F, true, false, false, [this]() {
        if (!m_SelectedFilePath.empty()) {
            if (m_SelectedIsDirectory)
                m_SearchDialog.Open(0, m_SelectedFilePath, false);
            else
                m_SearchDialog.Open(1, m_SelectedFilePath, true);
        }
    });
    // Ctrl+R: Replace in context
    m_shortcuts->Register(ImGuiKey_R, true, false, false, [this]() {
        if (!m_SelectedFilePath.empty()) {
            m_SearchDialog.Open(1, m_SelectedFilePath, m_SelectedIsDirectory ? false : true);
        }
    });
    // Alt+Left: Navigate back
    m_shortcuts->Register(ImGuiKey_LeftArrow, false, false, true, [this]() {
        m_NavBar.GoBack();
    });
    // Alt+Right: Navigate forward
    m_shortcuts->Register(ImGuiKey_RightArrow, false, false, true, [this]() {
        m_NavBar.GoForward();
    });
    // Ctrl+D: Toggle bookmark
    m_shortcuts->Register(ImGuiKey_D, true, false, false, [this]() {
        if (!m_SelectedFilePath.empty()) {
            m_Tree.SetBookmark(m_SelectedFilePath, !m_Tree.IsBookmarked(m_SelectedFilePath));
        }
    });
    // F5: Reload
    m_shortcuts->Register(ImGuiKey_F5, false, false, false, [this]() {
        if (m_Manager.IsOpen() && !m_ggpkLoadPending && m_taskRunner) {
            m_GgpkPath = m_Manager.GetOpenedPath();
            ResetLoadedState();
            StartLoad();
        }
    });
    // Escape: Close popups
    m_shortcuts->Register(ImGuiKey_Escape, false, false, false, [this]() {
        if (m_quickJump) m_quickJump->Close();
        m_SearchDialog.Close();
        if (m_bookmarkMgr) m_bookmarkMgr->Close();
    });

    // Init Oodle (hardcoded path)
    if (!m_OodleInitialized) {
        m_OodleInitialized = GGPKExp::GGPKManager::InitOodle("");
    }

    // Pre-fill path but don't auto-load
    if (m_GgpkPath.empty()) {
        std::string ggpkPath = ReadSmootherGGPKPath();
        std::error_code ec;
        if (!ggpkPath.empty() && fs::exists(GGPKExp::PathUtils::FromUtf8(ggpkPath), ec)) {
            m_GgpkPath = ggpkPath;
        }
    }

    // Start loading the DAT schema (or adopt one a previous session left loading).
    PollDatSchema();

    PluginLog(ctx(), "Info", "GGPKExplorer enabled");
}

// ---------------------------------------------------------------------------
// DAT schema: game data the host installs and updates (Game Data v2, desktop
// 371+: PSDK_GAMEDATA_GROUP_DAT_SCHEMA). Loaded off the drawing thread, again
// whenever its revision moves; an older host (revision 0) ships the file with
// the program, so it loads once.
// ---------------------------------------------------------------------------

// The schema in use: the newest one loaded, or an empty (not loaded) one before the first load.
GGPKExp::DatSchema* GGPKExplorer::CurrentDatSchema() {
    if (m_datSchemas.empty())
        m_datSchemas.push_back(std::make_unique<GGPKExp::DatSchema>());
    return m_datSchemas.back().get();
}

// Hands the schema to everything that reads it. Old schemas stay alive (m_datSchemas), so a
// viewer or a worker still holding one keeps valid tables.
void GGPKExplorer::UseDatSchema(GGPKExp::DatSchema* schema) {
    m_ContentPanel.SetDatSchema(schema);
    m_SearchDialog.SetDatSchema(schema);
    if (m_saveAsExporter) m_saveAsExporter->SetSchema(schema);
}

// Drawing thread: adopts a finished load; starts a load when the schema's game-data revision
// moved since the last attempt and the file is there (a missing file waits for the next move).
void GGPKExplorer::PollDatSchema() {
    if (m_datSchemaLoad.valid()) {
        if (m_datSchemaLoad.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            return;
        std::unique_ptr<GGPKExp::DatSchema> loaded = m_datSchemaLoad.get();
        if (loaded && loaded->IsLoaded()) {
            m_datSchemas.push_back(std::move(loaded));
            UseDatSchema(m_datSchemas.back().get());
            PluginLog(ctx(), "Info", "GGPKExplorer: DAT schema loaded (game data revision %llu)",
                      static_cast<unsigned long long>(m_datSchemaRevision));
        } else if (loaded) {
            PluginLog(ctx(), "Error", "GGPKExplorer: DAT schema not loaded: %s", loaded->GetError().c_str());
            // No schema loaded yet: the views show why this load failed instead of waiting for
            // ever (a schema loaded earlier stays in use). The next revision tries again.
            if (!CurrentDatSchema()->IsLoaded()) {
                m_datSchemas.push_back(std::move(loaded));
                UseDatSchema(m_datSchemas.back().get());
            }
        }
        return;
    }

    const uint64_t revision = ctx()->Lang.GameDataRevision(PSDK_GAMEDATA_GROUP_DAT_SCHEMA);
    if (revision == m_datSchemaRevision) return;
    m_datSchemaRevision = revision;

    wchar_t exePath[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    // fs::path keeps the native wide form: path::string() throws under the default C locale
    // when the program folder has non-ASCII characters (Cyrillic etc.).
    const fs::path schemaPath = fs::path(exePath).parent_path() / "Resources" / "dat-schema" / "schema.min.json";
    std::error_code ec;
    if (!fs::exists(schemaPath, ec)) {
        PluginLog(ctx(), "Info", "GGPKExplorer: waiting for the DAT schema (game data revision %llu)",
                  static_cast<unsigned long long>(revision));
        return;
    }
    m_datSchemaLoad = std::async(std::launch::async, [schemaPath]() {
        auto schema = std::make_unique<GGPKExp::DatSchema>();
        try {
            schema->Load(schemaPath);
        } catch (const std::exception& e) {
            schema->FailUnexpected(e.what());
        } catch (...) {
            schema->FailUnexpected(nullptr);
        }
        // Not loaded: GetError() says why (the views show it); the next revision tries again.
        if (!schema->IsLoaded() && schema->GetError().empty()) schema->FailUnexpected(nullptr);
        return schema;
    });
}

void GGPKExplorer::OnDisable() {
    // Flush any pending UI settings before shutdown so nothing is lost.
    // Gated on m_uiSettingsLoaded: without it we might write defaults to
    // ui.json when the plugin was never actually enabled this session.
    if (m_uiSettingsLoaded && m_uiSettingsDirtyTickMs != 0) {
        GGPKExp::SaveUiSettings(DirectoryPath(), m_uiSettings);
        m_uiSettingsDirtyTickMs = 0;
    }

    m_shortcuts.reset();
    m_quickJump.reset();
    m_bookmarkMgr.reset();

    // An open in flight finishes first (no job runs while it does): its archive is closed below.
    if (m_openFuture.valid()) {
        try {
            (void)m_openFuture.get();
        } catch (...) {}
    }
    m_ggpkLoadPending = false;

    // Every job is cancelled and joined, replaced files are saved, then the archive closes.
    ResetLoadedState();
    m_extractDialog.reset();
    m_saveAsExporter.reset();

    // A schema load in flight finishes here; the next OnEnable adopts its result.
    if (m_datSchemaLoad.valid()) {
        m_datSchemaLoad.wait();
    }
    m_taskRunner.reset();

    PluginLog(ctx(), "Info", "GGPKExplorer disabled");
}

// ---------------------------------------------------------------------------
// Archive lifecycle
// ---------------------------------------------------------------------------

bool GGPKExplorer::AnyJobRunning() const {
    return (m_extractDialog && m_extractDialog->IsExtractRunning())
        || (m_saveAsExporter && m_saveAsExporter->IsRunning())
        || m_ContentSearcher.IsRunning()
        || m_ContentPanel.IsDatReferenceScanRunning();   // the DAT viewer's Find references
}

// Cancels and joins every job that reads the archive or walks the tree. The DAT viewer's
// reference scan and row decode stop with ContentPanel::Clear (DatViewer::Clear cancels and
// waits for them). The auto-flush is waited for (a save is never cut short).
void GGPKExplorer::StopBackgroundJobs() {
    m_FileSearcher.CancelAndJoin();
    m_ContentSearcher.CancelAndJoin();
    if (m_extractDialog) m_extractDialog->JoinExtractThread();
    if (m_saveAsExporter) m_saveAsExporter->CancelAndJoin();
    m_ContentPanel.Clear();
    if (m_saveFuture.valid()) {
        std::string error;
        try {
            error = m_saveFuture.get();
        } catch (...) {
            error = "unknown error";
        }
        if (!error.empty())
            PluginLog(ctx(), "Error", "GGPKExplorer: saving the replaced files failed: %s", error.c_str());
    }
    m_savePending = false;
}

// Replaced files not saved yet: saved now (the jobs are stopped), before the archive closes.
void GGPKExplorer::FlushPendingWrites() {
    if (!m_Manager.HasPendingWrites()) return;
    std::string error;
    if (!m_Manager.FlushWrites(&error))
        PluginLog(ctx(), "Error", "GGPKExplorer: saving the replaced files failed: %s", error.c_str());
}

// Back to "nothing loaded" (Load, Unload, F5, OnDisable): jobs stopped, writes saved, the
// archive closed, the tree and every view of it reset.
void GGPKExplorer::ResetLoadedState() {
    StopBackgroundJobs();
    FlushPendingWrites();
    m_Manager.Close();
    m_Tree.Clear();
    m_NavBar.Clear();
    m_TreePanel.ClearSelection();
    m_SearchDialog.DiscardPending();
    m_SelectedFilePath.clear();
    m_SelectedIsDirectory = false;
    m_StatusBar.fileCount = 0;
    m_StatusBar.dirCount = 0;
    m_StatusBar.totalSize = 0;
    m_StatusBar.selectedName.clear();
    m_StatusBar.selectedSize = -1;
    m_StatusBar.progress.store(0.0f);
    m_StatusBar.progressActive.store(false);
    m_StatusBar.progressLabel = {};
    m_isLoaded = false;
}

// Opens m_GgpkPath on the task runner; RenderMainContent shows the spinner until FinishLoad.
void GGPKExplorer::StartLoad() {
    if (m_GgpkPath.empty() || !m_taskRunner) return;
    if (!m_OodleInitialized) {
        m_OodleInitialized = GGPKExp::GGPKManager::InitOodle("");
        if (m_OodleInitialized) {
            PluginLog(ctx(), "Info", "GGPKExplorer: Oodle initialized");
        }
    }
    m_ggpkLoadPending = true;
    m_openFuture = m_Manager.OpenAsync(m_GgpkPath, *m_taskRunner);
}

// The drawing thread, once the open finished: the tree and every view of the archive.
void GGPKExplorer::FinishLoad(const GGPKExp::Result<void>& result) {
    m_ggpkLoadPending = false;
    if (!result) {
        m_isLoaded = false;
        PluginLog(ctx(), "Error", "GGPKExplorer: failed to open: %s", result.error.c_str());
        return;
    }

    {
        const char* modeStr = (m_Manager.GetMode() == GGPKExp::SourceMode::StandaloneGGPK) ? "GGPK" : "Bundles";
        PluginLog(ctx(), "Info", "GGPKExplorer: opened %s (mode: %s)", m_GgpkPath.c_str(), modeStr);
    }
    auto savedBookmarks = m_Tree.GetBookmarks();
    m_Tree.BuildFromManager(m_Manager);
    if (!savedBookmarks.empty())
        m_Tree.SetBookmarks(std::move(savedBookmarks));
    m_ContentPanel.SetManager(&m_Manager);
    m_ContentPanel.SetVirtualTree(&m_Tree);
    if (ctx()->D3DDevice)
        m_ContentPanel.SetD3DDevice(static_cast<ID3D11Device*>(ctx()->D3DDevice));
    m_TreePanel.SetTree(&m_Tree);
    if (m_quickJump) m_quickJump->SetTree(&m_Tree);
    m_TreePanel.SetOnSelect([this](const GGPKExp::TreeEntry& entry) {
        m_SelectedFilePath = entry.fullPath;
        m_SelectedIsDirectory = entry.isDirectory;
        m_StatusBar.selectedName = entry.name;
        m_StatusBar.selectedSize = entry.fileSize;
        m_NavBar.Navigate(entry.fullPath);
        if (!entry.isDirectory) {
            m_ContentPanel.LoadFile(entry.fullPath, entry.name,
                                    entry.fileSize, entry.type, entry.nodePtr);
        } else {
            m_ContentPanel.Clear();
        }
    });
    m_SearchDialog.SetTree(&m_Tree);
    m_SearchDialog.SetManager(&m_Manager);
    m_SearchDialog.SetDatSchema(CurrentDatSchema());
    m_SearchDialog.SetFileSearcher(&m_FileSearcher);
    m_SearchDialog.SetContentSearcher(&m_ContentSearcher);
    m_SearchDialog.SetDatSearcher(&m_DatSearcher);
    m_SearchDialog.SetOnResults([this](const GGPKExp::SearchResult& sr) {
        m_ResultsPanel.ShowSearchResults(sr);
    });
    m_SearchDialog.SetOnReplaceLog([this](const std::vector<GGPKExp::ReplaceResult>& rr) {
        m_ResultsPanel.ShowReplaceLog(rr);
    });
    m_SearchDialog.SetLogCallback([this](const char* category, const char* message) {
        ctx()->Log.Log(category, message);
    });
    m_SearchDialog.SetOnNavigateToLine([this](const std::string& p, int line) {
        NavigateToPath(p);
        m_ContentPanel.ScrollToLine(line);
    });
    m_ResultsPanel.SetOnNavigate([this](const std::string& p, int line, bool isDatRow) {
        NavigateToPath(p);
        if (line >= 0) {
            if (isDatRow)
                m_ContentPanel.ScrollToRow(line);
            else
                m_ContentPanel.ScrollToLine(line);
        }
    });
    m_isLoaded = true;

    // Populate status bar
    m_StatusBar.fileCount = m_Tree.GetFileCount();
    m_StatusBar.dirCount = m_Tree.GetDirCount();
    m_StatusBar.totalSize = m_Tree.GetTotalSize();
}

// ---------------------------------------------------------------------------
// NavigateToPath — shared navigation logic for tree, search, breadcrumb, etc.
// ---------------------------------------------------------------------------

void GGPKExplorer::NavigateToPath(const std::string& path) {
    if (path.empty()) return;

    m_Tree.ExpandTo(path);
    uint32_t idx = m_Tree.FindByPath(path);
    if (idx != UINT32_MAX) {
        const auto& flatList = m_Tree.GetFlatList();
        const auto& entry = flatList[idx];
        m_TreePanel.SetSelectedPath(path);
        m_TreePanel.RevealSelected();
        m_SelectedFilePath = path;
        m_SelectedIsDirectory = entry.isDirectory;
        m_StatusBar.selectedName = entry.name;
        m_StatusBar.selectedSize = entry.fileSize;
        m_NavBar.Navigate(path);
        if (!entry.isDirectory) {
            m_ContentPanel.LoadFile(entry.fullPath, entry.name,
                                    entry.fileSize, entry.type, entry.nodePtr);
        } else {
            m_ContentPanel.Clear();
        }
    }
}

// ---------------------------------------------------------------------------
// DrawUI -- main window
// ---------------------------------------------------------------------------

void GGPKExplorer::DrawUI() {
    // All UI is rendered in DrawSettings() to appear in the plugin tab directly
}

void GGPKExplorer::RenderMainContent() {
    if (!HostCompatible()) return;
    if (ctx()->ImGuiContext)
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));

    // ====================================================================
    // One-time FontAwesome6 atlas sanity check (Phase 7 gate).
    // Runs on first render — by now the host has built its font atlas.
    // Probes ICON_FA_PEN (U+F304) which lives in the FA6 glyph block; if
    // missing, the host did not merge FA6 into the active atlas and DatViewer
    // icons will need text fallbacks.
    // ====================================================================
    // The host rebuilds its atlas when the language changes (every ImFont is new then, with a new
    // FontId): probe again for the new atlas.
    ImGuiIO& atlasIo = ImGui::GetIO();
    const unsigned int atlasFontId = atlasIo.Fonts->Fonts.Size > 0 ? atlasIo.Fonts->Fonts[0]->FontId : 0u;
    if (m_fa6Checked && (m_fa6Atlas != atlasIo.Fonts || m_fa6AtlasFontId != atlasFontId))
        m_fa6Checked = false;
    if (!m_fa6Checked) {
        m_fa6Checked = true;
        m_fa6Atlas = atlasIo.Fonts;
        m_fa6AtlasFontId = atlasFontId;
        ImGuiIO& io = ImGui::GetIO();
        m_fa6Available = false;

        // Probe ICON_FA_PEN (U+F304). The host merges FA6 onto Fonts[0]
        // (primary) and then *adds* a separate monospace entry as Fonts[1]
        // (NOT merged). So Fonts.back() is NOT a reliable probe target.
        // Iterate all loaded fonts and succeed if any one has the glyph —
        // robust to font-ordering changes in the host atlas.
        const ImWchar fa6Probe = static_cast<ImWchar>(0xf304);
        for (ImFont* f : io.Fonts->Fonts) {
            if (f && f->IsGlyphInFont(fa6Probe)) {
                m_fa6Available = true;
                break;
            }
        }

        if (!m_fa6Available) {
            PluginLog(ctx(), "GGPKExplorer",
                "[warn] FontAwesome6 glyphs not in any loaded font - icons will fall back to text labels");
        }
        // Propagate to ContentPanel so DatViewer gets it on lazy init or next
        // SetFA6Available call.
        m_ContentPanel.SetFA6Available(m_fa6Available);
    }

    // DAT schema: adopt a finished load, reload when the host updated the game data.
    PollDatSchema();

    // NOTE: UI settings debounced save lives AFTER ContentPanel::Render (see
    // below). Polling here would miss same-frame speed edits because the
    // viewer only commits the new speed during its Render() call.

    // ====================================================================
    // Async GGPK load — non-blocking, spinner while loading
    // ====================================================================
    if (m_ggpkLoadPending) {
        if (m_openFuture.valid() &&
            m_openFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            GGPKExp::Result<void> result = GGPKExp::Result<void>::Fail("Unknown error while opening the game files");
            try {
                result = m_openFuture.get();
            } catch (...) {}
            FinishLoad(result);
            return; // skip rest of rendering this frame
        }

        // Still loading — draw spinner overlay
        DrawBusyOverlay(I18n::Tr("status_loading"));
        return; // skip rest of rendering this frame
    }

    // ====================================================================
    // Async save — non-blocking, spinner while flushing writes
    // ====================================================================
    if (m_savePending) {
        if (m_saveFuture.valid() &&
            m_saveFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            std::string error;
            try {
                error = m_saveFuture.get();
            } catch (...) {
                error = "unknown error";
            }
            if (!error.empty())
                PluginLog(ctx(), "Error", "GGPKExplorer: saving the replaced files failed: %s", error.c_str());
            m_savePending = false;
        } else {
            // Still saving — draw spinner overlay
            DrawBusyOverlay(I18n::Tr("status_saving"));
            return; // skip rest of rendering this frame
        }
    }

    // ====================================================================
    // Keyboard shortcuts (delegated to KeyboardShortcuts component)
    // ====================================================================
    if (m_shortcuts) {
        m_shortcuts->ProcessInput();
    }

    // ====================================================================
    // Toolbar row
    // ====================================================================
    {
        // Path label
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(I18n::Tr("toolbar_path"));
        ImGui::SameLine();

        // Path input — fixed width
        float inputW = 1000.0f;
        float availW = ImGui::GetContentRegionAvail().x;
        if (inputW > availW * 0.5f) inputW = availW * 0.5f;
        if (inputW < 150.0f) inputW = 150.0f;

        // Editable path field backed by m_GgpkPath
        char pathBuf[1024];
        size_t copyLen = m_GgpkPath.size() < sizeof(pathBuf) - 1 ? m_GgpkPath.size() : sizeof(pathBuf) - 1;
        memcpy(pathBuf, m_GgpkPath.c_str(), copyLen);
        pathBuf[copyLen] = '\0';

        ImGui::SetNextItemWidth(inputW);
        if (ImGui::InputText("##GgpkPath", pathBuf, sizeof(pathBuf))) {
            m_GgpkPath = pathBuf;
        }

        ImGui::SameLine();

        // "..." button — opens file dialog for .ggpk files
        if (ImGui::Button("...", ImVec2(32.0f, 0.0f))) {
            wchar_t filePath[MAX_PATH] = {};
            GGPKExp::DialogFilter filter;
            filter.Add(I18n::Tr("dialog_filter_game_files"), L"*.ggpk;_.index.bin")
                  .Add(I18n::Tr("dialog_filter_ggpk_files"), L"*.ggpk")
                  .Add(I18n::Tr("dialog_filter_index_files"), L"_.index.bin")
                  .Add(I18n::Tr("dialog_filter_all_files"), L"*.*");
            const std::wstring title = GGPKExp::Wide(I18n::Tr("dialog_select_game_file"));
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = nullptr;
            ofn.lpstrFilter = filter.Get();
            ofn.lpstrFile = filePath;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            ofn.lpstrTitle = title.c_str();

            if (GetOpenFileNameW(&ofn)) {
                std::string utf8Path = GGPKExp::PathUtils::WideToUtf8(filePath);
                if (!utf8Path.empty()) {
                    m_GgpkPath = utf8Path;
                }
            }
        }

        ImGui::SameLine();

        // Load button — async: submits to task runner, spinner renders while loading
        const char* loadLabel = I18n::TrId("toolbar_load");
        if (ImGui::Button(loadLabel, ImVec2(GGPKExp::ButtonWidth(loadLabel, 7.5f), 0.0f)) && !m_ggpkLoadPending) {
            if (!m_GgpkPath.empty() && m_taskRunner) {
                // No "game is running" gate: on patch day the files have to come out of the
                // new build while the client is still up. The library reads without write
                // access (and falls back to read-only when the game holds the files), so the
                // open works; a write then fails with a precise message.
                ResetLoadedState();
                StartLoad();
            }
        }

        // Unload button: adjacent to Load (no text between them)
        if (m_isLoaded && m_Manager.IsOpen()) {
            ImGui::SameLine();
            const char* unloadLabel = I18n::TrId("toolbar_unload");
            if (ImGui::Button(unloadLabel, ImVec2(GGPKExp::ButtonWidth(unloadLabel, 7.5f), 0.0f))) {
                ResetLoadedState();
            }
        }

        // 16px visual gap, then Bookmarks
        if (m_Manager.IsOpen()) {
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(16.0f, 0.0f));
            ImGui::SameLine();
            const std::string bookmarksLabel = std::string("\xef\x80\xae ") + I18n::TrId("toolbar_bookmarks");
            if (ImGui::Button(bookmarksLabel.c_str())) {
                if (m_bookmarkMgr) m_bookmarkMgr->Open();
            }

            // Sort button (FontAwesome sort icon, U+F0DC)
            ImGui::SameLine();
            if (ImGui::Button("\xef\x83\x9c", ImVec2(28.0f, 0.0f))) {
                ImGui::OpenPopup("##GGPKSortPopup");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", I18n::Tr("toolbar_sort"));
            }

            if (ImGui::BeginPopup("##GGPKSortPopup")) {
                GGPKExp::SortMode current = m_Tree.GetSortMode();
                auto item = [&](const char* label, GGPKExp::SortMode mode) {
                    if (ImGui::MenuItem(label, nullptr, current == mode)) {
                        m_Tree.SetSortMode(mode);
                        SaveSettings();
                    }
                };
                item(I18n::TrId("sort_none"),            GGPKExp::SortMode::None);
                item(I18n::TrId("sort_name"),            GGPKExp::SortMode::Name);
                item(I18n::TrId("sort_size"),            GGPKExp::SortMode::Size);
                item(I18n::TrId("sort_type"),            GGPKExp::SortMode::Type);
                item(I18n::TrId("sort_bookmarks_first"), GGPKExp::SortMode::Bookmarks);
                ImGui::EndPopup();
            }
        }

        // A running extraction or Save As: its Cancel button (its progress bar is in the
        // status bar). The job stops on its own thread; the button goes when it has.
        if (m_extractDialog && m_extractDialog->IsExtractRunning()) {
            ImGui::SameLine();
            const char* cancelLabel = I18n::TrId("toolbar_cancel_extract");
            if (ImGui::Button(cancelLabel, ImVec2(GGPKExp::ButtonWidth(cancelLabel, 7.5f), 0.0f)))
                m_extractDialog->CancelExtract();
        }
        if (m_saveAsExporter && m_saveAsExporter->IsRunning()) {
            ImGui::SameLine();
            const char* cancelLabel = I18n::TrId("toolbar_cancel_save_as");
            if (ImGui::Button(cancelLabel, ImVec2(GGPKExp::ButtonWidth(cancelLabel, 7.5f), 0.0f)))
                m_saveAsExporter->Cancel();
        }

        // Status text (Loading / Error / Not loaded) — rendered after all buttons; it takes
        // the next line when the rest of the row is too narrow for it (a translation is often
        // longer than the English text).
        const char* statusText = nullptr;
        ImVec4 statusColor = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        if (m_ggpkLoadPending) {
            statusText = I18n::Tr("status_loading");
            statusColor = ImVec4(1.0f, 0.85f, 0.0f, 1.0f);
        } else if (!m_Manager.IsOpen() && !m_Manager.GetError().empty()) {
            statusText = I18n::Tr("status_error");
            statusColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        } else if (!m_Manager.IsOpen()) {
            statusText = I18n::Tr("status_not_loaded");
        }
        ImGui::SameLine();
        if (statusText) {
            if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(statusText).x)
                ImGui::NewLine();
            ImGui::TextColored(statusColor, "%s", statusText);
        }
    }

    // ====================================================================
    // Navigation bar row (back/forward + breadcrumb)
    // ====================================================================
    if (m_Manager.IsOpen()) {
        m_NavBar.Render();
    }
    ImGui::Separator();

    // ====================================================================
    // Main content area: Tree (left) | Content (right)
    // ====================================================================
    {
        float availWidth = ImGui::GetContentRegionAvail().x;
        // Reserve space for: status bar (frame + spacing) + extra padding
        float statusBarH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 4.0f;
        float resultsH = m_ResultsPanel.GetHeight();
        float availHeight = ImGui::GetContentRegionAvail().y - statusBarH - resultsH;

        float leftWidth = availWidth * m_SplitterRatio;
        float splitterWidth = 4.0f;
        float rightWidth = availWidth - leftWidth - splitterWidth;

        // Clamp
        if (leftWidth < 100.0f) leftWidth = 100.0f;
        if (rightWidth < 100.0f) {
            rightWidth = 100.0f;
            leftWidth = availWidth - rightWidth - splitterWidth;
        }

        // ------ Left panel: Tree ------
        ImGui::BeginChild("##LeftPane", ImVec2(leftWidth, availHeight), ImGuiChildFlags_None);
        m_TreePanel.Render(leftWidth);
        ImGui::EndChild();

        ImGui::SameLine(0.0f, 0.0f);

        // ------ Splitter (draggable) ------
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.6f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.6f, 0.7f, 1.0f));
        ImGui::Button("##Splitter", ImVec2(splitterWidth, availHeight));
        ImGui::PopStyleColor(3);

        if (ImGui::IsItemActive()) {
            float delta = ImGui::GetIO().MouseDelta.x;
            leftWidth += delta;
            // Clamp to reasonable bounds
            float minLeft = 100.0f;
            float maxLeft = availWidth - splitterWidth - 100.0f;
            if (leftWidth < minLeft) leftWidth = minLeft;
            if (leftWidth > maxLeft) leftWidth = maxLeft;
            m_SplitterRatio = leftWidth / availWidth;
        }

        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }

        ImGui::SameLine(0.0f, 0.0f);

        // ------ Right panel: ContentPanel ------
        ImGui::BeginChild("##RightPane", ImVec2(rightWidth, availHeight), ImGuiChildFlags_Borders);

        if (m_Manager.IsOpen()) {
            m_ContentPanel.Render();

            // Consume navigation request from ContentPanel (DatViewer foreign keys + TextViewer hyperlinks)
            if (!m_ContentPanel.pendingNavigatePath.empty()) {
                std::string navPath = m_ContentPanel.pendingNavigatePath;
                int navRow = m_ContentPanel.pendingNavigateRow;
                m_ContentPanel.pendingNavigatePath.clear();
                m_ContentPanel.pendingNavigateRow = -1;

                std::string foundPath;

                if (navPath.find('/') != std::string::npos) {
                    // Full path (TextViewer hyperlinks): expand parent dirs first, then search
                    m_Tree.ExpandTo(navPath);

                    // Try exact case match (O(1) hash lookup)
                    uint32_t idx = m_Tree.FindByPath(navPath);
                    if (idx != UINT32_MAX) {
                        foundPath = navPath;
                    }

                    // Case-insensitive fallback in now-expanded flat list
                    if (foundPath.empty()) {
                        const auto& flatList = m_Tree.GetFlatList();
                        for (const auto& entry : flatList) {
                            if (!entry.isDirectory && _stricmp(entry.fullPath.c_str(), navPath.c_str()) == 0) {
                                foundPath = entry.fullPath;
                                break;
                            }
                        }
                    }
                } else {
                    // Filename only (DatViewer foreign keys)
                    const auto& flatList = m_Tree.GetFlatList();
                    for (const auto& entry : flatList) {
                        if (!entry.isDirectory && _stricmp(entry.name.c_str(), navPath.c_str()) == 0) {
                            foundPath = entry.fullPath;
                            break;
                        }
                    }
                }

                if (!foundPath.empty()) {
                    NavigateToPath(foundPath);
                    if (navRow >= 0) {
                        m_ContentPanel.ScrollToRow(navRow);
                    }
                } else {
                    m_ContentPanel.navNotFoundPath = navPath;
                }
            }
        } else {
            float winW = ImGui::GetContentRegionAvail().x;
            float winH = ImGui::GetContentRegionAvail().y;

            // Show error message if the last open attempt failed (Task 21)
            const auto& openError = m_Manager.GetError();
            if (!openError.empty()) {
                float centerY = winH * 0.4f;
                ImGui::SetCursorPos(ImVec2(10.0f, centerY));
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", I18n::Tr("open_failed"));
                ImGui::SetCursorPosX(10.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", openError.c_str());
                ImGui::Spacing();
                ImGui::SetCursorPosX(10.0f);
                ImGui::TextDisabled("%s", I18n::Tr("open_failed_hint"));
            } else {
                const char* msg = I18n::Tr("open_hint");
                ImVec2 textSize = ImGui::CalcTextSize(msg);
                ImGui::SetCursorPos(ImVec2((winW - textSize.x) * 0.5f, (winH - textSize.y) * 0.5f));
                ImGui::TextDisabled("%s", msg);
            }
        }

        ImGui::EndChild();
    }

    // ====================================================================
    // UI settings debounced save: poll viewers for changes, flush after
    // kSaveDebounceMs of quiet time. Must run AFTER ContentPanel::Render
    // (above) so a speed slider edit made this frame is captured this frame
    // — otherwise a tab-away or shutdown right after the edit would save the
    // previous frame's stale value.
    // ====================================================================
    if (m_ContentPanel.ConsumeUiSettingsDirty()) {
        // Note: m_uiSettings.playbackSpeed is already kept up-to-date via the
        // pointer mechanism — AnimationSidePanel's combo writes through
        // *m_playbackSpeed which points at m_uiSettings.playbackSpeed directly
        // (see SetPlaybackSpeedPtr wiring); nothing to copy back here.
        m_uiSettingsDirtyTickMs = GetTickCount();
        // Defensive: if OnEnable somehow didn't run but the user managed to
        // interact, commit to managing ui.json from now on.
        m_uiSettingsLoaded = true;
    }
    if (m_uiSettingsLoaded && m_uiSettingsDirtyTickMs != 0) {
        const uint32_t now = GetTickCount();
        if (now - m_uiSettingsDirtyTickMs >= kSaveDebounceMs) {
            GGPKExp::SaveUiSettings(DirectoryPath(), m_uiSettings);
            m_uiSettingsDirtyTickMs = 0;
        }
    }

    // ====================================================================
    // Search results (bottom panel)
    // ====================================================================
    m_ResultsPanel.Render();

    // ====================================================================
    // Search dialog (modal, renders on top)
    // ====================================================================
    m_SearchDialog.Render();

    // ====================================================================
    // Status bar
    // ====================================================================
    GGPKExp::RenderStatusBar(m_StatusBar);

    // ====================================================================
    // Handle context menu action flags
    // ====================================================================
    HandleContextMenuActions();

    if (m_bookmarkMgr) {
        std::string bmNavTarget = m_bookmarkMgr->Render(m_Tree);
        if (!bmNavTarget.empty()) {
            NavigateToPath(bmNavTarget);
        }
    }

    // ====================================================================
    // Extract Options modal
    // ====================================================================
    if (m_extractDialog) {
        m_extractDialog->RenderExtractOptions();
    }
    if (m_saveAsExporter) {
        m_saveAsExporter->UpdateProgress();
    }

    // ====================================================================
    // Quick Jump popup (Ctrl+P)
    // ====================================================================
    if (m_quickJump) {
        m_quickJump->Render();
    }

    // ====================================================================
    // Auto-flush: if any writes are pending, start async save with spinner.
    // Never while a job runs (a replace job writes, the others read): the
    // flush waits until they ended, and the spinner keeps new ones from starting.
    // ====================================================================
    if (!m_savePending && m_Manager.HasPendingWrites() && !AnyJobRunning()) {
        m_savePending = true;
        m_saveFuture = std::async(std::launch::async, [this]() -> std::string {
            std::string error;
            try {
                if (!m_Manager.FlushWrites(&error) && error.empty())
                    error = "unknown error";
            } catch (const std::exception& ex) {
                error = ex.what();
            } catch (...) {
                error = "unknown error";
            }
            return error;
        });
    }
}

// ---------------------------------------------------------------------------
// DrawSettings
// ---------------------------------------------------------------------------

void GGPKExplorer::DrawSettings() {
    // Render the full GGPK Explorer UI directly in the plugin tab
    RenderMainContent();
}

// ---------------------------------------------------------------------------
// SaveSettings
// ---------------------------------------------------------------------------

void GGPKExplorer::SaveSettings() {
    fs::path configDir = DirectoryPath() / "config";
    if (!fs::exists(configDir)) fs::create_directories(configDir);

    // Phase 7 / T23 — query current panel width from DatViewer (via ContentPanel)
    // before serialising. If DatViewer was never created this session, the cached
    // value (= last-loaded or default 400) is returned.
    SetDatEditorPanelWidth(m_ContentPanel.GetDatEditorPanelWidth());

    json j;
    j["splitterRatio"] = m_SplitterRatio;
    j["sortMode"] = static_cast<int>(m_Tree.GetSortMode());
    j["ggpkPath"] = m_GgpkPath;
    j["datEditorPanelWidth"] = m_datEditorPanelWidth;

    // Save bookmarks
    json bm = json::array();
    for (const auto& entry : m_Tree.GetBookmarks()) {
        json obj;
        obj["path"] = entry.path;
        obj["icon"] = entry.iconIndex;
        bm.push_back(obj);
    }
    j["bookmarks"] = bm;

    // Save search dialog state
    json searchState;
    m_SearchDialog.SaveState(searchState);
    j["search"] = searchState;

    std::ofstream file{ configDir / "settings.json" };
    if (file.is_open()) {
        file << j.dump(2);
    }

    // Also persist UI settings (panel state + playback speed). These live in
    // a separate file (ui.json) managed by GGPKExplorerSettings.
    //
    // Only write if we've actually loaded (or had the user modify) these
    // settings this session. Without this guard, a disabled-but-loaded
    // plugin — whose OnEnable never ran — would happily overwrite a valid
    // ui.json with default-constructed values when PluginManager::SaveAll
    // calls SaveSettings().
    if (m_uiSettingsLoaded) {
        GGPKExp::SaveUiSettings(DirectoryPath(), m_uiSettings);
        m_uiSettingsDirtyTickMs = 0;
    }
}

// ---------------------------------------------------------------------------
// LoadSettings
// ---------------------------------------------------------------------------

void GGPKExplorer::LoadSettings() {
    fs::path settingsPath = DirectoryPath() / "config" / "settings.json";

    if (fs::exists(settingsPath)) {
        std::ifstream file{ settingsPath };
        if (file.is_open()) {
            try {
                json j = json::parse(file);

                if (j.contains("splitterRatio")) m_SplitterRatio = j["splitterRatio"].get<float>();
                if (j.contains("ggpkPath") && j["ggpkPath"].is_string())
                    m_GgpkPath = j["ggpkPath"].get<std::string>();

                // Phase 7 / T23 — restore DatViewer editor panel width.
                // Clamped to [kMinWidth, kMaxWidth] by SetDatEditorPanelWidth.
                if (j.contains("datEditorPanelWidth") && j["datEditorPanelWidth"].is_number()) {
                    SetDatEditorPanelWidth(j["datEditorPanelWidth"].get<float>());
                }

                if (j.contains("sortMode")) {
                    int sm = j["sortMode"].get<int>();
                    if (sm >= 0 && sm <= 4)
                        m_Tree.SetSortMode(static_cast<GGPKExp::SortMode>(sm));
                }

                if (j.contains("bookmarks") && j["bookmarks"].is_array()) {
                    std::vector<GGPKExp::BookmarkEntry> bookmarks;
                    for (const auto& b : j["bookmarks"]) {
                        if (b.is_object() && b.contains("path") && b["path"].is_string()) {
                            GGPKExp::BookmarkEntry entry;
                            entry.path = b["path"].get<std::string>();
                            entry.iconIndex = b.value("icon", 0);
                            bookmarks.push_back(std::move(entry));
                        } else if (b.is_string()) {
                            bookmarks.push_back({ b.get<std::string>(), 0 });
                        }
                    }
                    m_Tree.SetBookmarks(std::move(bookmarks));
                }

                if (j.contains("search") && j["search"].is_object()) {
                    m_SearchDialog.LoadState(j["search"]);
                }
            } catch (...) {
                // Ignore parse errors, use defaults
            }
        }
    }

    // Clamp splitter
    if (m_SplitterRatio < 0.1f) m_SplitterRatio = 0.1f;
    if (m_SplitterRatio > 0.5f) m_SplitterRatio = 0.5f;
}

// ---------------------------------------------------------------------------
// ReadSmootherGGPKPath -- read GGPK path from Smoother's config files
// ---------------------------------------------------------------------------

std::string GGPKExplorer::ReadSmootherGGPKPath() {
    wchar_t exePathBuf[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(NULL, exePathBuf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};

    fs::path exeDir = fs::path(exePathBuf).parent_path();

    // 1. Try Configs/smoother/config.json -> "ggpkPathPoE2"
    {
        fs::path smootherCfg = exeDir / "Configs" / "smoother" / "config.json";
        if (fs::exists(smootherCfg)) {
            try {
                std::ifstream file{ smootherCfg };
                if (file.is_open()) {
                    json j = json::parse(file);
                    if (j.contains("ggpkPathPoE2") && j["ggpkPathPoE2"].is_string()) {
                        std::string path = j["ggpkPathPoE2"].get<std::string>();
                        if (!path.empty()) {
                            PluginLog(ctx(), "Info", "GGPKExplorer: found GGPK path from smoother config: %s", path.c_str());
                            return path;
                        }
                    }
                }
            } catch (...) {
                // Ignore parse errors
            }
        }
    }

    // 2. Try the program's general config, Configs/general/config.json -> "smootherGGPKPath"
    //    (POEFixer/core/AppPaths.h AppPaths::Config::General, written by Config::saveGeneral)
    {
        fs::path generalCfg = exeDir / "Configs" / "general" / "config.json";
        if (fs::exists(generalCfg)) {
            try {
                std::ifstream file{ generalCfg };
                if (file.is_open()) {
                    json j = json::parse(file);
                    if (j.contains("smootherGGPKPath") && j["smootherGGPKPath"].is_string()) {
                        std::string path = j["smootherGGPKPath"].get<std::string>();
                        if (!path.empty()) {
                            PluginLog(ctx(), "Info", "GGPKExplorer: found GGPK path from general config: %s", path.c_str());
                            return path;
                        }
                    }
                }
            } catch (...) {
                // Ignore parse errors
            }
        }
    }

    return {};
}

// ---------------------------------------------------------------------------
// Recursive expand/collapse helpers
// ---------------------------------------------------------------------------

void GGPKExplorer::ExpandAllRecursive(const std::string& path) {
    // The node, its ancestors and every folder below it, then one relinearize.
    m_Tree.ExpandSubtree(path);
}

void GGPKExplorer::CollapseAllRecursive(const std::string& path) {
    // The node and every folder below it, then one relinearize.
    m_Tree.CollapseSubtree(path);
}

// ---------------------------------------------------------------------------
// HandleContextMenuActions
// ---------------------------------------------------------------------------

void GGPKExplorer::HandleContextMenuActions() {
    // Copy Path to clipboard
    if (m_TreePanel.wantsCopyPath) {
        m_TreePanel.wantsCopyPath = false;
        const auto& path = m_TreePanel.GetSelectedPath();
        if (!path.empty()) {
            ImGui::SetClipboardText(path.c_str());
            PluginLog(ctx(), "Info", "GGPKExplorer: copied path: %s", path.c_str());
        }
    }

    // Copy Name to clipboard
    if (m_TreePanel.wantsCopyName) {
        m_TreePanel.wantsCopyName = false;
        const auto& path = m_TreePanel.GetSelectedPath();
        if (!path.empty()) {
            // Extract just the filename
            auto slash = path.rfind('/');
            std::string name = (slash != std::string::npos) ? path.substr(slash + 1) : path;
            ImGui::SetClipboardText(name.c_str());
            PluginLog(ctx(), "Info", "GGPKExplorer: copied name: %s", name.c_str());
        }
    }

    // Remove bookmark for selected items
    if (m_TreePanel.wantsRemoveBookmark) {
        m_TreePanel.wantsRemoveBookmark = false;
        const auto& flatList = m_Tree.GetFlatList();
        for (int idx : m_TreePanel.GetSelectedIndices()) {
            if (idx >= 0 && idx < static_cast<int>(flatList.size())) {
                m_Tree.RemoveBookmark(flatList[static_cast<uint32_t>(idx)].fullPath);
            }
        }
    }

    // Add bookmark with icon for selected items
    if (m_TreePanel.wantsAddBookmark) {
        m_TreePanel.wantsAddBookmark = false;
        int iconIdx = m_TreePanel.bookmarkIconChoice;
        const auto& flatList = m_Tree.GetFlatList();
        for (int idx : m_TreePanel.GetSelectedIndices()) {
            if (idx >= 0 && idx < static_cast<int>(flatList.size())) {
                m_Tree.SetBookmark(flatList[static_cast<uint32_t>(idx)].fullPath, iconIdx);
            }
        }
    }

    // Extract File
    if (m_TreePanel.wantsExtractFile) {
        m_TreePanel.wantsExtractFile = false;
        if (m_extractDialog) m_extractDialog->DoExtractFile();
    }

    // Replace File
    if (m_TreePanel.wantsReplaceFile) {
        m_TreePanel.wantsReplaceFile = false;
        if (m_extractDialog) m_extractDialog->DoReplaceFile();
    }

    // Extract Selected Files
    if (m_TreePanel.wantsExtractSelectedFiles) {
        m_TreePanel.wantsExtractSelectedFiles = false;
        if (m_extractDialog) m_extractDialog->DoExtractSelectedFiles();
    }

    // Save As (convert) — JSON/CSV/DDS/PNG
    if (m_TreePanel.wantsSaveAs) {
        m_TreePanel.wantsSaveAs = false;
        if (m_saveAsExporter && !m_saveAsExporter->IsRunning())
            m_saveAsExporter->SaveSelectionAs(m_TreePanel.saveAsFormat);
    }

    // Extract Directory
    if (m_TreePanel.wantsExtractDir) {
        m_TreePanel.wantsExtractDir = false;
        const auto& selPath = m_TreePanel.GetSelectedPath();
        if (!selPath.empty() && m_extractDialog) {
            m_extractDialog->OpenExtractOptions(selPath);
        }
    }

    // Expand All Children
    if (m_TreePanel.wantsExpandAll) {
        m_TreePanel.wantsExpandAll = false;
        const auto& path = m_TreePanel.GetSelectedPath();
        if (!path.empty()) {
            ExpandAllRecursive(path);
            PluginLog(ctx(), "Info", "GGPKExplorer: expanded all: %s", path.c_str());
        }
    }

    // Collapse All Children
    if (m_TreePanel.wantsCollapseAll) {
        m_TreePanel.wantsCollapseAll = false;
        const auto& path = m_TreePanel.GetSelectedPath();
        if (!path.empty()) {
            CollapseAllRecursive(path);
            PluginLog(ctx(), "Info", "GGPKExplorer: collapsed all: %s", path.c_str());
        }
    }

    // Open Search Dialog
    if (m_TreePanel.wantsSearch) {
        m_TreePanel.wantsSearch = false;
        auto scopePaths = m_TreePanel.GetSelectedPaths();
        m_SearchDialog.Open(scopePaths, m_TreePanel.wantsSearchFilesTab);
        m_TreePanel.wantsSearchFilesTab = false;
    }
}

// ---------------------------------------------------------------------------
// Plugin factory
// ---------------------------------------------------------------------------

extern "C" PLUGIN_API PluginSDK::Plugin* CreatePlugin() { return new GGPKExplorer(); }
extern "C" PLUGIN_API void DestroyPlugin(PluginSDK::Plugin* plugin) { delete plugin; }

// Plugin info: the host checks it before CreatePlugin; the publish tool reads it.
POEFIXER_PLUGIN_INFO(
    "GGPKExplorer",
    "GGPK Explorer",
    "1.0.4",
    "POEFixer",
    "Browses the game's content files: searchable file tree, viewers for tables, images, models and audio, and file extraction.")
