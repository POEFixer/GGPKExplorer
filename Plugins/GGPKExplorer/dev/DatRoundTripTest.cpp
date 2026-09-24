#include "DatRoundTripTest.h"
#include "../core/DatParser.h"
#include "../core/DatSchema.h"
#include <fstream>
#include <filesystem>
#include <cstring>
#include <functional>

namespace GGPKExp::Dev {

namespace fs = std::filesystem;

// Read a file into a byte buffer. Uses std::filesystem::path overload for
// Unicode-safe path handling (user directories may contain non-ASCII characters).
static std::vector<uint8_t> ReadFile(const std::string& path) {
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f.is_open()) return {};
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    if (size <= 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(buf.data()), size);
    if (!f) {
        // Truncate to actual read count on short-read
        buf.resize(static_cast<size_t>(f.gcount()));
    }
    return buf;
}

// Deep structural equality for CellValue, including recursive CellArray compare.
static bool CellValueEquals(const CellValue& a, const CellValue& b) {
    if (a.index() != b.index()) return false;
    if (std::holds_alternative<std::monostate>(a)) return true;
    if (std::holds_alternative<bool>(a))     return std::get<bool>(a) == std::get<bool>(b);
    if (std::holds_alternative<int16_t>(a))  return std::get<int16_t>(a) == std::get<int16_t>(b);
    if (std::holds_alternative<uint16_t>(a)) return std::get<uint16_t>(a) == std::get<uint16_t>(b);
    if (std::holds_alternative<int32_t>(a))  return std::get<int32_t>(a) == std::get<int32_t>(b);
    if (std::holds_alternative<uint32_t>(a)) return std::get<uint32_t>(a) == std::get<uint32_t>(b);
    if (std::holds_alternative<float>(a))    return std::get<float>(a) == std::get<float>(b);
    if (std::holds_alternative<std::string>(a)) return std::get<std::string>(a) == std::get<std::string>(b);
    if (std::holds_alternative<int64_t>(a))  return std::get<int64_t>(a) == std::get<int64_t>(b);
    if (std::holds_alternative<CellArray>(a)) {
        const auto& A = std::get<CellArray>(a);
        const auto& B = std::get<CellArray>(b);
        if (A.items.size() != B.items.size()) return false;
        for (size_t i = 0; i < A.items.size(); ++i)
            if (!CellValueEquals(A.items[i], B.items[i])) return false;
        return true;
    }
    return false;
}

