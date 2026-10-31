// Plugins/GGPKExplorer/core/ContentSearcher.h
#pragma once
#include "SearchTypes.h"
#include "VirtualTree.h"
#include "GGPKManager.h"
#include "DatSearcher.h"
#include "DatSchema.h"
#include <thread>
#include <mutex>
#include <atomic>

namespace GGPK { class FileRecord; }

namespace GGPKExp {

class SearchQuery;

/// Find (and replace) in the contents of the files of a scope, on one worker thread. The
/// files are read through the index's batch reader (grouped by bundle, a few readers) and
/// searched as they arrive; results come in tree order. The query is prepared once per job
/// and owned by the worker (Cancel never touches it).
class ContentSearcher {
public:
    ~ContentSearcher();

    void StartFind(const SearchOptions& opts, VirtualTree* tree, GGPKManager* mgr,
                   DatSearcher* datSearcher, DatSchema* schema);

    /// Replace in the files of the scope: text files are rewritten in the archive (pending
    /// until the next flush), .datc64 matches come back as previews to confirm. The outcome
    /// is collected with TakeReplaceOutcome once the job ended.
    void StartReplace(const SearchOptions& opts, VirtualTree* tree, GGPKManager* mgr,
                      DatSearcher* datSearcher, DatSchema* schema);

    void Cancel();
    /// Cancel and wait for the worker: before the archive closes or the tree is cleared.
    void CancelAndJoin();
    bool IsRunning() const { return m_running.load(); }
    float GetProgress() const { return m_progress.load(); }
    SearchResult GetResult();

    struct ReplaceOutcome {
        std::vector<ReplaceResult> replaced;            // text files rewritten (tree order)
        std::vector<ReplacementPreview> datPreviews;    // .datc64 matches to confirm
        std::vector<std::string> writeErrors;           // "<path>: <why>" (not rewritten)
        std::string error;                              // the job itself failed
        bool cancelled = false;
    };
    /// The outcome of the last StartReplace that ended, once (false: none waiting).
    bool TakeReplaceOutcome(ReplaceOutcome& out);

private:
    // One file of the scope: the tree node's own path and name strings (stable while the
    // tree lives, which outlives the job) and its record.
    struct ScopeFile {
        const std::string* path = nullptr;
        const std::string* name = nullptr;
        GGPK::FileRecord* record = nullptr;
    };

    bool Launch(void (ContentSearcher::*body)(SearchOptions), const SearchOptions& opts,
                VirtualTree* tree, GGPKManager* mgr, DatSearcher* datSearcher, DatSchema* schema);
    void FindWorker(SearchOptions opts);
    void ReplaceWorker(SearchOptions opts);
    void Find(const SearchOptions& opts, SearchResult& result);
    void Replace(const SearchOptions& opts, ReplaceOutcome& outcome);
    std::vector<ScopeFile> CollectFiles(const SearchOptions& opts, bool directChildrenRule) const;
    bool SkipUnreadDat(const ScopeFile& file) const;

    std::vector<ContentMatch> SearchInText(const std::string& text, const std::string& path,
                                            const SearchQuery& query) const;
    std::string ReplaceInText(const std::string& text, const SearchQuery& query, int& count) const;
    bool MatchesFilter(const std::string& name, const std::string& filters) const;
    bool IsDatc64(const std::string& name) const;

    std::thread m_worker;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_running{false};
    std::atomic<float> m_progress{0.0f};
    std::mutex m_resultMutex;
    SearchResult m_result;
    ReplaceOutcome m_replaceOutcome;
    bool m_replaceOutcomeReady = false;
    // All non-owning, lifetime guaranteed by GGPKExplorer
    VirtualTree* m_tree = nullptr;
    GGPKManager* m_mgr = nullptr;
    DatSearcher* m_datSearcher = nullptr;
    DatSchema* m_schema = nullptr;

    static constexpr size_t MAX_FILE_SIZE = 50 * 1024 * 1024; // 50MB
};

} // namespace GGPKExp
