#include "DatImporter.h"
#include "../lib/json.hpp"
#include <fstream>
#include <filesystem>
#include <sstream>
#include <cstdint>
#include <cstdlib>

namespace GGPKExp {

// ---- RFC 4180 tokenizer ------------------------------------------------------

static std::vector<std::vector<std::string>> ParseCSVLines(std::istream& in) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> currentRow;
    std::string field;
    bool inQuotes = false;

    int ch;
    while ((ch = in.get()) != EOF) {
        if (inQuotes) {
            if (ch == '"') {
                int peek = in.peek();
                if (peek == '"') { field += '"'; (void)in.get(); }
                else { inQuotes = false; }
            } else {
                field += static_cast<char>(ch);
            }
        } else {
            if (ch == '"' && field.empty()) {
                inQuotes = true;
            } else if (ch == ',') {
                currentRow.push_back(std::move(field));
                field.clear();
            } else if (ch == '\r') {
                // skip; handled by \n
            } else if (ch == '\n') {
                currentRow.push_back(std::move(field));
                field.clear();
                rows.push_back(std::move(currentRow));
                currentRow.clear();
            } else {
                field += static_cast<char>(ch);
            }
        }
    }
    // Last field
    if (!field.empty() || !currentRow.empty()) {
        currentRow.push_back(std::move(field));
        rows.push_back(std::move(currentRow));
    }
    return rows;
}

// ---- Value coercion ----------------------------------------------------------

// Match the stable fallback keys emitted by DatExport for unnamed columns.
static std::string ExportColumnName(const ColumnDef& col) {
    return col.name.empty() ? "Col" + std::to_string(col.index) : col.name;
}

static CellValue DefaultValue(const ColumnDef& col) {
    if (col.isArray) return CellArray{};
    switch (col.type) {
        case ColumnType::String: return std::string{};
        case ColumnType::Bool:   return false;
        case ColumnType::Row:
        case ColumnType::ForeignRow: return std::monostate{};
        case ColumnType::F32:    return 0.0f;
        case ColumnType::I16:    return int16_t{0};
        case ColumnType::U16:    return uint16_t{0};
        case ColumnType::I32:
        case ColumnType::EnumRow: return int32_t{0};
        case ColumnType::U8:
        case ColumnType::U32:    return uint32_t{0};
        default: return std::monostate{};
    }
}

static CellValue CoerceScalarCSV(const std::string& s, const ColumnDef& col, bool& ok) {
    ok = true;
    if (s.empty() || s == "null") {
        ok = true;
        return DefaultValue(col);
    }
    try {
        switch (col.type) {
            case ColumnType::U8: return ParseU8Value(s);
            case ColumnType::Bool:
                return (s == "true" || s == "1" || s == "True" || s == "TRUE");
            case ColumnType::I16: return static_cast<int16_t>(std::stoi(s));
            case ColumnType::U16: return static_cast<uint16_t>(std::stoul(s));
            case ColumnType::I32: case ColumnType::EnumRow: {
                if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                    return static_cast<int32_t>(std::stoul(s.substr(2), nullptr, 16));
                return static_cast<int32_t>(std::stoi(s));
            }
            case ColumnType::U32: {
                if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                    return static_cast<uint32_t>(std::stoul(s.substr(2), nullptr, 16));
                return static_cast<uint32_t>(std::stoul(s));
            }
            case ColumnType::F32: return std::stof(s);
            case ColumnType::String: return s;
            case ColumnType::Row: case ColumnType::ForeignRow:
                return static_cast<int64_t>(std::stoll(s));
            default:
                ok = false;
                return DefaultValue(col);
        }
    } catch (...) {
        ok = false;
        return DefaultValue(col);
    }
}

// ---- Quote-aware CSV array tokenizer ----------------------------------------
//
// Splits the *body* of a CSV-exported array (without surrounding brackets) into
// tokens, honouring RFC-4180-style double-quoting of items. Supports:
//   - unquoted items:      `a, b, c`            -> {"a","b","c"}
//   - quoted items:        `"a, comma", b`      -> {"a, comma","b"}
//   - escaped quotes:      `"he said ""hi"""`   -> {"he said \"hi\""}
//   - leading spaces after a comma are absorbed (ExportCSV emits ", ").
// Strings with unbalanced quotes are treated tolerantly (consume to end).
// Backwards compatible with the old `[a, b, c]` format.
static std::vector<std::string> TokenizeCSVArray(const std::string& body) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inQuotes = false;
    const size_t n = body.size();
    size_t i = 0;

    auto skipSpacesForward = [&]() {
        while (i < n && body[i] == ' ') ++i;
    };

    while (i < n) {
        char c = body[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < n && body[i + 1] == '"') { cur += '"'; i += 2; continue; }
                inQuotes = false; ++i;
            } else {
                cur += c; ++i;
            }
        } else {
            if (c == '"' && cur.empty()) {
                inQuotes = true; ++i;
            } else if (c == ',') {
                tokens.push_back(std::move(cur)); cur.clear();
                ++i;
                skipSpacesForward();
            } else {
                cur += c; ++i;
            }
        }
    }
    // Trailing token: push unless we started with an empty cur and no tokens
    // (i.e. truly empty body — callers already short-circuit that case).
    if (!cur.empty() || !tokens.empty()) tokens.push_back(std::move(cur));
    return tokens;
}

