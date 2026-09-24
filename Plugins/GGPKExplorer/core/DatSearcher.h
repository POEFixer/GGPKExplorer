// Plugins/GGPKExplorer/core/DatSearcher.h
#pragma once
#include "SearchTypes.h"
#include "DatParser.h"
#include "DatSchema.h"
#include <vector>
#include <string>
#include <cstdint>
#include <functional>

namespace GGPKExp {

class VirtualTree;
class GGPKManager;

// One foreign-row reference to a target (table, row).
struct RowReference {
    std::string table;    // referencing table name (no .datc64 suffix), e.g. "GemEffects"
    std::string column;   // column name in the referencing table that holds the foreign row
    uint32_t row = 0;     // referencing row index in `table`
    std::string preview;  // first non-empty string-column value from that row, for display
};

class DatSearcher {
public:
    std::vector<DatMatch> SearchInDat(const std::string& path,
                                       const std::vector<uint8_t>& fileData,
                                       const SearchOptions& opts,
                                       DatSchema* schema);

    ReplacementPreview BuildPreview(const std::string& path,
                                     const std::vector<uint8_t>& fileData,
                                     const SearchOptions& opts,
                                     DatSchema* schema);

    std::vector<uint8_t> ApplyReplacements(const std::vector<uint8_t>& fileData,
                                            const ReplacementPreview& preview,
                                            DatSchema* schema,
                                            const std::string& tableName);

    // Per-file scan: find every ForeignRow column in this DAT pointing at
    // (targetTable, targetRow). `targetTable` is matched case-insensitively
    // against `col.refTable` (with any trailing ".datc64" stripped on either side).
    // Array-of-ForeignRow columns are scanned element-wise.
    // `path` is used purely to derive the schema table name for this file and
    // populate RowReference::table / preview.
    std::vector<RowReference> FindReferencesInDat(const std::string& path,
                                                   const std::vector<uint8_t>& fileData,
                                                   DatSchema* schema,
                                                   const std::string& targetTable,
                                                   uint32_t targetRow);

private:
    bool MatchesQuery(const std::string& value, const SearchOptions& opts) const;
    std::string ApplyReplace(const std::string& value, const SearchOptions& opts) const;
    std::string ExpandExtended(const std::string& pattern) const;
    static std::string ExtractTableName(const std::string& path);
};

// Top-level driver: walks every .datc64 in `tree`, calls FindReferencesInDat
// on each, aggregates results. `progress(currentFileIdx, totalFiles)` is
// invoked periodically (after each file). If `cancel()` returns true mid-scan,
// the function returns partial results.
//
// `tree`, `mgr`, `schema` must remain valid for the duration of the call.
std::vector<RowReference> FindReferencesToRow(
    VirtualTree* tree,
    GGPKManager* mgr,
    DatSchema* schema,
    const std::string& targetTable,
    uint32_t targetRow,
    std::function<void(size_t currentFileIdx, size_t totalFiles)> progress = {},
    std::function<bool()> cancel = {});

} // namespace GGPKExp
