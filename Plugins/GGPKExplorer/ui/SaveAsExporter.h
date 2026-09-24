#pragma once
#include "SaveFormat.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace GGPKExp {

class GGPKManager;
class VirtualTree;
class TreePanel;
struct StatusBarState;
class DatSchema;

/// Tree-level "Save As" exporter: converts selected GGPK files to a chosen
/// on-disk format. Single file -> Save dialog; many/folder -> folder picker with
/// GGPK-structure mirroring. Conversion runs on a background thread.
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

    bool IsRunning() const { return m_running.load(); }
    void Cancel()          { m_running.store(false); }
    void JoinThread();
    ~SaveAsExporter() { Cancel(); JoinThread(); }

private:
    struct Target { void* nodePtr = nullptr; std::string ggpkPath; };

    static const char*    RequiredExt(SaveFormat); // ".datc64" | ".dds"
    static const wchar_t* OutExtW(SaveFormat);     // L".json" | L".csv" | L".dds" | L".png"
    static const char*    FormatName(SaveFormat);  // "JSON" | "CSV" | "DDS" | "PNG"
    static const wchar_t* FormatNameW(SaveFormat);

    void CollectTargets(SaveFormat fmt, std::vector<Target>& out, bool& sawNonMatching) const;
    void RunExport(SaveFormat fmt, std::vector<Target> targets,
                   std::filesystem::path destRoot, std::filesystem::path singleOut);

    GGPKManager*    m_manager   = nullptr;
    VirtualTree*    m_tree      = nullptr;
    TreePanel*      m_treePanel = nullptr;
    StatusBarState* m_statusBar = nullptr;
    DatSchema*      m_schema    = nullptr;
    LogCallback     m_log;
    std::thread     m_thread;
    std::atomic<bool> m_running{ false };
};

} // namespace GGPKExp
