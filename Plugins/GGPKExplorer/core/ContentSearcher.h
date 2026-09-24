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
#include <optional>
#include <regex>

namespace GGPKExp {

class ContentSearcher {
public:
    ~ContentSearcher();

    void StartFind(const SearchOptions& opts, VirtualTree* tree, GGPKManager* mgr,
                   DatSearcher* datSearcher, DatSchema* schema);
    void Cancel();
    bool IsRunning() const { return m_running.load(); }
    float GetProgress() const { return m_progress.load(); }
    SearchResult GetResult();

    // Direct replace (text files only; datc64 entries returned in outDatPreviews)
    std::vector<ReplaceResult> ReplaceInFiles(const SearchOptions& opts, VirtualTree* tree,
                                               GGPKManager* mgr, DatSearcher* datSearcher,
                                               DatSchema* schema,
                                               std::vector<ReplacementPreview>& outDatPreviews);

    // Preview for all file types
    std::vector<ReplacementPreview> BuildPreview(const SearchOptions& opts, VirtualTree* tree,
                                                  GGPKManager* mgr, DatSearcher* datSearcher,
                                                  DatSchema* schema);

    // Apply confirmed preview
    std::vector<ReplaceResult> ApplyPreview(const std::vector<ReplacementPreview>& preview,
                                             GGPKManager* mgr, DatSearcher* datSearcher,
                                             DatSchema* schema);

private:
    void WorkerThread(SearchOptions opts);
    std::vector<ContentMatch> SearchInText(const std::string& text, const std::string& path,
                                            const SearchOptions& opts) const;
    std::string ReplaceInText(const std::string& text, const SearchOptions& opts, int& count) const;
    bool MatchesFilter(const std::string& name, const std::string& filters) const;
    bool IsUnderScope(const std::string& path, const SearchOptions& opts) const;
    bool IsDatc64(const std::string& name) const;
    std::string ExpandExtended(const std::string& pattern) const;
    const std::regex& GetOrCompileRegex(const std::string& pattern, bool caseSensitive) const;
    void ClearRegexCache();

    std::thread m_worker;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_running{false};
    std::atomic<float> m_progress{0.0f};
    std::mutex m_resultMutex;
    SearchResult m_result;
    // All non-owning, lifetime guaranteed by GGPKExplorer
    VirtualTree* m_tree = nullptr;
    GGPKManager* m_mgr = nullptr;
    DatSearcher* m_datSearcher = nullptr;
    DatSchema* m_schema = nullptr;

    // Regex compiled once per search, reused across all files
    mutable std::optional<std::regex> m_cachedRegex;
    mutable std::string m_cachedPattern;
    mutable bool m_cachedCaseFlag = false;

    static constexpr size_t MAX_FILE_SIZE = 50 * 1024 * 1024; // 50MB
};

} // namespace GGPKExp
