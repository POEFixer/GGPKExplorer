#pragma once
#include "SaveFormat.h"
#include "../core/ExtractJob.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GGPKExp {

class GGPKManager;
class VirtualTree;
class TreePanel;
struct StatusBarState;
class DatSchema;

/// Tree-level "Save As" exporter: converts selected GGPK files to a chosen
/// on-disk format. Single file -> Save dialog; many/folder -> folder picker with
/// GGPK-structure mirroring. Runs as an ExtractJob: reads grouped by bundle, a small pool
/// converts (DDS -> PNG, DAT -> JSON/CSV), one writer writes.
class SaveAsExporter {
public:
    using LogCallback = std::function<void(const char* category, const char* message)>;

    void SetManager(GGPKManager* m)      { m_manager = m; }
    void SetTree(VirtualTree* t)         { m_tree = t; }
    void SetTreePanel(TreePanel* p)      { m_treePanel = p; }
    void SetStatusBar(StatusBarState* s) { m_statusBar = s; }
    void SetSchema(DatSchema* s)         { m_schema = s; }
    void SetLogCallback(LogCallback cb)  { m_log = std::move(cb); }

    void SaveSelectionAs(SaveFormat fmt);

    /// Pass the running export's progress to the status bar (call every frame).
    void UpdateProgress();

    bool IsRunning() const { return m_job && m_job->IsRunning(); }
    /// Ask a running export to stop (the Cancel button); it ends on its own thread.
    void Cancel()          { if (m_job) m_job->Cancel(); }
    /// Cancel and wait: before the archive closes.
    void CancelAndJoin()   { if (m_job) m_job->CancelAndJoin(); }
    ~SaveAsExporter() { CancelAndJoin(); }

private:
    static const char*    RequiredExt(SaveFormat); // ".datc64" | ".dds"
    static const wchar_t* OutExtW(SaveFormat);     // L".json" | L".csv" | L".dds" | L".png"
    static const char*    FormatName(SaveFormat);  // "JSON" | "CSV" | "DDS" | "PNG"

    void CollectTargets(SaveFormat fmt, std::vector<ExtractJob::Item>& out, bool& sawNonMatching) const;
    void RunExport(SaveFormat fmt, std::vector<ExtractJob::Item> targets,
                   std::filesystem::path destRoot, std::filesystem::path singleOut);

    GGPKManager*    m_manager   = nullptr;
    VirtualTree*    m_tree      = nullptr;
    TreePanel*      m_treePanel = nullptr;
    StatusBarState* m_statusBar = nullptr;
    DatSchema*      m_schema    = nullptr;
    LogCallback     m_log;
    std::unique_ptr<ExtractJob> m_job;
};

} // namespace GGPKExp
