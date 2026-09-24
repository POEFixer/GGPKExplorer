// Plugins/GGPKExplorer/core/FileSearcher.cpp
#include "FileSearcher.h"
#include <regex>
#include <algorithm>
#include <cctype>

namespace GGPKExp {

static std::string ToLower(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (unsigned char c : s) r += static_cast<char>(std::tolower(c));
    return r;
}

FileSearcher::~FileSearcher() {
    Cancel();
    if (m_worker.joinable()) m_worker.join();
}

std::string FileSearcher::ExpandExtended(const std::string& pattern) const {
    std::string result;
    result.reserve(pattern.size());
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '\\' && i + 1 < pattern.size()) {
            char next = pattern[i + 1];
            switch (next) {
            case 'n': result += '\n'; i++; break;
            case 'r': result += '\r'; i++; break;
            case 't': result += '\t'; i++; break;
            case '0': result += '\0'; i++; break;
            case 'x':
                if (i + 3 < pattern.size()) {
                    result += static_cast<char>(std::stoi(pattern.substr(i + 2, 2), nullptr, 16));
                    i += 3;
                } else { result += pattern[i]; }
                break;
            case '\\': result += '\\'; i++; break;
            default: result += pattern[i]; break;
            }
        } else {
            result += pattern[i];
        }
    }
    return result;
}

bool FileSearcher::MatchesFilter(const std::string& name, const std::string& filters) const {
    if (filters.empty() || filters == "*.*" || filters == "*") return true;

    // Extract extension from name
    auto dot = name.rfind('.');
    std::string ext = (dot != std::string::npos) ? ToLower(name.substr(dot)) : "";

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
            std::string maskExt = ToLower(mask.substr(maskDot));
            if (ext == maskExt) return true;
        }
    }
    return false;
}

bool FileSearcher::MatchesQuery(const std::string& name, const SearchOptions& opts) const {
    std::string query = opts.query;
    if (opts.mode == SearchMode::Extended)
        query = ExpandExtended(query);

    if (opts.mode == SearchMode::Regex) {
        try {
            auto flags = std::regex::ECMAScript | std::regex::optimize;
            if (!opts.matchCase) flags |= std::regex::icase;
            std::regex re(query, flags);
            return std::regex_search(name, re);
        } catch (...) {
            return false;
        }
    }

    std::string haystack = opts.matchCase ? name : ToLower(name);
    std::string needle = opts.matchCase ? query : ToLower(query);

    if (opts.matchWholeWord) {
        size_t pos = 0;
        while ((pos = haystack.find(needle, pos)) != std::string::npos) {
            bool startOk = (pos == 0 || !std::isalnum(static_cast<unsigned char>(haystack[pos - 1])));
            bool endOk = (pos + needle.size() >= haystack.size() ||
                          !std::isalnum(static_cast<unsigned char>(haystack[pos + needle.size()])));
            if (startOk && endOk) return true;
            pos++;
        }
        return false;
    }

    return haystack.find(needle) != std::string::npos;
}

void FileSearcher::Start(const SearchOptions& opts, VirtualTree* tree,
                           bool searchFiles, bool searchFolders, bool searchByPath) {
    Cancel();
    if (m_worker.joinable()) m_worker.join();

    m_tree = tree;
    m_cancel.store(false);
    m_running.store(true);
    m_progress.store(0.0f);
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_result = SearchResult{};
    }

    m_worker = std::thread(&FileSearcher::WorkerThread, this, opts, searchFiles, searchFolders, searchByPath);
}

void FileSearcher::Cancel() {
    m_cancel.store(true);
}

SearchResult FileSearcher::GetResult() {
    std::lock_guard<std::mutex> lock(m_resultMutex);
    return m_result;
}

void FileSearcher::WorkerThread(SearchOptions opts, bool searchFiles, bool searchFolders, bool searchByPath) {
    SearchResult result;

    if (!m_tree || opts.query.empty()) {
        result.completed = true;
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_result = std::move(result);
        m_running.store(false);
        return;
    }

    // Collect all entries via internal tree walk (ignores expansion state)
    struct EntryInfo { std::string fullPath; std::string name; int64_t fileSize; bool isDirectory; };
    std::vector<EntryInfo> entries;

    if (opts.scopePaths.empty()) {
        // Search entire tree — walk from root
        const auto& flatList = m_tree->GetFlatList();
        entries.reserve(flatList.size());
        // For "entire tree" scope, we still need all entries.
        // Use ForEachEntryUnder on each top-level node, but we need
        // a way to walk everything. Walk root's children.
        // Actually for empty scope, just walk all flat entries expanded from root.
        // Better: walk the internal tree. We need root path.
        // Simplest: treat empty scope as "match everything" in the filter below.
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
                    std::string remainder = entry.fullPath.substr(scope.size() + 1);
                    if (remainder.find('/') == std::string::npos)
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
            matched = MatchesQuery(entry.name, opts);
        if (!matched && searchByPath)
            matched = MatchesQuery(entry.fullPath, opts);
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

    std::lock_guard<std::mutex> lock(m_resultMutex);
    m_result = std::move(result);
    m_running.store(false);
}

} // namespace GGPKExp
