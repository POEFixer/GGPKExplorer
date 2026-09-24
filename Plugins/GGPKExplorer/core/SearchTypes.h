#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace GGPKExp {

enum class SearchMode { Normal, Extended, Regex };

struct SearchOptions {
    std::string query;
    std::string replaceWith;
    SearchMode mode = SearchMode::Normal;
    bool matchCase = false;
    bool matchWholeWord = false;
    bool inSubFolders = true;
    bool dotMatchesNewline = false;
    std::string filters;
    std::vector<std::string> scopePaths;
};

struct FileMatch {
    std::string fullPath;
    std::string name;
    bool isDirectory = false;
    uint64_t fileSize = 0;
};

struct ContentMatch {
    std::string fullPath;
    int lineNumber = 0;
    std::string lineText;
    int matchStart = 0;
    int matchLength = 0;
};

struct DatMatch {
    std::string fullPath;
    int row = 0;
    std::string columnName;
    std::string cellValue;
    int matchStart = 0;
    int matchLength = 0;
};

struct FileSearchResult {
    std::string fullPath;
    bool isDatc64 = false;
    std::vector<ContentMatch> contentMatches;
    std::vector<DatMatch> datMatches;
};

struct SearchResult {
    std::vector<FileMatch> fileMatches;
    std::vector<FileSearchResult> fileResults;
    int totalMatches = 0;
    int totalFiles = 0;
    bool completed = false;
    bool cancelled = false;
    std::string error;
};

struct ReplacementPreview {
    std::string fullPath;
    bool isDatc64 = false;
    struct Entry {
        int line = 0;
        std::string column;
        std::string before;
        std::string after;
    };
    std::vector<Entry> entries;
};

struct ReplaceResult {
    std::string path;
    int count = 0;
};

} // namespace GGPKExp