// Decodes all rows using the schema, walking the fixed row with per-column offsets
// (same pattern as DatViewer::DecodeAllRows).
static std::vector<std::vector<CellValue>> DecodeRows(const DatFile& dat, const SchemaTable& table) {
    std::vector<std::vector<CellValue>> rows;
    rows.reserve(dat.rowCount);
    for (uint32_t r = 0; r < dat.rowCount; ++r) {
        std::vector<CellValue> row;
        row.reserve(table.columns.size());
        uint32_t colOff = 0;
        for (const auto& col : table.columns) {
            row.push_back(DecodeCell(dat, r, colOff, col));
            colOff += static_cast<uint32_t>(GetColumnByteSize(col));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

RoundTripResult RunRoundTrip(const std::string& datPath, const std::string& schemaJsonPath) {
    RoundTripResult res;
    res.fileName = fs::path(datPath).filename().string();

    // 1. Load schema
    DatSchema schema;
    if (!schema.Load(schemaJsonPath)) {
        res.detail = "schema load failed: " + schema.GetError();
        return res;
    }
    // FindTable is keyed by table name (e.g. "stats"), not raw filename ("stats.datc64").
    const std::string tableKey = ExtractTableName(datPath);
    const SchemaTable* table = schema.FindTable(tableKey);
    if (!table) {
        res.detail = "no schema for " + res.fileName + " (key='" + tableKey + "')";
        return res;
    }

    // 2. Read + parse original
    auto bytes = ReadFile(datPath);
    if (bytes.empty()) { res.detail = "read failed: " + datPath; return res; }
    DatFile orig = Parse(bytes);
    if (!orig.valid) { res.detail = "orig parse failed: " + orig.error; return res; }
    res.parsed = true;

    auto origRows = DecodeRows(orig, *table);

    // Capture interval second halves from the source (DecodeCell drops them).
    auto intervalSeconds = ExtractIntervalSeconds(orig, table->columns);

    // 3. Serialize -> bytes
    auto serialized = Serialize(origRows, table->columns);
    if (serialized.empty()) { res.detail = "Serialize returned empty"; return res; }
    // Patch interval secondaries back in; must happen before reparse.
    PatchIntervalSecondHalves(serialized, orig.rowCount, table->columns, intervalSeconds);
    res.serialized = true;

    // 4. Parse serialized
    DatFile re = Parse(serialized);
    if (!re.valid) { res.detail = "reparse failed: " + re.error; return res; }
    res.reparsed = true;

    res.rowCountMatches = (orig.rowCount == re.rowCount);
    if (!res.rowCountMatches) {
        res.detail = "rowCount mismatch: orig=" + std::to_string(orig.rowCount) +
                     " re=" + std::to_string(re.rowCount);
        return res;
    }

    auto reRows = DecodeRows(re, *table);

    // 5. Compare cell-by-cell
    for (size_t r = 0; r < origRows.size(); ++r) {
        if (r >= reRows.size()) {
            res.firstDiffRow = r;
            res.detail = "reparsed row count short at r=" + std::to_string(r);
            return res;
        }
        const auto& origRow = origRows[r];
        const auto& reRow = reRows[r];
        for (size_t c = 0; c < origRow.size(); ++c) {
            if (c >= reRow.size()) {
                res.firstDiffRow = r;
                res.firstDiffCol = c;
                res.detail = "reparsed row width short at r=" + std::to_string(r)
                           + " c=" + std::to_string(c);
                return res;
            }
            if (!CellValueEquals(origRow[c], reRow[c])) {
                res.firstDiffRow = r;
                res.firstDiffCol = c;
                std::function<std::string(const CellValue&)> describe;
                describe = [&](const CellValue& v) -> std::string {
                    if (std::holds_alternative<std::monostate>(v)) return "null";
                    if (std::holds_alternative<bool>(v))     return std::string(std::get<bool>(v) ? "T" : "F");
                    if (std::holds_alternative<int16_t>(v))  return std::to_string(std::get<int16_t>(v));
                    if (std::holds_alternative<uint16_t>(v)) return std::to_string(std::get<uint16_t>(v));
                    if (std::holds_alternative<int32_t>(v))  return std::to_string(std::get<int32_t>(v));
                    if (std::holds_alternative<uint32_t>(v)) return std::to_string(std::get<uint32_t>(v));
                    if (std::holds_alternative<float>(v))    return std::to_string(std::get<float>(v));
                    if (std::holds_alternative<int64_t>(v))  return std::to_string(std::get<int64_t>(v));
                    if (std::holds_alternative<std::string>(v)) return "'" + std::get<std::string>(v) + "'";
                    if (std::holds_alternative<CellArray>(v)) {
                        std::string s = "[";
                        const auto& a = std::get<CellArray>(v).items;
                        for (size_t i = 0; i < a.size(); ++i) {
                            if (i) s += ",";
                            s += describe(a[i]);
                        }
                        s += "]";
                        return s;
                    }
                    return "?";
                };
                // Determine typeTag for human readability
                std::string typeTag;
                switch (table->columns[c].type) {
                case ColumnType::Bool: typeTag = "bool"; break;
                case ColumnType::I16: typeTag = "i16"; break;
                case ColumnType::U16: typeTag = "u16"; break;
                case ColumnType::I32: typeTag = "i32"; break;
                case ColumnType::U8: typeTag = "u8"; break;
                case ColumnType::U32: typeTag = "u32"; break;
                case ColumnType::F32: typeTag = "f32"; break;
                case ColumnType::String: typeTag = "str"; break;
                case ColumnType::Row: typeTag = "row"; break;
                case ColumnType::ForeignRow: typeTag = "frow"; break;
                case ColumnType::EnumRow: typeTag = "enum"; break;
                case ColumnType::Array: typeTag = "array"; break;
                }
                if (table->columns[c].isArray) typeTag = "arr<" + typeTag + ">";
                res.detail = "diff r=" + std::to_string(r) + " c=" + std::to_string(c)
                           + " col='" + table->columns[c].name + "' type=" + typeTag
                           + " orig=" + describe(origRow[c])
                           + " re=" + describe(reRow[c]);
                return res;
            }
        }
    }
    // Second-pass: verify interval secondaries round-tripped. We compare the
    // side-maps extracted from orig vs. re; if they differ the patch path is
    // broken. DecodeCell does not return them, so this is the only way to
    // confirm Serialize+Patch preserves them.
    auto reIntervalSeconds = ExtractIntervalSeconds(re, table->columns);
    if (intervalSeconds.size() != reIntervalSeconds.size()) {
        res.detail = "interval second-half count mismatch: orig="
                   + std::to_string(intervalSeconds.size())
                   + " re=" + std::to_string(reIntervalSeconds.size());
        return res;
    }
    for (const auto& [k, v] : intervalSeconds) {
        auto it = reIntervalSeconds.find(k);
        if (it == reIntervalSeconds.end() || !CellValueEquals(v, it->second)) {
            uint32_t r = static_cast<uint32_t>(k >> 32);
            uint32_t c = static_cast<uint32_t>(k & 0xFFFFFFFFu);
            res.firstDiffRow = r;
            res.firstDiffCol = c;
            res.detail = "interval second-half diff at r=" + std::to_string(r)
                       + " c=" + std::to_string(c)
                       + " col='" + table->columns[c].name + "'";
            return res;
        }
    }

    res.allCellsMatch = true;
    res.detail = "OK (" + std::to_string(orig.rowCount) + " rows, "
               + std::to_string(table->columns.size()) + " cols, "
               + std::to_string(intervalSeconds.size()) + " interval-seconds)";
    return res;
}

std::vector<RoundTripResult> RunAllRoundTrips(const std::string& schemaJsonPath) {
    static const std::vector<std::string> kTestFiles = {
        "D:/POE2Files/data/balance/stats.datc64",
        "D:/POE2Files/data/balance/activeskills.datc64",
        "D:/POE2Files/data/balance/skillgems.datc64",
        "D:/POE2Files/data/balance/monstervarieties.datc64",
        "D:/POE2Files/data/balance/itemclasses.datc64",
        "D:/POE2Files/data/balance/acts.datc64",
        // Interval-bearing tables: exercise the second-half patch path.
        "D:/POE2Files/data/balance/alternatepassiveadditions.datc64",
        "D:/POE2Files/data/balance/mods.datc64",
    };
    std::vector<RoundTripResult> out;
    out.reserve(kTestFiles.size());
    for (const auto& p : kTestFiles) out.push_back(RunRoundTrip(p, schemaJsonPath));
    return out;
}

} // namespace GGPKExp::Dev
