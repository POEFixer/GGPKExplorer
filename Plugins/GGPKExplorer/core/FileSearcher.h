// Plugins/GGPKExplorer/core/FileSearcher.h
#pragma once
#include "SearchTypes.h"
#include "VirtualTree.h"
#include "GGPKManager.h"
#include <thread>
#include <mutex>
#include <atomic>

namespace GGPKExp {

class SearchQuery;

class FileSearcher {
public:
    ~FileSearcher();

    void Start(const SearchOptions& opts, VirtualTree* tree,
               bool searchFiles, bool searchFolders, bool searchByPath = false);
    void Cancel();
    /// Cancel and wait for the worker: before the tree is cleared.
    void CancelAndJoin();
    bool IsRunning() const { return m_running.load(); }
    float GetProgress() const { return m_progress.load(); }
    SearchResult GetResult();

private:
    void WorkerThread(SearchOptions opts, bool searchFiles, bool searchFolders, bool searchByPath);
    void Search(const SearchOptions& opts, bool searchFiles, bool searchFolders, bool searchByPath,
                SearchResult& result);
    bool MatchesFilter(const std::string& name, const std::string& filters) const;

    std::thread m_worker;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_running{false};
    std::atomic<float> m_progress{0.0f};
    std::mutex m_resultMutex;
    SearchResult m_result;
    VirtualTree* m_tree = nullptr;  // non-owning, lifetime guaranteed by caller
};

} // namespace GGPKExp
