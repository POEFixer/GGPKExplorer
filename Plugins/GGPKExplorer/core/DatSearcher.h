// Plugins/GGPKExplorer/core/DatSearcher.h
#pragma once
#include "SearchTypes.h"
#include "SearchQuery.h"
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

// Stateless: every method may run on several threads at once (the schema is only read).
class DatSearcher {
public:
    // The SearchOptions forms prepare the query themselves; a search over many files
    // prepares one SearchQuery and passes it (the regex compiled once, not per cell).
    std::vector<DatMatch> SearchInDat(const std::string& path,
                                       const std::vector<uint8_t>& fileData,
                                       const SearchOptions& opts,
                                       DatSchema* schema);
    std::vector<DatMatch> SearchInDat(const std::string& path,
                                       const std::vector<uint8_t>& fileData,
                                       const SearchQuery& query,
                                       DatSchema* schema);

    ReplacementPreview BuildPreview(const std::string& path,
                                     const std::vector<uint8_t>& fileData,
                                     const SearchOptions& opts,
                                     DatSchema* schema);
    ReplacementPreview BuildPreview(const std::string& path,
                                     const std::vector<uint8_t>& fileData,
                                     const SearchQuery& query,
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

    // The schema table of the DAT at `path` when it has a string column (the only ones a
    // search or replace reads), else nullptr: such a file never matches, so it need not be
    // read at all.
    static const SchemaTable* SearchableTable(const std::string& path, const DatSchema* schema);

    // The schema table of the DAT at `path` when one of its ForeignRow columns refers to
    // `targetTable`, else nullptr: only those files can hold a reference.
    static const SchemaTable* ReferencingTable(const std::string& path, const DatSchema* schema,
                                               const std::string& targetTable);

    // "Foo" for ".../Foo.datc64" (the schema's table name).
    static std::string ExtractTableName(const std::string& path);

private:
    std::vector<DatMatch> SearchParsed(const std::string& path, const DatFile& dat,
                                       const SchemaTable& table, const SearchQuery& query) const;
    std::string ApplyReplace(const std::string& value, const SearchQuery& query) const;
};

// Top-level driver: walks every .datc64 in `tree`, calls FindReferencesInDat
// on each, aggregates results. `progress(currentFileIdx, totalFiles)` is
// invoked periodically (after each file). If `cancel()` returns true mid-scan,
// the function returns partial results. Only the files whose table can refer to
// `targetTable` are read, through the index's batch reader; results come in tree order.
// Defined in DatReferenceScan.cpp (it reads the archive; DatSearcher.cpp does not).
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
