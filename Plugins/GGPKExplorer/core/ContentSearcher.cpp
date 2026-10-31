// Plugins/GGPKExplorer/core/ContentSearcher.cpp
#include "ContentSearcher.h"
#include "SearchQuery.h"
#include "FileTypeDetector.h"
#include "TextEncoding.h"
#include "ExtractJob.h"
#include "Index.h"
#include <algorithm>
#include <cctype>
#include <iterator>
#include <sstream>
#include <unordered_set>

namespace GGPKExp {

// Convert raw file bytes to UTF-8 string using TextEncoding utility
static std::string ConvertToUtf8(const std::vector<uint8_t>& data) {
    if (data.empty()) return {};
    auto encoding = DetectEncoding(data.data(), data.size());
    return DecodeToUtf8(data.data(), data.size(), encoding);
}

ContentSearcher::~ContentSearcher() {
    CancelAndJoin();
}

bool ContentSearcher::IsDatc64(const std::string& name) const {
    std::string lower = SearchToLower(name);
    return lower.size() > 7 && lower.substr(lower.size() - 7) == ".datc64";
}

bool ContentSearcher::MatchesFilter(const std::string& name, const std::string& filters) const {
    if (filters.empty() || filters == "*.*" || filters == "*") return true;
    auto dot = name.rfind('.');
    std::string ext = (dot != std::string::npos) ? SearchToLower(name.substr(dot)) : "";
    std::string remaining = filters;
    size_t pos = 0;
    while (pos < remaining.size()) {
        size_t space = remaining.find(' ', pos);
        std::string mask = (space != std::string::npos) ? remaining.substr(pos, space - pos) : remaining.substr(pos);
        pos = (space != std::string::npos) ? space + 1 : remaining.size();
        if (mask.empty()) continue;
        if (mask == "*.*" || mask == "*") return true;
        auto maskDot = mask.rfind('.');
        if (maskDot != std::string::npos) {
            if (ext == SearchToLower(mask.substr(maskDot))) return true;
        }
    }
    return false;
}

std::vector<ContentMatch> ContentSearcher::SearchInText(const std::string& text,
                                                          const std::string& path,
                                                          const SearchQuery& query) const {
    std::vector<ContentMatch> matches;
    if (!query.Valid()) return matches;   // a regex that does not compile matches nothing

    if (query.IsRegex()) {
        try {
            // Note: MSVC std::regex does not support multiline flag.
            // dotMatchesNewline is handled by searching full text instead of line-by-line.
            const auto& re = query.Regex();

            // Search line by line
            std::istringstream stream(text);
            std::string line;
            int lineNum = 1;
            while (std::getline(stream, line)) {
                std::sregex_iterator it(line.begin(), line.end(), re);
                std::sregex_iterator end;
                for (; it != end; ++it) {
                    ContentMatch m;
                    m.fullPath = path;
                    m.lineNumber = lineNum;
                    m.lineText = line;
                    m.matchStart = static_cast<int>(it->position());
                    m.matchLength = static_cast<int>(it->length());
                    matches.push_back(std::move(m));
                }
                lineNum++;
            }
        } catch (...) { /* a regex the matcher gives up on: what was found so far */ }
        return matches;
    }

    // Normal / Extended mode
    const std::string& needle = query.Needle();

    std::istringstream stream(text);
    std::string line;
    int lineNum = 1;
    while (std::getline(stream, line)) {
        std::string lowered;
        if (!query.MatchCase()) lowered = SearchToLower(line);
        const std::string& haystack = query.MatchCase() ? line : lowered;
        size_t pos = 0;
        while ((pos = haystack.find(needle, pos)) != std::string::npos) {
            bool match = true;
            if (query.WholeWord())
                match = IsWholeWordAt(haystack, pos, needle.size());
            if (match) {
                ContentMatch m;
                m.fullPath = path;
                m.lineNumber = lineNum;
                m.lineText = line;
                m.matchStart = static_cast<int>(pos);
                m.matchLength = static_cast<int>(needle.size());
                matches.push_back(std::move(m));
            }
            pos += needle.empty() ? 1 : needle.size();
        }
        lineNum++;
    }
    return matches;
}

std::string ContentSearcher::ReplaceInText(const std::string& text, const SearchQuery& q,
                                             int& count) const {
    count = 0;
    if (!q.ReplacementValid()) return text;

    const std::string& query = q.Query();
    const std::string& replacement = q.Replacement();

    if (q.IsRegex()) {
        try {
            const auto& re = q.Regex();

            // Count matches first
            auto it = std::sregex_iterator(text.begin(), text.end(), re);
            auto end = std::sregex_iterator();
            count = static_cast<int>(std::distance(it, end));

            return std::regex_replace(text, re, replacement);
        } catch (...) {
            return text;
        }
    }

    // Normal/Extended
    const std::string& needle = q.Needle();
    std::string lowered;
    if (!q.MatchCase()) lowered = SearchToLower(text);
    const std::string& haystackLower = q.MatchCase() ? text : lowered;
    std::string output;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t found = haystackLower.find(needle, pos);
        if (found == std::string::npos) {
            output += text.substr(pos);
            break;
        }
        if (q.WholeWord() && !IsWholeWordAt(haystackLower, found, needle.size())) {
            output += text[pos];
            pos++;
            continue;
        }
        output += text.substr(pos, found - pos);
        output += replacement;
        pos = found + query.size();
        count++;
    }
    return output;
}

