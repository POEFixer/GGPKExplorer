// Plugins/GGPKExplorer/core/DatSearcher.cpp
// Byte-level DAT search, replace and reference finding: no archive access (the scan over the
// archive, FindReferencesToRow, is DatReferenceScan.cpp), so tests/run_tests.bat builds this
// file with DatParser and DatSchema alone.
#include "DatSearcher.h"
#include <regex>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <variant>

namespace GGPKExp {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Lowercase a copy and strip a trailing ".datc64" / ".dat64" / ".dat" suffix.
// Used to compare schema refTable strings (e.g. "Stats") against file-derived
// table names (e.g. "stats" extracted from "stats.datc64").
std::string NormalizeTableName(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out += static_cast<char>(std::tolower(c));
    auto stripSuffix = [&](const char* suffix) {
        size_t n = std::strlen(suffix);
        if (out.size() > n &&
            out.compare(out.size() - n, n, suffix) == 0) {
            out.resize(out.size() - n);
        }
    };
    stripSuffix(".datc64");
    stripSuffix(".dat64");
    stripSuffix(".dat");
    return out;
}

// Pull the first non-empty string column from `row` for a friendly preview.
// Falls back to "" if no string column carries a non-empty value.
std::string BuildRowPreview(const DatFile& dat,
                            uint32_t rowIdx,
                            const std::vector<ColumnDef>& columns) {
    uint32_t colOff = 0;
    for (const auto& col : columns) {
        if (col.type == ColumnType::String && !col.isArray) {
            CellValue v = DecodeCell(dat, rowIdx, colOff, col);
            if (std::holds_alternative<std::string>(v)) {
                const std::string& s = std::get<std::string>(v);
                if (!s.empty() && s != "null")
                    return s;
            }
        }
        colOff += static_cast<uint32_t>(GetColumnByteSize(col));
    }
    return std::string{};
}

bool ForeignRowEquals(const CellValue& v, uint32_t targetRow) {
    if (std::holds_alternative<int64_t>(v)) {
        int64_t raw = std::get<int64_t>(v);
        // DecodeCell returns std::monostate for the NULL marker, so a real
        // value reaching here is the actual primary row index.
        if (raw < 0) return false;
        return static_cast<uint64_t>(raw) == static_cast<uint64_t>(targetRow);
    }
    return false;
}

// The columns of `table` that are ForeignRow references to the (normalized) target table.
bool FlagReferenceColumns(const SchemaTable& table, const std::string& normTarget,
                          std::vector<bool>* colMatches) {
    bool anyMatch = false;
    if (colMatches) colMatches->assign(table.columns.size(), false);
    for (size_t i = 0; i < table.columns.size(); ++i) {
        const auto& col = table.columns[i];
        if (col.type != ColumnType::ForeignRow) continue;
        if (col.refTable.empty()) continue;
        if (NormalizeTableName(col.refTable) == normTarget) {
            if (colMatches) (*colMatches)[i] = true;
            anyMatch = true;
        }
    }
    return anyMatch;
}

} // namespace

std::string DatSearcher::ExtractTableName(const std::string& path) {
    auto slash = path.rfind('/');
    std::string filename = (slash != std::string::npos) ? path.substr(slash + 1) : path;
    auto dot = filename.rfind('.');
    if (dot != std::string::npos)
        filename = filename.substr(0, dot);
    return filename;
}

const SchemaTable* DatSearcher::SearchableTable(const std::string& path, const DatSchema* schema) {
    const SchemaTable* table = schema ? schema->FindTable(ExtractTableName(path)) : nullptr;
    if (!table) return nullptr;
    for (const auto& col : table->columns)
        if (col.type == ColumnType::String) return table;
    return nullptr;
}

const SchemaTable* DatSearcher::ReferencingTable(const std::string& path, const DatSchema* schema,
                                                 const std::string& targetTable) {
    const SchemaTable* table = schema ? schema->FindTable(ExtractTableName(path)) : nullptr;
    if (!table) return nullptr;
    const std::string normTarget = NormalizeTableName(targetTable);
    if (normTarget.empty()) return nullptr;
    return FlagReferenceColumns(*table, normTarget, nullptr) ? table : nullptr;
}

std::string DatSearcher::ApplyReplace(const std::string& value, const SearchQuery& q) const {
    const std::string& query = q.Query();
    const std::string& replacement = q.Replacement();

    if (q.IsRegex()) {
        try {
            return std::regex_replace(value, q.Regex(), replacement);
        } catch (...) {
            return value;
        }
    }

    // Normal/Extended: case-sensitive or case-insensitive find & replace all
    const std::string& result = value;
    std::string lowered;
    if (!q.MatchCase()) lowered = SearchToLower(result);
    const std::string& haystackLower = q.MatchCase() ? result : lowered;
    const std::string& needleLower = q.Needle();

    std::string output;
    size_t pos = 0;
    while (pos < result.size()) {
        size_t found = haystackLower.find(needleLower, pos);
        if (found == std::string::npos) {
            output += result.substr(pos);
            break;
        }

        if (q.WholeWord() && !IsWholeWordAt(haystackLower, found, needleLower.size())) {
            output += result[pos];
            pos++;
            continue;
        }

        output += result.substr(pos, found - pos);
        output += replacement;
        pos = found + query.size();
    }
    return output;
}

