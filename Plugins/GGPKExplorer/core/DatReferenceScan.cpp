// Plugins/GGPKExplorer/core/DatReferenceScan.cpp
// FindReferencesToRow (declared in DatSearcher.h): the DAT viewer's "Find references" scan
// over the archive. Kept out of DatSearcher.cpp, which stays free of archive access.
#include "DatSearcher.h"
#include "VirtualTree.h"
#include "GGPKManager.h"
#include "ExtractJob.h"
#include "Index.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <iterator>
#include <mutex>

namespace GGPKExp {

namespace {

bool IsDatc64Path(const std::string& path) {
    if (path.size() < 7) return false;
    std::string ext;
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    ext = path.substr(dot);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".datc64";
}

} // namespace

std::vector<RowReference> FindReferencesToRow(
    VirtualTree* tree,
    GGPKManager* mgr,
    DatSchema* schema,
    const std::string& targetTable,
    uint32_t targetRow,
    std::function<void(size_t, size_t)> progress,
    std::function<bool()> cancel)
{
    std::vector<RowReference> results;
    if (!tree || !mgr || !schema) return results;
    GGPK::Index* index = mgr->GetIndex();
    if (!index) return results;
    // Every .datc64 file of the tree (recursive, independent of expansion state) whose table
    // has a ForeignRow column to the target: the others cannot hold a reference, so they are
    // not read at all (FindReferencesInDat would return nothing for them).
    struct FileRef { const std::string* path; };
    std::vector<FileRef> files;
    std::vector<GGPK::FileRecord*> records;
    tree->ForEachFileUnder("", [&](const std::string& fullPath, const std::string& /*name*/,
                                    int64_t /*sz*/, void* nodePtr) {
        if (!nodePtr || !IsDatc64Path(fullPath)) return;
        if (!DatSearcher::ReferencingTable(fullPath, schema, targetTable)) return;
        files.push_back({ &fullPath });
        records.push_back(static_cast<GGPK::FileRecord*>(nodePtr));
    });

    const size_t total = files.size();
    if (total == 0) return results;   // also an empty target table: nothing can refer to it

    // Read in bundle order (each bundle opened once, each run of chunks decoded once); the
    // hits are put back in tree order at the end.
    DatSearcher searcher;
    std::atomic<bool> stop{false};
    std::mutex mutex;
    size_t done = 0;
    std::vector<std::pair<size_t, std::vector<RowReference>>> perFile;
    GGPK::ReadFilesOptions options;
    options.workers = ExtractJob::DefaultReadWorkers();
    try {
        index->ReadFiles(records,
            [&](size_t position, GGPK::FileRecord* /*record*/, std::vector<uint8_t>&& bytes, const std::string* error) {
                if (cancel && cancel()) {
                    stop.store(true);
                    return;
                }
                std::vector<RowReference> hits;
                if (!error && !bytes.empty())
                    hits = searcher.FindReferencesInDat(*files[position].path, bytes, schema, targetTable, targetRow);
                std::lock_guard<std::mutex> lock(mutex);
                if (!hits.empty())
                    perFile.emplace_back(position, std::move(hits));
                ++done;
                if (progress) progress(done, total);
            },
            &stop, options);
    } catch (...) {
        // A failed scan returns what it found so far.
    }
    mgr->TrimReadCache();

    std::sort(perFile.begin(), perFile.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [position, hits] : perFile) {
        (void)position;
        results.insert(results.end(),
                       std::make_move_iterator(hits.begin()),
                       std::make_move_iterator(hits.end()));
    }
    return results;
}

} // namespace GGPKExp