// ---------------------------------------------------------------------------
// Jobs: start, cancel, results
// ---------------------------------------------------------------------------

void ContentSearcher::StartFind(const SearchOptions& opts, VirtualTree* tree, GGPKManager* mgr,
                                 DatSearcher* datSearcher, DatSchema* schema) {
    Launch(&ContentSearcher::FindWorker, opts, tree, mgr, datSearcher, schema);
}

void ContentSearcher::StartReplace(const SearchOptions& opts, VirtualTree* tree, GGPKManager* mgr,
                                    DatSearcher* datSearcher, DatSchema* schema) {
    Launch(&ContentSearcher::ReplaceWorker, opts, tree, mgr, datSearcher, schema);
}

bool ContentSearcher::Launch(void (ContentSearcher::*body)(SearchOptions), const SearchOptions& opts,
                             VirtualTree* tree, GGPKManager* mgr, DatSearcher* datSearcher, DatSchema* schema) {
    CancelAndJoin();

    m_tree = tree;
    m_mgr = mgr;
    m_datSearcher = datSearcher;
    m_schema = schema;
    m_cancel.store(false);
    m_running.store(true);
    m_progress.store(0.0f);
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        if (body == &ContentSearcher::FindWorker) {
            m_result = SearchResult{};
        } else {
            m_replaceOutcome = ReplaceOutcome{};
            m_replaceOutcomeReady = false;
        }
    }

    try {
        m_worker = std::thread(body, this, opts);
        return true;
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        if (body == &ContentSearcher::FindWorker) {
            m_result.completed = true;
            m_result.error = "could not start the search";
        } else {
            m_replaceOutcome.error = "could not start the replace";
            m_replaceOutcomeReady = true;
        }
        m_running.store(false);
        return false;
    }
}

void ContentSearcher::Cancel() {
    m_cancel.store(true);
}

void ContentSearcher::CancelAndJoin() {
    Cancel();
    if (m_worker.joinable()) m_worker.join();
}

SearchResult ContentSearcher::GetResult() {
    std::lock_guard<std::mutex> lock(m_resultMutex);
    return m_result;
}

bool ContentSearcher::TakeReplaceOutcome(ReplaceOutcome& out) {
    std::lock_guard<std::mutex> lock(m_resultMutex);
    if (!m_replaceOutcomeReady) return false;
    out = std::move(m_replaceOutcome);
    m_replaceOutcome = ReplaceOutcome{};
    m_replaceOutcomeReady = false;
    return true;
}

void ContentSearcher::FindWorker(SearchOptions opts) {
    SearchResult result;
    try {
        Find(opts, result);
    } catch (const std::exception& ex) {
        result = SearchResult{};
        result.completed = true;
        result.error = ex.what();
    } catch (...) {
        result = SearchResult{};
        result.completed = true;
        result.error = "unknown error";
    }
    if (m_mgr) m_mgr->TrimReadCache();
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_result = std::move(result);
    }
    m_running.store(false);
}

