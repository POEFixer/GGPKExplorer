// Plugins/GGPKExplorer/core/FileSearcher.cpp
#include "FileSearcher.h"
#include "SearchQuery.h"
#include <algorithm>
#include <cctype>

namespace GGPKExp {

FileSearcher::~FileSearcher() {
    CancelAndJoin();
}

bool FileSearcher::MatchesFilter(const std::string& name, const std::string& filters) const {
    if (filters.empty() || filters == "*.*" || filters == "*") return true;

    // Extract extension from name
    auto dot = name.rfind('.');
    std::string ext = (dot != std::string::npos) ? SearchToLower(name.substr(dot)) : "";

    // Parse filters: "*.txt *.json *.datc64" split by space
    std::string remaining = filters;
    size_t pos = 0;
    while (pos < remaining.size()) {
        size_t space = remaining.find(' ', pos);
        std::string mask = (space != std::string::npos) ? remaining.substr(pos, space - pos) : remaining.substr(pos);
        pos = (space != std::string::npos) ? space + 1 : remaining.size();

        if (mask.empty()) continue;
        if (mask == "*.*" || mask == "*") return true;

        // mask is like "*.txt" — extract the extension part
        auto maskDot = mask.rfind('.');
        if (maskDot != std::string::npos) {
            std::string maskExt = SearchToLower(mask.substr(maskDot));
            if (ext == maskExt) return true;
        }
    }
    return false;
}

void FileSearcher::Start(const SearchOptions& opts, VirtualTree* tree,
                           bool searchFiles, bool searchFolders, bool searchByPath) {
    CancelAndJoin();

    m_tree = tree;
    m_cancel.store(false);
    m_running.store(true);
    m_progress.store(0.0f);
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_result = SearchResult{};
    }

    try {
        m_worker = std::thread(&FileSearcher::WorkerThread, this, opts, searchFiles, searchFolders, searchByPath);
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_result.completed = true;
        m_result.error = "could not start the search";
        m_running.store(false);
    }
}

void FileSearcher::Cancel() {
    m_cancel.store(true);
}

void FileSearcher::CancelAndJoin() {
    Cancel();
    if (m_worker.joinable()) m_worker.join();
}

SearchResult FileSearcher::GetResult() {
    std::lock_guard<std::mutex> lock(m_resultMutex);
    return m_result;
}

void FileSearcher::WorkerThread(SearchOptions opts, bool searchFiles, bool searchFolders, bool searchByPath) {
    SearchResult result;
    try {
        Search(opts, searchFiles, searchFolders, searchByPath, result);
    } catch (const std::exception& ex) {
        result = SearchResult{};
        result.completed = true;
        result.error = ex.what();
    } catch (...) {
        result = SearchResult{};
        result.completed = true;
        result.error = "unknown error";
    }
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_result = std::move(result);
    }
    m_running.store(false);
}

void FileSearcher::Search(const SearchOptions& opts, bool searchFiles, bool searchFolders, bool searchByPath,
                          SearchResult& result) {
    if (!m_tree || opts.query.empty()) {
        result.completed = true;
        return;
    }

    // The query once per search (not once per name): the regex compiled, the needle lowered.
    const SearchQuery query(opts);

    // Collect all entries via internal tree walk (ignores expansion state)
    struct EntryInfo { std::string fullPath; std::string name; int64_t fileSize; bool isDirectory; };
    std::vector<EntryInfo> entries;

    if (opts.scopePaths.empty()) {
        // Search entire tree — walk from root (an empty scope is the root's children).
        entries.reserve(m_tree->GetFileCount() + m_tree->GetDirCount());
        m_tree->ForEachEntryUnder("", [&](const std::string& fullPath, const std::string& name,
                                           int64_t fileSize, bool isDir, void*) {
            entries.push_back({fullPath, name, fileSize, isDir});
        });
    } else {
        for (const auto& scope : opts.scopePaths) {
            m_tree->ForEachEntryUnder(scope, [&](const std::string& fullPath, const std::string& name,
                                                   int64_t fileSize, bool isDir, void*) {
                entries.push_back({fullPath, name, fileSize, isDir});
            });
        }
    }

    int total = static_cast<int>(entries.size());

    for (int i = 0; i < total; ++i) {
        if (m_cancel.load()) {
            result.cancelled = true;
            break;
        }

        const auto& entry = entries[i];

        // Skip based on type
        if (entry.isDirectory && !searchFolders) continue;
        if (!entry.isDirectory && !searchFiles) continue;

        // inSubFolders check: if disabled, only direct children of scope
        if (!opts.inSubFolders && !opts.scopePaths.empty()) {
            bool isDirect = false;
            for (const auto& scope : opts.scopePaths) {
                // Direct child: path after "scope/" has no more '/'
                if (entry.fullPath.size() > scope.size() + 1 &&
                    entry.fullPath.compare(0, scope.size(), scope) == 0 &&
                    entry.fullPath[scope.size()] == '/') {
                    if (entry.fullPath.find('/', scope.size() + 1) == std::string::npos)
                        isDirect = true;
                }
            }
            if (!isDirect) continue;
        }

        // Check filter (only for files)
        if (!entry.isDirectory && !MatchesFilter(entry.name, opts.filters)) continue;

        // Match query against name and/or path
        bool matched = false;
        if ((searchFiles && !entry.isDirectory) || (searchFolders && entry.isDirectory))
            matched = query.Matches(entry.name);
        if (!matched && searchByPath)
            matched = query.Matches(entry.fullPath);
        if (matched) {
            FileMatch fm;
            fm.fullPath = entry.fullPath;
            fm.name = entry.name;
            fm.isDirectory = entry.isDirectory;
            fm.fileSize = (entry.fileSize >= 0) ? static_cast<uint64_t>(entry.fileSize) : 0;
            result.fileMatches.push_back(std::move(fm));
            result.totalMatches++;
        }

        m_progress.store(static_cast<float>(i + 1) / static_cast<float>(total));
    }

    result.totalFiles = static_cast<int>(result.fileMatches.size());
    result.completed = !result.cancelled;
}

} // namespace GGPKExp
