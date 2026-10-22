#include "GGPKExplorer.h"
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

// Helper: Convert wide string to UTF-8
static std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr || !wstr[0]) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string result(needed - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), needed, nullptr, nullptr);
    return result;
}

// Helper: Convert UTF-8 to wide string
static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(needed - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, result.data(), needed);
    return result;
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

    // Propagate to ContentPanel — which caches and forwards to MeshViewer/
    // AocViewer at viewer-init time (they don't exist until SetD3DDevice).
    m_ContentPanel.SetAnimPanelState(&m_uiSettings.animPanel);
    m_ContentPanel.SetPlaybackSpeedPtr(&m_uiSettings.playbackSpeed);
    m_ContentPanel.SetUiSettings(&m_uiSettings);
    m_ContentPanel.SetViewSettings(&m_uiSettings.meshView);
    m_ContentPanel.SetInitialPlaybackSpeed(m_uiSettings.playbackSpeed);
    m_ContentPanel.SetPluginDir(Directory());

    // Phase 7 / T23 — push persisted editor panel width. ContentPanel caches
    // it until DatViewer is lazily created (InitViewers picks it up there).
    m_ContentPanel.SetDatEditorPanelWidth(m_datEditorPanelWidth);

    // Create async task runner for non-blocking operations
    if (!m_taskRunner) {
        m_taskRunner = std::make_unique<GGPKExp::AsyncTaskRunner>(2);
    }

    // Load DAT schema
    if (!m_DatSchema.IsLoaded()) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        auto schemaPath = std::filesystem::path(exePath).parent_path() / "resources" / "dat-schema" / "schema.min.json";
        if (std::filesystem::exists(schemaPath)) {
            // Pass fs::path directly — schemaPath.string() throws std::filesystem_error
            // under the default C locale when the exe lives in a directory
            // with non-ASCII chars (Cyrillic etc.). DatSchema::Load now takes
            // an fs::path which keeps the native wide form.
            m_DatSchema.Load(schemaPath);
        }
    }

    // Pass schema and task runner to DatViewer inside ContentPanel
    m_ContentPanel.SetDatSchema(&m_DatSchema);
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
    m_saveAsExporter->SetSchema(&m_DatSchema);
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
            m_Manager.Close();
            m_Tree.Clear();
            m_NavBar.Clear();
            m_SelectedFilePath.clear();
            m_ContentPanel.Clear();
            m_isLoaded = false;
            m_ggpkLoadPending = true;
            m_openFuture = m_Manager.OpenAsync(m_GgpkPath, *m_taskRunner);
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
        if (!ggpkPath.empty() && std::filesystem::exists(ggpkPath)) {
            m_GgpkPath = ggpkPath;
        }
    }

    PluginLog(ctx(), "Info", "GGPKExplorer enabled");
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
    m_FileSearcher.Cancel();
    m_ContentSearcher.Cancel();

    // Wait for extract thread if running
    if (m_extractDialog) m_extractDialog->JoinExtractThread();
    m_extractDialog.reset();

    // Wait for Save As export thread if running
    if (m_saveAsExporter) {
        m_saveAsExporter->Cancel();
        m_saveAsExporter->JoinThread();
    }
    m_saveAsExporter.reset();

    // Wait for any pending async open to complete before closing
    if (m_openFuture.valid()) {
        m_openFuture.wait();
    }
    m_taskRunner.reset();

    m_Manager.Close();
    m_Tree.Clear();
    m_NavBar.Clear();
    PluginLog(ctx(), "Info", "GGPKExplorer disabled");
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
    if (!m_fa6Checked) {
        m_fa6Checked = true;
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

    // NOTE: UI settings debounced save lives AFTER ContentPanel::Render (see
    // below). Polling here would miss same-frame speed edits because the
    // viewer only commits the new speed during its Render() call.

    // ====================================================================
    // Async GGPK load — non-blocking, spinner while loading
    // ====================================================================
    if (m_ggpkLoadPending) {
        if (m_openFuture.valid()) {
            // Check if async open completed
            if (m_openFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                auto result = m_openFuture.get();
                m_ggpkLoadPending = false;
                if (result) {
                    // Post-load setup (same as the synchronous path in OpenGGPK)
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
                    m_SearchDialog.SetDatSchema(&m_DatSchema);
                    m_SearchDialog.SetFileSearcher(&m_FileSearcher);
                    m_SearchDialog.SetContentSearcher(&m_ContentSearcher);
                    m_SearchDialog.SetDatSearcher(&m_DatSearcher);
                    m_SearchDialog.SetOnResults([this](const GGPKExp::SearchResult& sr) {
                        m_ResultsPanel.ShowSearchResults(sr);
                    });
                    m_SearchDialog.SetOnReplaceLog([this](const std::vector<GGPKExp::ReplaceResult>& rr) {
                        m_ResultsPanel.ShowReplaceLog(rr);
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
                } else {
                    m_isLoaded = false;
                    PluginLog(ctx(), "Error", "GGPKExplorer: failed to open: %s", result.error.c_str());
                }
                return; // skip rest of rendering this frame
            }
        }

        // Still loading — draw spinner overlay
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
        ImVec2 textSize = ImGui::CalcTextSize("Loading...");
        drawList->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y + radius + 12.0f),
            IM_COL32(255, 255, 255, 255), "Loading...");
        return; // skip rest of rendering this frame
    }

    // ====================================================================
    // Async save — non-blocking, spinner while flushing writes
    // ====================================================================
    if (m_savePending) {
        if (m_saveFuture.valid() &&
            m_saveFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            m_saveFuture.get();
            m_savePending = false;
        } else {
            // Still saving — draw spinner overlay
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
            ImVec2 textSize = ImGui::CalcTextSize("Saving...");
            drawList->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y + radius + 12.0f),
                IM_COL32(255, 255, 255, 255), "Saving...");
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
        ImGui::Text("Path:");
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
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = nullptr;
            ofn.lpstrFilter = L"Game Files (*.ggpk;_.index.bin)\0*.ggpk;_.index.bin\0"
                              L"GGPK Files (*.ggpk)\0*.ggpk\0"
                              L"Index Files (_.index.bin)\0_.index.bin\0"
                              L"All Files (*.*)\0*.*\0";
            ofn.lpstrFile = filePath;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            ofn.lpstrTitle = L"Select Content.ggpk or _.index.bin";

            if (GetOpenFileNameW(&ofn)) {
                std::string utf8Path = WideToUtf8(filePath);
                if (!utf8Path.empty()) {
                    m_GgpkPath = utf8Path;
                }
            }
        }

        ImGui::SameLine();

        // Load button — async: submits to task runner, spinner renders while loading
        if (ImGui::Button("Load", ImVec2(100.0f, 0.0f)) && !m_ggpkLoadPending) {
            if (!m_GgpkPath.empty() && m_taskRunner) {
                // Game-running gate: PoE holds a lock on the GGPK file while the
                // process is alive. DISABLED 2026-09-04 --
                // on patch day the files have to come out of the new build while
                // the client is still up, and the gate refuses before anything is
                // even attempted. The lock is not always exclusive, and when it is,
                // the open fails on its own with a precise message
                // (lib/ggpk/Bundle.cpp:23, "file is locked by another process"),
                // which is strictly more informative than refusing up front.
                // Flip this back to true to restore the pre-emptive modal.
                static constexpr bool kBlockLoadWhileGameRuns = false;
                if (kBlockLoadWhileGameRuns && ctx()->Game.IsAttached()) {
                    m_ShowGameRunningModal = true;
                } else {
                    // Reset UI state before async open
                    m_Manager.Close();
                    m_Tree.Clear();
                    m_NavBar.Clear();
                    m_SelectedFilePath.clear();
                    m_ContentPanel.Clear();
                    m_StatusBar.fileCount = 0;
                    m_StatusBar.dirCount = 0;
                    m_StatusBar.totalSize = 0;
                    m_StatusBar.selectedName.clear();
                    m_StatusBar.selectedSize = -1;
                    m_StatusBar.progress.store(0.0f);
                    m_StatusBar.progressActive.store(false);
                    m_StatusBar.progressLabel.clear();
                    m_isLoaded = false;

                    // Initialize Oodle if needed
                    if (!m_OodleInitialized) {
                        m_OodleInitialized = GGPKExp::GGPKManager::InitOodle("");
                        if (m_OodleInitialized) {
                            PluginLog(ctx(), "Info", "GGPKExplorer: Oodle initialized");
                        }
                    }

                    m_ggpkLoadPending = true;
                    m_openFuture = m_Manager.OpenAsync(m_GgpkPath, *m_taskRunner);
                }
            }
        }

        // Unload button: adjacent to Load (no text between them)
        if (m_isLoaded && m_Manager.IsOpen()) {
            ImGui::SameLine();
            if (ImGui::Button("Unload", ImVec2(100.0f, 0.0f))) {
                m_Manager.Close();
                m_Tree.Clear();
                m_NavBar.Clear();
                m_ContentPanel.Clear();
                m_SelectedFilePath.clear();
                m_SelectedIsDirectory = false;
                m_StatusBar.fileCount = 0;
                m_StatusBar.dirCount = 0;
                m_StatusBar.totalSize = 0;
                m_StatusBar.selectedName.clear();
                m_StatusBar.selectedSize = -1;
                m_isLoaded = false;
            }
        }

        // 16px visual gap, then Bookmarks
        if (m_Manager.IsOpen()) {
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(16.0f, 0.0f));
            ImGui::SameLine();
            if (ImGui::Button("\xef\x80\xae Bookmarks")) {
                if (m_bookmarkMgr) m_bookmarkMgr->Open();
            }

            // Sort button (FontAwesome sort icon, U+F0DC)
            ImGui::SameLine();
            if (ImGui::Button("\xef\x83\x9c", ImVec2(28.0f, 0.0f))) {
                ImGui::OpenPopup("##GGPKSortPopup");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Sort");
            }

            if (ImGui::BeginPopup("##GGPKSortPopup")) {
                GGPKExp::SortMode current = m_Tree.GetSortMode();
                auto item = [&](const char* label, GGPKExp::SortMode mode) {
                    if (ImGui::MenuItem(label, nullptr, current == mode)) {
                        m_Tree.SetSortMode(mode);
                        SaveSettings();
                    }
                };
                item("None",             GGPKExp::SortMode::None);
                item("Name",             GGPKExp::SortMode::Name);
                item("Size",             GGPKExp::SortMode::Size);
                item("Type",             GGPKExp::SortMode::Type);
                item("Bookmarks first",  GGPKExp::SortMode::Bookmarks);
                ImGui::EndPopup();
            }
        }

        // Status text (Loading / Error / Not loaded) — rendered after all buttons
        ImGui::SameLine();
        if (m_ggpkLoadPending) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "Loading...");
        } else if (!m_Manager.IsOpen() && !m_Manager.GetError().empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error");
        } else if (!m_Manager.IsOpen()) {
            ImGui::TextDisabled("Not loaded");
        }

        // ------------------------------------------------------------------
        // Game-running warning modal (paired with the Load button gate above).
        // ------------------------------------------------------------------
        if (m_ShowGameRunningModal) {
            ImGui::OpenPopup("##GGPKGameRunning");
            m_ShowGameRunningModal = false;
        }
        if (ImGui::BeginPopupModal("##GGPKGameRunning", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextWrapped(
                "Path of Exile is currently running.\n"
                "Close the game before loading GGPK data \xE2\x80\x94 "
                "the file is locked while the game is running.");
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
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
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Failed to open:");
                ImGui::SetCursorPosX(10.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", openError.c_str());
                ImGui::Spacing();
                ImGui::SetCursorPosX(10.0f);
                ImGui::TextDisabled("Try opening a different .ggpk / _.index.bin file or game folder.");
            } else {
                const char* msg = "Open a .ggpk, _.index.bin or game folder to browse files";
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
        // (see SetPlaybackSpeedPtr wiring). Explicitly re-reading it from
        // GetCurrentPlaybackSpeed() is not only redundant but a regression path
        // when the dirty-source is NOT a Mesh/Aoc viewer (e.g. PSG language
        // combo): GetCurrentPlaybackSpeed() returns the hardcoded 1.0f default
        // for non-mesh/aoc types, stomping the user's saved speed.
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

    // ====================================================================
    // Quick Jump popup (Ctrl+P)
    // ====================================================================
    if (m_quickJump) {
        m_quickJump->Render();
    }

    // ====================================================================
    // Auto-flush: if any writes are pending, start async save with spinner
    // ====================================================================
    if (!m_savePending && m_Manager.HasPendingWrites()) {
        m_savePending = true;
        m_saveFuture = std::async(std::launch::async, [this]() {
            return m_Manager.FlushWrites();
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
    j["showWindow"] = m_ShowWindow;
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

    // Try new JSON format first
    if (fs::exists(settingsPath)) {
        std::ifstream file{ settingsPath };
        if (file.is_open()) {
            try {
                json j = json::parse(file);

                if (j.contains("showWindow"))    m_ShowWindow = j["showWindow"].get<bool>();
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
    } else {
        // Try legacy settings.txt (from previous version)
        fs::path legacyPath = DirectoryPath() / "config" / "settings.txt";
        if (fs::exists(legacyPath)) {
            std::ifstream file{ legacyPath };
            std::string line;
            while (std::getline(file, line)) {
                auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);
                if (key == "ShowWindow") m_ShowWindow = (val == "1");
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

    // 2. Try Configs/general/general_config.json -> "smootherGGPKPath"
    {
        fs::path generalCfg = exeDir / "Configs" / "general" / "general_config.json";
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
// OpenGGPK
// ---------------------------------------------------------------------------

void GGPKExplorer::OpenGGPK(const std::string& path) {
    if (path.empty()) {
        PluginLog(ctx(), "Error", "GGPKExplorer: empty path");
        return;
    }

    // Initialize Oodle if not already loaded
    if (!m_OodleInitialized) {
        m_OodleInitialized = GGPKExp::GGPKManager::InitOodle("");
        if (m_OodleInitialized) {
            PluginLog(ctx(), "Info", "GGPKExplorer: Oodle initialized");
        }
    }

    // Close previous
    m_Manager.Close();
    m_Tree.Clear();
    m_NavBar.Clear();
    m_SelectedFilePath.clear();
    m_ContentPanel.Clear();

    // Reset status bar (atomics are not assignable, so reset each field)
    m_StatusBar.fileCount = 0;
    m_StatusBar.dirCount = 0;
    m_StatusBar.totalSize = 0;
    m_StatusBar.selectedName.clear();
    m_StatusBar.selectedSize = -1;
    m_StatusBar.progress.store(0.0f);
    m_StatusBar.progressActive.store(false);
    m_StatusBar.progressLabel.clear();

    // Open
    auto openResult = m_Manager.Open(path);
    if (!openResult) {
        PluginLog(ctx(), "Error", "GGPKExplorer: failed to open: %s", m_Manager.GetError().c_str());
        return;
    }

    {
        const char* modeStr = (m_Manager.GetMode() == GGPKExp::SourceMode::StandaloneGGPK) ? "GGPK" : "Bundles";
        PluginLog(ctx(), "Info", "GGPKExplorer: opened %s (mode: %s)", path.c_str(), modeStr);
    }

    // Restore bookmarks into tree before building (they persist across loads)
    auto savedBookmarks = m_Tree.GetBookmarks();

    // Build virtual tree
    m_Tree.BuildFromManager(m_Manager);

    // Restore bookmarks
    if (!savedBookmarks.empty()) {
        m_Tree.SetBookmarks(std::move(savedBookmarks));
    }

    // Connect content panel to manager
    m_ContentPanel.SetManager(&m_Manager);
    m_ContentPanel.SetVirtualTree(&m_Tree);

    // Provide D3D device for image viewer
    if (ctx()->D3DDevice) {
        m_ContentPanel.SetD3DDevice(static_cast<ID3D11Device*>(ctx()->D3DDevice));
    }

    // Connect tree panel
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

    // Connect search system
    m_SearchDialog.SetTree(&m_Tree);
    m_SearchDialog.SetManager(&m_Manager);
    m_SearchDialog.SetDatSchema(&m_DatSchema);
    m_SearchDialog.SetFileSearcher(&m_FileSearcher);
    m_SearchDialog.SetContentSearcher(&m_ContentSearcher);
    m_SearchDialog.SetDatSearcher(&m_DatSearcher);
    m_SearchDialog.SetOnResults([this](const GGPKExp::SearchResult& result) {
        m_ResultsPanel.ShowSearchResults(result);
    });
    m_SearchDialog.SetOnReplaceLog([this](const std::vector<GGPKExp::ReplaceResult>& results) {
        m_ResultsPanel.ShowReplaceLog(results);
    });
    m_SearchDialog.SetOnNavigateToLine([this](const std::string& path, int line) {
        NavigateToPath(path);
        m_ContentPanel.ScrollToLine(line);
    });
    m_ResultsPanel.SetOnNavigate([this](const std::string& path, int line, bool isDatRow) {
        NavigateToPath(path);
        if (line >= 0) {
            if (isDatRow)
                m_ContentPanel.ScrollToRow(line);
            else
                m_ContentPanel.ScrollToLine(line);
        }
    });

    // Update status bar
    m_StatusBar.fileCount = m_Tree.GetFileCount();
    m_StatusBar.dirCount = m_Tree.GetDirCount();
    m_StatusBar.totalSize = m_Tree.GetTotalSize();
}

// ---------------------------------------------------------------------------
// Recursive expand/collapse helpers
// ---------------------------------------------------------------------------

void GGPKExplorer::ExpandAllRecursive(const std::string& path) {
    // ExpandTo already expands all ancestors, but we need to expand
    // the node itself AND all its descendants.
    m_Tree.ExpandTo(path);

    // Now iterate the flat list and expand every directory that is a descendant
    // We need to loop because expanding a node changes the flat list.
    // Strategy: keep expanding until no more unexpanded directories found.
    std::string prefix = path + "/";
    bool changed = true;
    while (changed) {
        changed = false;
        const auto& flatList = m_Tree.GetFlatList();
        for (uint32_t i = 0; i < static_cast<uint32_t>(flatList.size()); ++i) {
            const auto& e = flatList[i];
            if (!e.isDirectory || e.isExpanded) continue;
            if (e.fullPath == path || e.fullPath.compare(0, prefix.size(), prefix) == 0) {
                m_Tree.ToggleExpand(i);
                changed = true;
                break; // flat list invalidated, restart
            }
        }
    }
}

void GGPKExplorer::CollapseAllRecursive(const std::string& path) {
    // Collapse all expanded descendants first, then the node itself.
    // Iterate from the bottom up to avoid index invalidation issues.
    std::string prefix = path + "/";
    bool changed = true;
    while (changed) {
        changed = false;
        const auto& flatList = m_Tree.GetFlatList();
        for (uint32_t i = static_cast<uint32_t>(flatList.size()); i > 0; --i) {
            uint32_t idx = i - 1;
            const auto& e = flatList[idx];
            if (!e.isDirectory || !e.isExpanded) continue;
            if (e.fullPath == path || e.fullPath.compare(0, prefix.size(), prefix) == 0) {
                m_Tree.ToggleExpand(idx);
                changed = true;
                break; // flat list invalidated, restart
            }
        }
    }
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
    "1.0.0",
    "POEFixer",
    "Browses the game's content files: searchable file tree, viewers for tables, images, models and audio, and file extraction.")