void ContentSearcher::ReplaceWorker(SearchOptions opts) {
    ReplaceOutcome outcome;
    try {
        Replace(opts, outcome);
    } catch (const std::exception& ex) {
        outcome.error = ex.what();
    } catch (...) {
        outcome.error = "unknown error";
    }
    if (m_mgr) m_mgr->TrimReadCache();
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_replaceOutcome = std::move(outcome);
        m_replaceOutcomeReady = true;
    }
    m_running.store(false);
}

// ---------------------------------------------------------------------------
// The files of the scope
// ---------------------------------------------------------------------------

std::vector<ContentSearcher::ScopeFile> ContentSearcher::CollectFiles(const SearchOptions& opts,
                                                                      bool directChildrenRule) const {
    // Via internal tree walk (ignores expansion state). Each file once, even when the
    // selection names a folder and something inside it.
    std::vector<ScopeFile> files;
    std::unordered_set<const void*> seen;
    auto collect = [&](const std::string& scopePath) {
        m_tree->ForEachFileUnder(scopePath, [&](const std::string& fullPath, const std::string& name,
                                                  int64_t fileSize, void* nodePtr) {
            if (!nodePtr) return;
            if (!MatchesFilter(name, opts.filters)) return;
            if (fileSize > static_cast<int64_t>(MAX_FILE_SIZE)) return;
            if (directChildrenRule && !opts.inSubFolders) {
                // Only direct children: no '/' after scope+'/' (a file scope is itself)
                if (fullPath.size() > scopePath.size() &&
                    fullPath.find('/', scopePath.size() + 1) != std::string::npos)
                    return;
            }
            if (!seen.insert(nodePtr).second) return;
            files.push_back({ &fullPath, &name, static_cast<GGPK::FileRecord*>(nodePtr) });
        });
    };

    if (opts.scopePaths.empty()) {
        // Search entire tree
        collect("");
    } else {
        for (const auto& scope : opts.scopePaths)
            collect(scope);
    }
    return files;
}

// A .datc64 searched through the schema whose table has no string column (or is not in the
// schema) can never match: it is not read at all.
bool ContentSearcher::SkipUnreadDat(const ScopeFile& file) const {
    return IsDatc64(*file.name) && m_datSearcher && m_schema &&
           DatSearcher::SearchableTable(*file.path, m_schema) == nullptr;
}

// ---------------------------------------------------------------------------
// Find
// ---------------------------------------------------------------------------

void ContentSearcher::Find(const SearchOptions& opts, SearchResult& result) {
    GGPK::Index* index = m_mgr ? m_mgr->GetIndex() : nullptr;
    if (!m_tree || !index || opts.query.empty()) {
        result.completed = true;
        return;
    }

    const SearchQuery query(opts);   // once per search (compiled regex, lowered needle)
    std::vector<ScopeFile> files = CollectFiles(opts, true);
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const ScopeFile& f) { return SkipUnreadDat(f); }),
                files.end());
    std::vector<GGPK::FileRecord*> records;
    records.reserve(files.size());
    for (const auto& f : files) records.push_back(f.record);

    const size_t total = files.size();
    std::atomic<size_t> done{0};
    std::mutex foundMutex;
    std::vector<std::pair<size_t, FileSearchResult>> found;

    // Read grouped by bundle; each file is searched on the reader that decoded it.
    GGPK::ReadFilesOptions readOptions;
    readOptions.workers = ExtractJob::DefaultReadWorkers();
    const GGPK::ReadFilesResult read = index->ReadFiles(records,
        [&](size_t position, GGPK::FileRecord* /*record*/, std::vector<uint8_t>&& bytes, const std::string* error) {
            if (!error && !bytes.empty()) {
                try {
                    const ScopeFile& f = files[position];
                    FileSearchResult fsr;
                    fsr.fullPath = *f.path;
                    bool any = false;
                    if (IsDatc64(*f.name) && m_datSearcher && m_schema) {
                        fsr.isDatc64 = true;
                        fsr.datMatches = m_datSearcher->SearchInDat(*f.path, bytes, query, m_schema);
                        any = !fsr.datMatches.empty();
                    } else {
                        // Treat as text
                        std::string text = ConvertToUtf8(bytes);
                        fsr.contentMatches = SearchInText(text, *f.path, query);
                        any = !fsr.contentMatches.empty();
                    }
                    if (any) {
                        std::lock_guard<std::mutex> lock(foundMutex);
                        found.emplace_back(position, std::move(fsr));
                    }
                } catch (...) {
                    // One file the search could not handle (memory): the others go on.
                }
            }
            const size_t d = done.fetch_add(1) + 1;
            m_progress.store(static_cast<float>(d) / static_cast<float>(total));
        },
        &m_cancel, readOptions);

    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [position, fsr] : found) {
        (void)position;
        result.totalMatches += static_cast<int>(fsr.isDatc64 ? fsr.datMatches.size() : fsr.contentMatches.size());
        result.totalFiles++;
        result.fileResults.push_back(std::move(fsr));
    }
    result.cancelled = read.cancelled || m_cancel.load();
    result.completed = !result.cancelled;
}

