#pragma once
#include "../core/GGPKManager.h"
#include "../core/VirtualTree.h"
#include "../core/ExtractJob.h"
#include "StatusBar.h"
#include "TreePanel.h"
#include "ContentPanel.h"
#include <string>
#include <vector>
#include <memory>
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

/// Handles all extract/replace operations and the Extract Options modal. A folder or
/// selection extraction runs as an ExtractJob (core/ExtractJob.h) on its own threads.
class ExtractDialog {
public:
    using LogCallback = std::function<void(const char* category, const char* message)>;

    ~ExtractDialog();

    void SetManager(GGPKManager* mgr) { m_manager = mgr; }
    void SetTree(VirtualTree* tree) { m_tree = tree; }
    void SetTreePanel(TreePanel* panel) { m_treePanel = panel; }
    void SetContentPanel(ContentPanel* contentPanel) { m_contentPanel = contentPanel; }
    void SetStatusBar(StatusBarState* bar) { m_statusBar = bar; }
    void SetLogCallback(LogCallback cb) { m_log = std::move(cb); }

    /// Open the Extract Options modal for a directory.
    void OpenExtractOptions(const std::string& dirPath);

    /// Render the Extract Options modal and pass the running job's progress to the status
    /// bar (call every frame).
    void RenderExtractOptions();

    /// Is an extraction currently running?
    bool IsExtractRunning() const { return m_job && m_job->IsRunning(); }

    /// Ask a running extraction to stop (the Cancel button); it ends on its own thread.
    void CancelExtract() { if (m_job) m_job->Cancel(); }

    /// Cancel a running extraction and wait for it: before the archive closes.
    void JoinExtractThread();

    // Individual extract/replace operations
    void DoExtractFile();
    void DoReplaceFile();
    void DoExtractSelectedFiles();

private:
    void CollectExtensionStats(const std::string& dirPath);
    void ExtractDirectoryFiltered();
    std::wstring BrowseForOutputFolder(const char* titleKey) const;
    void StartJob(std::vector<ExtractJob::Item> items, std::wstring outputDir,
                  int64_t skipLargerThan, const char* progressKey);

    // All non-owning, lifetime guaranteed by GGPKExplorer
    GGPKManager* m_manager = nullptr;
    VirtualTree* m_tree = nullptr;
    TreePanel* m_treePanel = nullptr;
    ContentPanel* m_contentPanel = nullptr;
    StatusBarState* m_statusBar = nullptr;
    LogCallback m_log;

    bool m_showExtractOptions = false;
    ExtractOptions m_extractOptions;
    std::unique_ptr<ExtractJob> m_job;
};

} // namespace GGPKExp
