// Plugins/GGPKExplorer/core/DatSearcher.cpp
#include "DatSearcher.h"
#include "VirtualTree.h"
#include "GGPKManager.h"
#include <regex>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <sstream>
#include <variant>

namespace GGPKExp {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string ToLower(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (unsigned char c : s) r += static_cast<char>(std::tolower(c));
    return r;
}

std::string DatSearcher::ExpandExtended(const std::string& pattern) const {
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
                    std::string hex = pattern.substr(i + 2, 2);
                    result += static_cast<char>(std::stoi(hex, nullptr, 16));
                    i += 3;
                } else {
                    result += pattern[i];
                }
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

bool DatSearcher::MatchesQuery(const std::string& value, const SearchOptions& opts) const {
    std::string query = opts.query;
    if (opts.mode == SearchMode::Extended)
        query = ExpandExtended(query);

    if (opts.mode == SearchMode::Regex) {
        try {
            auto flags = std::regex::ECMAScript | std::regex::optimize;
            if (!opts.matchCase)
                flags |= std::regex::icase;
            std::regex re(query, flags);
            return std::regex_search(value, re);
        } catch (...) {
            return false;
        }
    }

    std::string haystack = value;
    std::string needle = query;
    if (!opts.matchCase) {
        haystack = ToLower(haystack);
        needle = ToLower(needle);
    }

    if (opts.matchWholeWord) {
        // Whole word: check boundaries around each occurrence
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

std::string DatSearcher::ApplyReplace(const std::string& value, const SearchOptions& opts) const {
    std::string query = opts.query;
    std::string replacement = opts.replaceWith;
    if (opts.mode == SearchMode::Extended) {
        query = ExpandExtended(query);
        replacement = ExpandExtended(replacement);
    }

    if (opts.mode == SearchMode::Regex) {
        try {
            auto flags = std::regex::ECMAScript | std::regex::optimize;
            if (!opts.matchCase)
                flags |= std::regex::icase;
            std::regex re(query, flags);
            return std::regex_replace(value, re, replacement);
        } catch (...) {
            return value;
        }
    }

    // Normal/Extended: case-sensitive or case-insensitive find & replace all
    std::string result = value;
    std::string haystackLower = opts.matchCase ? result : ToLower(result);
    std::string needleLower = opts.matchCase ? query : ToLower(query);

    std::string output;
    size_t pos = 0;
    while (pos < result.size()) {
        size_t found = haystackLower.find(needleLower, pos);
        if (found == std::string::npos) {
            output += result.substr(pos);
            break;
        }

        if (opts.matchWholeWord) {
            bool startOk = (found == 0 || !std::isalnum(static_cast<unsigned char>(haystackLower[found - 1])));
            bool endOk = (found + needleLower.size() >= haystackLower.size() ||
                          !std::isalnum(static_cast<unsigned char>(haystackLower[found + needleLower.size()])));
            if (!startOk || !endOk) {
                output += result[pos];
                pos++;
                continue;
            }
        }

        output += result.substr(pos, found - pos);
        output += replacement;
        pos = found + query.size();
    }
    return output;
}

std::string DatSearcher::ExtractTableName(const std::string& path) {
    auto slash = path.rfind('/');
    std::string filename = (slash != std::string::npos) ? path.substr(slash + 1) : path;
    auto dot = filename.rfind('.');
    if (dot != std::string::npos)
        filename = filename.substr(0, dot);
    return filename;
}

// ---------------------------------------------------------------------------
// SearchInDat
// ---------------------------------------------------------------------------

std::vector<DatMatch> DatSearcher::SearchInDat(const std::string& path,
                                                const std::vector<uint8_t>& fileData,
                                                const SearchOptions& opts,
                                                DatSchema* schema) {
    std::vector<DatMatch> matches;
    DatFile dat = Parse(fileData);
    if (!dat.valid) return matches;

    std::string tableName = ExtractTableName(path);
    const SchemaTable* table = schema ? schema->FindTable(tableName) : nullptr;
    if (!table) return matches;  // Need schema to know which columns are strings

    for (uint32_t rowIdx = 0; rowIdx < dat.rowCount; ++rowIdx) {
        uint32_t columnOffset = 0;
        for (const auto& col : table->columns) {
            if (col.type == ColumnType::String) {
                CellValue cell = DecodeCell(dat, rowIdx, columnOffset, col);

                if (!col.isArray) {
                    // Scalar string column — existing logic.
                    if (std::holds_alternative<std::string>(cell)) {
                        const std::string& val = std::get<std::string>(cell);
                        if (MatchesQuery(val, opts)) {
                            DatMatch m;
                            m.fullPath = path;
                            m.row = static_cast<int>(rowIdx);
                            m.columnName = col.name;
                            m.cellValue = val;
                            std::string query = opts.query;
                            if (opts.mode == SearchMode::Extended)
                                query = ExpandExtended(query);
                            std::string haystack = opts.matchCase ? val : ToLower(val);
                            std::string needle   = opts.matchCase ? query : ToLower(query);
                            size_t pos = haystack.find(needle);
                            m.matchStart = (pos != std::string::npos) ? static_cast<int>(pos) : 0;
                            m.matchLength = static_cast<int>(query.size());
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
                                if (!anyMatch && MatchesQuery(s, opts)) anyMatch = true;
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
    ReplacementPreview preview;
    preview.fullPath = path;
    preview.isDatc64 = true;

    DatFile dat = Parse(fileData);
    if (!dat.valid) return preview;
    std::string tableName = ExtractTableName(path);
    const SchemaTable* table = schema ? schema->FindTable(tableName) : nullptr;
    if (!table) return preview;

    auto matches = SearchInDat(path, fileData, opts, schema);
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
        e.after = ApplyReplace(m.cellValue, opts);
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
// Reference-finder helpers
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

    DatFile dat = Parse(fileData);
    if (!dat.valid) return hits;

    std::string tableName = ExtractTableName(path);
    const SchemaTable* table = schema->FindTable(tableName);
    if (!table) return hits;

    const std::string normTarget = NormalizeTableName(targetTable);
    if (normTarget.empty()) return hits;

    // Pre-flag columns whose refTable matches the target; cheap once per file.
    std::vector<bool> colMatches(table->columns.size(), false);
    bool anyMatch = false;
    for (size_t i = 0; i < table->columns.size(); ++i) {
        const auto& col = table->columns[i];
        if (col.type != ColumnType::ForeignRow) continue;
        if (col.refTable.empty()) continue;
        if (NormalizeTableName(col.refTable) == normTarget) {
            colMatches[i] = true;
            anyMatch = true;
        }
    }
    if (!anyMatch) return hits;

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

// ---------------------------------------------------------------------------
// FindReferencesToRow — top-level driver
// ---------------------------------------------------------------------------

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
    if (NormalizeTableName(targetTable).empty()) return results;

    // Collect all .datc64 file paths + nodePtrs from the tree (recursive,
    // independent of expansion state).
    struct FileRef { std::string path; void* nodePtr; };
    std::vector<FileRef> files;
    tree->ForEachFileUnder("", [&](const std::string& fullPath, const std::string& name,
                                    int64_t /*sz*/, void* nodePtr) {
        (void)name;
        if (IsDatc64Path(fullPath))
            files.push_back({fullPath, nodePtr});
    });

    DatSearcher searcher;
    const size_t total = files.size();

    for (size_t i = 0; i < total; ++i) {
        if (cancel && cancel()) break;

        const auto& f = files[i];
        auto readRes = mgr->ReadFile(f.nodePtr);
        if (readRes && !readRes.value.empty()) {
            auto perFile = searcher.FindReferencesInDat(
                f.path, readRes.value, schema, targetTable, targetRow);
            if (!perFile.empty()) {
                results.insert(results.end(),
                               std::make_move_iterator(perFile.begin()),
                               std::make_move_iterator(perFile.end()));
            }
        }

        if (progress) progress(i + 1, total);
    }

    return results;
}

} // namespace GGPKExp