// ---------------------------------------------------------------------------
// SearchInDat
// ---------------------------------------------------------------------------

std::vector<DatMatch> DatSearcher::SearchInDat(const std::string& path,
                                                const std::vector<uint8_t>& fileData,
                                                const SearchOptions& opts,
                                                DatSchema* schema) {
    return SearchInDat(path, fileData, SearchQuery(opts), schema);
}

std::vector<DatMatch> DatSearcher::SearchInDat(const std::string& path,
                                                const std::vector<uint8_t>& fileData,
                                                const SearchQuery& query,
                                                DatSchema* schema) {
    // The schema first: a table it does not know is never parsed.
    std::string tableName = ExtractTableName(path);
    const SchemaTable* table = schema ? schema->FindTable(tableName) : nullptr;
    if (!table) return {};  // Need schema to know which columns are strings
    if (!query.Valid()) return {};

    DatFile dat = Parse(fileData);
    if (!dat.valid) return {};
    return SearchParsed(path, dat, *table, query);
}

std::vector<DatMatch> DatSearcher::SearchParsed(const std::string& path, const DatFile& dat,
                                                const SchemaTable& table, const SearchQuery& query) const {
    std::vector<DatMatch> matches;
    for (uint32_t rowIdx = 0; rowIdx < dat.rowCount; ++rowIdx) {
        uint32_t columnOffset = 0;
        for (const auto& col : table.columns) {
            if (col.type == ColumnType::String) {
                CellValue cell = DecodeCell(dat, rowIdx, columnOffset, col);

                if (!col.isArray) {
                    // Scalar string column — existing logic.
                    if (std::holds_alternative<std::string>(cell)) {
                        const std::string& val = std::get<std::string>(cell);
                        if (query.Matches(val)) {
                            DatMatch m;
                            m.fullPath = path;
                            m.row = static_cast<int>(rowIdx);
                            m.columnName = col.name;
                            m.cellValue = val;
                            std::string haystack = query.MatchCase() ? val : SearchToLower(val);
                            size_t pos = haystack.find(query.Needle());
                            m.matchStart = (pos != std::string::npos) ? static_cast<int>(pos) : 0;
                            m.matchLength = static_cast<int>(query.Query().size());
                            matches.push_back(std::move(m));
                        }
                    }
                } else {
                    // array<string>: check each element; if any matches, emit one match
                    // keyed on the whole array (represented as joined string for display).
                    if (std::holds_alternative<CellArray>(cell)) {
                        const auto& arr = std::get<CellArray>(cell).items;
                        bool anyMatch = false;
                        std::string joined;
                        for (size_t i = 0; i < arr.size(); ++i) {
                            if (i) joined += ", ";
                            if (std::holds_alternative<std::string>(arr[i])) {
                                const auto& s = std::get<std::string>(arr[i]);
                                joined += s;
                                if (!anyMatch && query.Matches(s)) anyMatch = true;
                            }
                        }
                        if (anyMatch) {
                            DatMatch m;
                            m.fullPath = path;
                            m.row = static_cast<int>(rowIdx);
                            m.columnName = col.name;
                            m.cellValue = joined;
                            m.matchStart = 0;
                            m.matchLength = 0;
                            matches.push_back(std::move(m));
                        }
                    }
                }
            }
            columnOffset += static_cast<uint32_t>(GetColumnByteSize(col));
        }
    }
    return matches;
}

// ---------------------------------------------------------------------------
// BuildPreview
// ---------------------------------------------------------------------------

ReplacementPreview DatSearcher::BuildPreview(const std::string& path,
                                              const std::vector<uint8_t>& fileData,
                                              const SearchOptions& opts,
                                              DatSchema* schema) {
    return BuildPreview(path, fileData, SearchQuery(opts), schema);
}