// ---------------------------------------------------------------------------
// Replace — text files rewritten, datc64 matches collected as previews
// ---------------------------------------------------------------------------

void ContentSearcher::Replace(const SearchOptions& opts, ReplaceOutcome& outcome) {
    GGPK::Index* index = m_mgr ? m_mgr->GetIndex() : nullptr;
    if (!m_tree || !index || opts.query.empty()) return;

    const SearchQuery query(opts);
    std::vector<ScopeFile> files = CollectFiles(opts, false);
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const ScopeFile& f) { return SkipUnreadDat(f); }),
                files.end());
    std::vector<GGPK::FileRecord*> records;
    records.reserve(files.size());
    for (const auto& f : files) records.push_back(f.record);

    const size_t total = files.size();
    size_t done = 0;
    std::vector<std::pair<size_t, ReplaceResult>> replaced;
    std::vector<std::pair<size_t, ReplacementPreview>> previews;
    std::vector<std::pair<size_t, std::string>> writeErrors;

    // Sink calls one at a time (serializeSink): the writes go into the archive in bundle
    // order, and the lists need no lock. Decoding still runs on several readers.
    GGPK::ReadFilesOptions readOptions;
    readOptions.workers = ExtractJob::DefaultReadWorkers();
    readOptions.serializeSink = true;
    const GGPK::ReadFilesResult read = index->ReadFiles(records,
        [&](size_t position, GGPK::FileRecord* /*record*/, std::vector<uint8_t>&& data, const std::string* error) {
            const ScopeFile& f = files[position];
            if (!error && !data.empty()) {
                try {
                    if (IsDatc64(*f.name) && m_datSearcher && m_schema) {
                        auto preview = m_datSearcher->BuildPreview(*f.path, data, query, m_schema);
                        if (!preview.entries.empty())
                            previews.emplace_back(position, std::move(preview));
                    } else {
                        auto encoding = DetectEncoding(data.data(), data.size());
                        bool hasBom = false;
                        if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
                            hasBom = true;
                        else if (data.size() >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF)))
                            hasBom = true;

                        std::string text = DecodeToUtf8(data.data(), data.size(), encoding);
                        int count = 0;
                        std::string replacedText = ReplaceInText(text, query, count);
                        if (count > 0) {
                            auto newData = EncodeFromUtf8(replacedText, encoding, hasBom);
                            std::string why;
                            if (m_mgr->WriteFile(f.record, newData, &why))
                                replaced.emplace_back(position, ReplaceResult{ *f.path, count });
                            else
                                writeErrors.emplace_back(position, *f.path + ": " + why);
                        }
                    }
                } catch (const std::exception& ex) {
                    writeErrors.emplace_back(position, *f.path + ": " + ex.what());
                } catch (...) {
                    writeErrors.emplace_back(position, *f.path + ": unknown error");
                }
            }
            ++done;
            m_progress.store(static_cast<float>(done) / static_cast<float>(total));
        },
        &m_cancel, readOptions);

    auto byPosition = [](const auto& a, const auto& b) { return a.first < b.first; };
    std::sort(replaced.begin(), replaced.end(), byPosition);
    std::sort(previews.begin(), previews.end(), byPosition);
    std::sort(writeErrors.begin(), writeErrors.end(), byPosition);
    for (auto& [position, r] : replaced) { (void)position; outcome.replaced.push_back(std::move(r)); }
    for (auto& [position, p] : previews) { (void)position; outcome.datPreviews.push_back(std::move(p)); }
    for (auto& [position, e] : writeErrors) { (void)position; outcome.writeErrors.push_back(std::move(e)); }
    outcome.cancelled = read.cancelled || m_cancel.load();
}

} // namespace GGPKExp