static CellValue CoerceCellCSV(const std::string& s, const ColumnDef& col, bool& ok) {
    if (!col.isArray) return CoerceScalarCSV(s, col, ok);

    // Array: expect "[a, b, c]" OR "[]" for empty
    ok = true;
    CellArray arr;
    std::string body = s;
    if (body.size() >= 2 && body.front() == '[' && body.back() == ']')
        body = body.substr(1, body.size() - 2);

    if (body.empty()) return arr;

    ColumnDef inner = col;
    inner.isArray = false;

    auto tokens = TokenizeCSVArray(body);
    for (auto& tok : tokens) {
        if (tok.empty()) continue;  // blank tokens (e.g. trailing comma) ignored
        bool inner_ok;
        arr.items.push_back(CoerceScalarCSV(tok, inner, inner_ok));
        if (!inner_ok) ok = false;
    }
    return arr;
}

// ---- ImportCSV --------------------------------------------------------------

ImportResult DatImporter::ImportCSV(const std::string& path, const SchemaTable& table) {
    ImportResult res;
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f.is_open()) {
        res.errors.push_back("Cannot open file: " + path);
        return res;
    }

    auto raw = ParseCSVLines(f);
    if (raw.empty()) {
        res.errors.push_back("Empty file");
        return res;
    }

    // Header row - first column is "Row" index, skip it.
    const auto& header = raw[0];
    if (header.size() < 2) {
        res.errors.push_back("Header must have at least Row + 1 column");
        return res;
    }

    // Map CSV column index -> (schema column index, isRange secondary half).
    // `_range` columns map to the primary interval column index with isRange=true
    // so row parsing can route them into ImportResult::intervalSeconds.
    struct CsvColMap { int schemaCol = -1; bool isRange = false; };
    std::vector<CsvColMap> csvToSchema(header.size());
    for (size_t i = 1; i < header.size(); ++i) {
        const std::string& name = header[i];

        if (name.size() >= 6 && name.substr(name.size() - 6) == "_range") {
            std::string base = name.substr(0, name.size() - 6);
            int primary = -1;
            for (size_t c = 0; c < table.columns.size(); ++c) {
                if (ExportColumnName(table.columns[c]) == base && table.columns[c].isInterval
                    && !table.columns[c].isArray) {
                    primary = static_cast<int>(c);
                    break;
                }
            }
            if (primary < 0) {
                res.warnings.push_back("Unknown _range column ignored: " + name);
            }
            csvToSchema[i] = { primary, true };
            continue;
        }

        int found = -1;
        for (size_t c = 0; c < table.columns.size(); ++c) {
            if (ExportColumnName(table.columns[c]) == name) { found = static_cast<int>(c); break; }
        }
        if (found == -1) {
            res.warnings.push_back("Unknown CSV column ignored: " + name);
        }
        csvToSchema[i] = { found, false };
    }

    // Check all required (primary, non-_range) schema columns exist in CSV.
    // A missing _range for an interval column is a warning, not an error: the
    // second-half falls back to the column-type default (zero).
    std::vector<bool> schemaPresent(table.columns.size(), false);
    for (const auto& cm : csvToSchema) if (cm.schemaCol >= 0 && !cm.isRange) schemaPresent[cm.schemaCol] = true;
    for (size_t c = 0; c < table.columns.size(); ++c) {
        if (!schemaPresent[c]) {
            res.errors.push_back("Missing CSV column: '" + table.columns[c].name + "'");
        }
    }
    if (!res.errors.empty()) return res;

    // Data rows
    for (size_t r = 1; r < raw.size(); ++r) {
        const auto& row = raw[r];
        if (row.empty()) continue; // skip trailing blank lines
        std::vector<CellValue> out(table.columns.size(), std::monostate{});

        // Default everything (so columns missing from this row get defaults)
        for (size_t c = 0; c < table.columns.size(); ++c) {
            out[c] = DefaultValue(table.columns[c]);
        }

        const uint32_t rowIdx = static_cast<uint32_t>(res.rows.size());

        for (size_t i = 1; i < row.size() && i < csvToSchema.size(); ++i) {
            const auto& cm = csvToSchema[i];
            if (cm.schemaCol < 0) continue;
            const ColumnDef& primaryCol = table.columns[cm.schemaCol];
            bool ok = true;

            if (cm.isRange) {
                // Interval secondary: coerce as a scalar of the primary's type
                // and stash in the side-map. Arrays-as-intervals aren't a
                // real schema shape; reject defensively.
                if (primaryCol.isArray) {
                    res.warnings.push_back("Row " + std::to_string(r - 1) +
                                           " col '" + primaryCol.name +
                                           "_range': column is array, ignored");
                    continue;
                }
                ColumnDef scalarCol = primaryCol;
                scalarCol.isArray = false;
                scalarCol.isInterval = false;
                CellValue v = CoerceScalarCSV(row[i], scalarCol, ok);
                if (!ok && primaryCol.type == ColumnType::U8) {
                    res.errors.push_back("Row " + std::to_string(r - 1) + ": invalid u8 interval value");
                    return res;
                }
                if (!ok) {
                    res.warnings.push_back("Row " + std::to_string(r - 1) +
                                           " col '" + primaryCol.name +
                                           "_range': coercion failed, used default");
                }
                res.intervalSeconds.emplace(
                    MakeIntervalKey(rowIdx, static_cast<uint32_t>(cm.schemaCol)),
                    std::move(v));
            } else {
                out[cm.schemaCol] = CoerceCellCSV(row[i], primaryCol, ok);
                if (!ok && primaryCol.type == ColumnType::U8) {
                    res.errors.push_back("Row " + std::to_string(r - 1) + ": invalid u8 value");
                    return res;
                }
                if (!ok) {
                    res.warnings.push_back("Row " + std::to_string(r - 1) +
                                           " col '" + primaryCol.name +
                                           "': coercion failed, used default");
                }
            }
        }
        res.rows.push_back(std::move(out));
    }

    res.valid = true;
    return res;
}

