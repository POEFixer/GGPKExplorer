// Plugins/GGPKExplorer/ui/SearchDialog.h
#pragma once
#include "../core/SearchTypes.h"
#include "../core/FileSearcher.h"
#include "../core/ContentSearcher.h"
#include "../core/DatSearcher.h"
#include "../core/VirtualTree.h"
#include "../core/GGPKManager.h"
#include "../core/DatSchema.h"
#include "../../lib/json.hpp"
#include "UiText.h"
#include <string>
#include <vector>
#include <functional>

namespace GGPKExp {

class SearchDialog {
public:
    using ResultCallback = std::function<void(const SearchResult& result)>;
    using ReplaceLogCallback = std::function<void(const std::vector<ReplaceResult>& results)>;
    using LogCallback = std::function<void(const char* category, const char* message)>;

    void SetTree(VirtualTree* tree) { m_tree = tree; }
    void SetManager(GGPKManager* mgr) { m_mgr = mgr; }
    void SetDatSchema(DatSchema* schema) { m_schema = schema; }
    void SetFileSearcher(FileSearcher* fs) { m_fileSearcher = fs; }
    void SetContentSearcher(ContentSearcher* cs) { m_contentSearcher = cs; }
    void SetDatSearcher(DatSearcher* ds) { m_datSearcher = ds; }

    void SetOnResults(ResultCallback cb) { m_onResults = std::move(cb); }
    void SetOnReplaceLog(ReplaceLogCallback cb) { m_onReplaceLog = std::move(cb); }
    void SetLogCallback(LogCallback cb) { m_log = std::move(cb); }

    /// The archive closes: a replace preview waiting for confirmation and the Find Next
    /// position name files of it, so they are dropped.
    void DiscardPending();

    void SaveState(nlohmann::json& j) const;
    void LoadState(const nlohmann::json& j);

    // Open dialog with scope (paths from tree selection)
    void Open(const std::vector<std::string>& scopePaths, bool openFindReplaceTab = false);
    // Open with specific tab and scope (for Ctrl+F / Ctrl+R)
    void Open(int tab, const std::string& scopePath, bool isFileScope);
    void Close();
    bool IsOpen() const { return m_isOpen; }

    void Render();

    // Navigate-to-line callback (set by host to open file + scroll)
    using NavigateToLineCallback = std::function<void(const std::string& path, int line)>;
    void SetOnNavigateToLine(NavigateToLineCallback cb) { m_onNavigateToLine = std::move(cb); }

private:
    bool m_isOpen = false;
    int m_activeTab = 0; // 0 = Find File, 1 = Find/Replace in Files

    // Dynamic window resize tracking (Task 6 — auto-grow for progress bar)
    bool m_progressVisibleLastFrame = false;
    bool m_userResizedHeight = false;
    float m_lastWindowWidth = 620.0f;
    float m_lastRenderedHeight = 520.0f;
    bool m_firstRender = true;
    float m_measuredContentHeight = 520.0f;   // last-measured content height (for auto-fit)

    // Shared state
    // All non-owning, lifetime guaranteed by GGPKExplorer
    VirtualTree* m_tree = nullptr;
    GGPKManager* m_mgr = nullptr;
    DatSchema* m_schema = nullptr;
    FileSearcher* m_fileSearcher = nullptr;
    ContentSearcher* m_contentSearcher = nullptr;
    DatSearcher* m_datSearcher = nullptr;
    std::vector<std::string> m_scopePaths;
    bool m_isScopedSearch = false;  // true when opened via Ctrl+F/R with specific scope

    // Callbacks
    ResultCallback m_onResults;
    ReplaceLogCallback m_onReplaceLog;
    LogCallback m_log;

    // A Replace in Files job runs on the content searcher's worker; its outcome is taken
    // (PollReplace, every frame) once it ended.
    bool m_replacePending = false;
    void PollReplace();
    void Log(const char* category, const std::string& message) const;

    // Find File tab state
    char m_findFileBuf[256] = {};
    char m_findFileFilterBuf[256] = {};
    bool m_searchFileNames = true;
    bool m_searchFolderNames = true;
    bool m_searchByPath = false;
    bool m_ffMatchWholeWord = false;
    bool m_ffMatchCase = false;
    bool m_ffInSubFolders = true;
    int m_ffSearchMode = 0; // 0=Normal, 1=Extended, 2=Regex
    bool m_ffResultDelivered = false; // one-shot result delivery flag

    // Find/Replace in Files tab state
    char m_findContentBuf[256] = {};
    char m_replaceBuf[256] = {};
    int m_filterPresetIndex = 1; // default "All text + datc64"
    char m_customFilterBuf[512] = {};
    bool m_frMatchWholeWord = false;
    bool m_frMatchCase = false;
    bool m_frInSubFolders = true;
    int m_frSearchMode = 0; // 0=Normal, 1=Extended, 2=Regex
    bool m_frDotMatchesNewline = false;
    bool m_frResultDelivered = false; // one-shot result delivery flag

    // Search history
    std::vector<std::string> m_searchHistory;
    static constexpr int MAX_HISTORY = 20;
    std::vector<std::string> m_replaceHistory;

    // Replace preview state
    bool m_showPreview = false;
    std::vector<ReplacementPreview> m_previewData;

    // Navigate callback
    NavigateToLineCallback m_onNavigateToLine;

    // Find Next state
    struct FindNextState {
        std::string query;
        bool caseSensitive = false;
        std::string scopePath;
        bool isFileScope = false;

        std::string currentFile;
        int currentLine = -1;
        int currentCol = -1;
        std::vector<std::string> fileList;
        int currentFileIndex = 0;
        bool initialized = false;
    };
    FindNextState m_findNext;

    void DoFindNext();
    bool FindNextInFile(const std::string& filePath, int startLine, int startCol,
                        int& outLine, int& outCol);
    void CollectTextFiles(const std::string& folderPath, std::vector<std::string>& out);

    // Filter presets: the name is a lang/ key, looked up when drawn.
    struct FilterPreset { const char* nameKey; const char* value; };
    static constexpr FilterPreset FILTER_PRESETS[] = {
        {I18n::Key("search_preset_all_files"), "*.*"},
        {I18n::Key("search_preset_text_and_datc64"), "*.txt *.json *.xml *.cfg *.csv *.datc64 *.shader *.hlsl *.glsl *.lua *.ini *.csd"},
        {I18n::Key("search_preset_text_only"), "*.txt *.json *.xml *.cfg *.csv *.ini"},
        {I18n::Key("search_preset_data_files"), "*.datc64 *.dat"},
        {I18n::Key("search_preset_shaders"), "*.shader *.hlsl *.glsl"},
        {I18n::Key("search_preset_custom"), ""},
    };
    static constexpr int PRESET_COUNT = 6;

    void RenderFindFileTab();
    void RenderFindReplaceTab();
    void RenderReplacePreview();
    void AddToHistory(const std::string& query);
    bool AreAllScopePathsFiles() const;
    SearchOptions BuildFindFileOptions() const;
    SearchOptions BuildContentOptions() const;
    std::string GetCurrentFilter() const;
    void DoFindFiles();
    void DoFindInFiles();
    void DoReplaceInFiles();
    void DoApplyPreview();

    static bool RenderHistoryInput(const char* id, char* buf, int bufSize,
                                   std::vector<std::string>& history, float inputW);
    static void AddToHistoryList(std::vector<std::string>& history, const std::string& entry);
};

} // namespace GGPKExp
