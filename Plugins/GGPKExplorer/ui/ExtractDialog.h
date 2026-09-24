#pragma once
#include "../core/GGPKManager.h"
#include "../core/VirtualTree.h"
#include "StatusBar.h"
#include "TreePanel.h"
#include "ContentPanel.h"
#include <string>
#include <vector>
#include <set>
#include <thread>
#include <atomic>
#include <functional>
#include <imgui.h>

namespace GGPKExp {

struct ExtractFilter {
    std::string extension;
    int count = 0;
    bool included = true;
};

struct ExtractOptions {
    std::vector<ExtractFilter> filters;
    bool skipLargeFiles = false;
    int64_t maxFileSize = 500;
    int sizeUnit = 0;  // 0=KB, 1=MB, 2=GB
    std::string targetDirPath;
};

/// Handles all extract/replace operations and the Extract Options modal.
class ExtractDialog {
public:
    using LogCallback = std::function<void(const char* category, const char* message)>;

    void SetManager(GGPKManager* mgr) { m_manager = mgr; }
    void SetTree(VirtualTree* tree) { m_tree = tree; }
    void SetTreePanel(TreePanel* panel) { m_treePanel = panel; }
    void SetContentPanel(ContentPanel* contentPanel) { m_contentPanel = contentPanel; }
    void SetStatusBar(StatusBarState* bar) { m_statusBar = bar; }
    void SetLogCallback(LogCallback cb) { m_log = std::move(cb); }

    /// Open the Extract Options modal for a directory.
    void OpenExtractOptions(const std::string& dirPath);
    bool IsExtractOptionsOpen() const { return m_showExtractOptions; }

    /// Render the Extract Options modal (call every frame).
    void RenderExtractOptions();

    /// Is an extraction currently running?
    bool IsExtractRunning() const { return m_extractRunning.load(); }

    /// Cancel a running extraction.
    void CancelExtract() { m_extractRunning.store(false); }

    /// Wait for the extract thread to finish and join it.
    void JoinExtractThread();

    // Individual extract/replace operations
    void DoExtractFile();
    void DoReplaceFile();
    void DoExtractDirectory();
    void DoExtractSelectedFiles();

private:
    void CollectExtensionStats(const std::string& dirPath);
    void ExtractDirectoryFiltered();

    // All non-owning, lifetime guaranteed by GGPKExplorer
    GGPKManager* m_manager = nullptr;
    VirtualTree* m_tree = nullptr;
    TreePanel* m_treePanel = nullptr;
    ContentPanel* m_contentPanel = nullptr;
    StatusBarState* m_statusBar = nullptr;
    LogCallback m_log;

    bool m_showExtractOptions = false;
    ExtractOptions m_extractOptions;
    std::thread m_extractThread;
    std::atomic<bool> m_extractRunning{ false };
};

} // namespace GGPKExp