// ---- JSON value coercion -----------------------------------------------------

static CellValue CoerceJsonValue(const nlohmann::json& j, const ColumnDef& col, bool& ok);

static CellValue CoerceJsonScalar(const nlohmann::json& j, const ColumnDef& col, bool& ok) {
    ok = true;
    if (j.is_null()) return DefaultValue(col);
    try {
        switch (col.type) {
            case ColumnType::U8:
                if (j.is_string()) return ParseU8Value(j.get<std::string>());
                if (j.is_number_integer() || j.is_number_unsigned()) return ParseU8Value(j.dump());
                ok = false; return DefaultValue(col);
            case ColumnType::Bool:
                if (j.is_boolean()) return j.get<bool>();
                if (j.is_number()) return j.get<int>() != 0;
                ok = false; return DefaultValue(col);
            case ColumnType::I16: return static_cast<int16_t>(j.get<int>());
            case ColumnType::U16: return static_cast<uint16_t>(j.get<int>());
            case ColumnType::I32: case ColumnType::EnumRow: {
                if (j.is_string()) {
                    std::string s = j.get<std::string>();
                    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                        return static_cast<int32_t>(std::stoul(s.substr(2), nullptr, 16));
                    return static_cast<int32_t>(std::stoi(s));
                }
                return static_cast<int32_t>(j.get<int>());
            }
            case ColumnType::U32: {
                if (j.is_string()) {
                    std::string s = j.get<std::string>();
                    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                        return static_cast<uint32_t>(std::stoul(s.substr(2), nullptr, 16));
                    return static_cast<uint32_t>(std::stoul(s));
                }
                return static_cast<uint32_t>(j.get<uint32_t>());
            }
            case ColumnType::F32: return j.get<float>();
            case ColumnType::String:
                if (j.is_string()) return j.get<std::string>();
                if (j.is_null()) return std::string{};
                ok = false; return DefaultValue(col);
            case ColumnType::Row: case ColumnType::ForeignRow:
                if (j.is_null()) return int64_t{-1};
                return static_cast<int64_t>(j.get<int64_t>());
            default:
                ok = false;
                return DefaultValue(col);
        }
    } catch (...) {
        ok = false;
        return DefaultValue(col);
    }
}