ReplacementPreview DatSearcher::BuildPreview(const std::string& path,
                                              const std::vector<uint8_t>& fileData,
                                              const SearchQuery& query,
                                              DatSchema* schema) {
    ReplacementPreview preview;
    preview.fullPath = path;
    preview.isDatc64 = true;

    // The schema first; the file is parsed once (search and preview share it).
    std::string tableName = ExtractTableName(path);
    const SchemaTable* table = schema ? schema->FindTable(tableName) : nullptr;
    if (!table) return preview;
    if (!query.ReplacementValid()) return preview;
    DatFile dat = Parse(fileData);
    if (!dat.valid) return preview;

    auto matches = SearchParsed(path, dat, *table, query);
    for (const auto& m : matches) {
        bool isArrayCol = false;
        for (const auto& c : table->columns) {
            if (c.name == m.columnName && c.isArray && c.type == ColumnType::String) {
                isArrayCol = true; break;
            }
        }
        if (isArrayCol) continue;

        ReplacementPreview::Entry e;
        e.line = m.row;
        e.column = m.columnName;
        e.before = m.cellValue;
        e.after = ApplyReplace(m.cellValue, query);
        if (e.before != e.after)
            preview.entries.push_back(std::move(e));
    }
    return preview;
}

// ---------------------------------------------------------------------------
// ApplyReplacements
// ---------------------------------------------------------------------------

std::vector<uint8_t> DatSearcher::ApplyReplacements(const std::vector<uint8_t>& fileData,
                                                      const ReplacementPreview& preview,
                                                      DatSchema* schema,
                                                      const std::string& tableName) {
    DatFile dat = Parse(fileData);
    if (!dat.valid) return fileData;

    const SchemaTable* table = schema ? schema->FindTable(tableName) : nullptr;
    if (!table) return fileData;

    std::vector<DatEdit> edits;
    edits.reserve(preview.entries.size());
    for (const auto& entry : preview.entries) {
        if (entry.line < 0 || static_cast<uint32_t>(entry.line) >= dat.rowCount) continue;
        size_t colIdx = SIZE_MAX;
        for (size_t c = 0; c < table->columns.size(); ++c) {
            if (table->columns[c].name == entry.column) { colIdx = c; break; }
        }
        if (colIdx == SIZE_MAX) continue;

        DatEdit e;
        e.rowIndex = static_cast<uint32_t>(entry.line);
        e.columnIndex = static_cast<uint32_t>(colIdx);
        e.newValue = entry.after;
        edits.push_back(std::move(e));
    }

    auto result = ApplyEdits(dat, table->columns, edits);
    if (result.empty()) return fileData;  // Mismatch/error — keep original
    return result;
}

// ---------------------------------------------------------------------------
// FindReferencesInDat
// ---------------------------------------------------------------------------

std::vector<RowReference> DatSearcher::FindReferencesInDat(const std::string& path,
                                                            const std::vector<uint8_t>& fileData,
                                                            DatSchema* schema,
                                                            const std::string& targetTable,
                                                            uint32_t targetRow) {
    std::vector<RowReference> hits;
    if (!schema) return hits;

    // The schema first: a file whose table cannot refer to the target is never parsed.
    std::string tableName = ExtractTableName(path);
    const SchemaTable* table = schema->FindTable(tableName);
    if (!table) return hits;

    const std::string normTarget = NormalizeTableName(targetTable);
    if (normTarget.empty()) return hits;

    // Pre-flag columns whose refTable matches the target; cheap once per file.
    std::vector<bool> colMatches;
    if (!FlagReferenceColumns(*table, normTarget, &colMatches)) return hits;

    DatFile dat = Parse(fileData);
    if (!dat.valid) return hits;

    for (uint32_t rowIdx = 0; rowIdx < dat.rowCount; ++rowIdx) {
        uint32_t colOff = 0;
        bool rowHit = false;
        std::string firstHitColumn;
        for (size_t i = 0; i < table->columns.size(); ++i) {
            const auto& col = table->columns[i];
            if (colMatches[i]) {
                CellValue cell = DecodeCell(dat, rowIdx, colOff, col);
                if (!col.isArray) {
                    if (ForeignRowEquals(cell, targetRow)) {
                        rowHit = true;
                        firstHitColumn = col.name;
                    }
                } else if (std::holds_alternative<CellArray>(cell)) {
                    const auto& items = std::get<CellArray>(cell).items;
                    for (const auto& el : items) {
                        if (ForeignRowEquals(el, targetRow)) {
                            rowHit = true;
                            firstHitColumn = col.name;
                            break;
                        }
                    }
                }
            }
            colOff += static_cast<uint32_t>(GetColumnByteSize(col));
            if (rowHit) {
                // Emit one RowReference per (row, column) hit. A row that
                // references the target via two different columns is reported
                // twice — that mirrors how the user thinks about the data.
                RowReference ref;
                ref.table = tableName;
                ref.column = firstHitColumn;
                ref.row = rowIdx;
                ref.preview = BuildRowPreview(dat, rowIdx, table->columns);
                hits.push_back(std::move(ref));
                rowHit = false;
                firstHitColumn.clear();
            }
        }
    }

    return hits;
}

} // namespace GGPKExp