static CellValue CoerceJsonValue(const nlohmann::json& j, const ColumnDef& col, bool& ok) {
    if (!col.isArray) return CoerceJsonScalar(j, col, ok);
    ok = true;
    if (j.is_null()) return CellArray{};
    if (!j.is_array()) { ok = false; return CellArray{}; }

    CellArray arr;
    ColumnDef inner = col;
    inner.isArray = false;
    for (const auto& e : j) {
        bool innerOk;
        arr.items.push_back(CoerceJsonScalar(e, inner, innerOk));
        if (!innerOk) ok = false;
    }
    return arr;
}

// ---- ImportJSON --------------------------------------------------------------

ImportResult DatImporter::ImportJSON(const std::string& path, const SchemaTable& table) {
    ImportResult res;
    std::ifstream f{std::filesystem::path(path)};
    if (!f.is_open()) {
        res.errors.push_back("Cannot open file: " + path);
        return res;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(f);
    } catch (const nlohmann::json::exception& e) {
        res.errors.push_back(std::string("JSON parse error: ") + e.what());
        return res;
    }

    if (!j.is_array()) {
        res.errors.push_back("JSON root must be an array of row objects");
        return res;
    }

    for (size_t r = 0; r < j.size(); ++r) {
        const auto& obj = j[r];
        if (!obj.is_object()) {
            res.warnings.push_back("Row " + std::to_string(r) + ": not an object, skipped");
            continue;
        }

        std::vector<CellValue> row(table.columns.size(), std::monostate{});
        for (size_t c = 0; c < table.columns.size(); ++c) {
            row[c] = DefaultValue(table.columns[c]);
        }

        std::vector<bool> filled(table.columns.size(), false);
        const uint32_t rowIdx = static_cast<uint32_t>(res.rows.size());

        for (auto it = obj.begin(); it != obj.end(); ++it) {
            const std::string& key = it.key();
            if (key == "_rid") continue;

            // _range keys: route into the interval-secondary side-map.
            if (key.size() >= 6 && key.substr(key.size() - 6) == "_range") {
                std::string base = key.substr(0, key.size() - 6);
                int primary = -1;
                for (size_t c = 0; c < table.columns.size(); ++c) {
                    if (ExportColumnName(table.columns[c]) == base && table.columns[c].isInterval
                        && !table.columns[c].isArray) {
                        primary = static_cast<int>(c);
                        break;
                    }
                }
                if (primary < 0) {
                    res.warnings.push_back("Row " + std::to_string(r) +
                                           ": unknown _range key '" + key + "'");
                    continue;
                }
                ColumnDef scalarCol = table.columns[primary];
                scalarCol.isArray = false;
                scalarCol.isInterval = false;
                bool ok;
                CellValue v = CoerceJsonScalar(it.value(), scalarCol, ok);
                if (!ok && scalarCol.type == ColumnType::U8) {
                    res.errors.push_back("Row " + std::to_string(r) + " col '" + key + "': invalid u8 interval value");
                    return res;
                }
                if (!ok) {
                    res.warnings.push_back("Row " + std::to_string(r) +
                                           " col '" + key + "': coercion failed, used default");
                }
                res.intervalSeconds.emplace(
                    MakeIntervalKey(rowIdx, static_cast<uint32_t>(primary)),
                    std::move(v));
                continue;
            }

            int col = -1;
            for (size_t c = 0; c < table.columns.size(); ++c) {
                if (ExportColumnName(table.columns[c]) == key) { col = static_cast<int>(c); break; }
            }
            if (col < 0) {
                res.warnings.push_back("Row " + std::to_string(r) + ": unknown key '" + key + "'");
                continue;
            }
            bool ok;
            row[col] = CoerceJsonValue(it.value(), table.columns[col], ok);
            if (!ok && table.columns[col].type == ColumnType::U8) {
                res.errors.push_back("Row " + std::to_string(r) + " col '" + key + "': invalid u8 value");
                return res;
            }
            if (!ok) {
                res.warnings.push_back("Row " + std::to_string(r) +
                                       " col '" + key + "': coercion failed, used default");
            }
            filled[col] = true;
        }

        // Warn about missing keys (columns not seen in this row object).
        // Interval primaries are still required — only the `_range` half is
        // optional (falls back to zero).
        for (size_t c = 0; c < table.columns.size(); ++c) {
            if (!filled[c]) {
                res.warnings.push_back("Row " + std::to_string(r) +
                                       ": missing '" + table.columns[c].name + "', used default");
            }
        }

        res.rows.push_back(std::move(row));
    }

    res.valid = true;
    return res;
}

} // namespace GGPKExp
