#include "DatExport.h"
#include <fstream>
#include <functional>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace GGPKExp::DatExport {

// (moved from DatViewer.h IsHashColumn -- internal)
static bool IsHashColumn(const std::string& colName) {
    std::string lower;
    lower.reserve(colName.size());
    for (unsigned char c : colName)
        lower.push_back(static_cast<char>(std::tolower(c)));
    return lower.find("hash") != std::string::npos;
}

std::string CellToString(const CellValue& val, const ColumnDef* col) {
    bool isHash = col && IsHashColumn(col->name);
    if (std::holds_alternative<std::monostate>(val)) return "<null>";
    if (std::holds_alternative<bool>(val)) return std::get<bool>(val) ? "true" : "false";
    if (std::holds_alternative<int16_t>(val)) {
        if (isHash) { char b[16]; snprintf(b, sizeof(b), "0x%04X", static_cast<uint16_t>(std::get<int16_t>(val))); return b; }
        return std::to_string(std::get<int16_t>(val));
    }
    if (std::holds_alternative<uint16_t>(val)) {
        if (isHash) { char b[16]; snprintf(b, sizeof(b), "0x%04X", std::get<uint16_t>(val)); return b; }
        return std::to_string(std::get<uint16_t>(val));
    }
    if (std::holds_alternative<int32_t>(val)) {
        if (isHash) { char b[16]; snprintf(b, sizeof(b), "0x%08X", static_cast<uint32_t>(std::get<int32_t>(val))); return b; }
        return std::to_string(std::get<int32_t>(val));
    }
    if (std::holds_alternative<uint32_t>(val)) {
        if (isHash) { char b[16]; snprintf(b, sizeof(b), "0x%08X", std::get<uint32_t>(val)); return b; }
        return std::to_string(std::get<uint32_t>(val));
    }
    if (std::holds_alternative<float>(val)) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.4f", std::get<float>(val)); return buf;
    }
    if (std::holds_alternative<std::string>(val)) return std::get<std::string>(val);
    if (std::holds_alternative<int64_t>(val)) return std::to_string(std::get<int64_t>(val));
    if (std::holds_alternative<CellArray>(val)) {
        const auto& arr = std::get<CellArray>(val);
        if (arr.items.empty()) return "[]";
        std::string out = "[";
        for (size_t i = 0; i < arr.items.size(); ++i) {
            if (i > 0) out += ", ";
            out += CellToString(arr.items[i], col);
        }
        out += "]";
        return out;
    }
    return "";
}

nlohmann::json CellValueToJson(const CellValue& val, bool isHash) {
    if (std::holds_alternative<std::monostate>(val)) return nullptr;
    if (std::holds_alternative<bool>(val)) return std::get<bool>(val);
    if (std::holds_alternative<int16_t>(val)) {
        if (isHash) { char b[16]; snprintf(b, sizeof(b), "0x%04X", static_cast<uint16_t>(std::get<int16_t>(val))); return std::string(b); }
        return std::get<int16_t>(val);
    }
    if (std::holds_alternative<uint16_t>(val)) {
        if (isHash) { char b[16]; snprintf(b, sizeof(b), "0x%04X", std::get<uint16_t>(val)); return std::string(b); }
        return std::get<uint16_t>(val);
    }
    if (std::holds_alternative<int32_t>(val)) {
        if (isHash) { char b[16]; snprintf(b, sizeof(b), "0x%08X", static_cast<uint32_t>(std::get<int32_t>(val))); return std::string(b); }
        return std::get<int32_t>(val);
    }
    if (std::holds_alternative<uint32_t>(val)) {
        if (isHash) { char b[16]; snprintf(b, sizeof(b), "0x%08X", std::get<uint32_t>(val)); return std::string(b); }
        return std::get<uint32_t>(val);
    }
    if (std::holds_alternative<float>(val)) return std::get<float>(val);
    if (std::holds_alternative<std::string>(val)) {
        const auto& s = std::get<std::string>(val);
        return (s == "null") ? nlohmann::json(nullptr) : nlohmann::json(s);
    }
    if (std::holds_alternative<int64_t>(val)) return std::get<int64_t>(val);
    return nullptr;
}

// (moved from DatViewer.cpp -- internal CSV array formatter)
static std::string FormatArrayForCSV(const CellArray& arr, const ColumnDef* col,
                                     const std::function<std::string(const CellValue&, const ColumnDef*)>& cellToString) {
    std::string result = "[";
    for (size_t i = 0; i < arr.items.size(); ++i) {
        if (i > 0) result += ", ";
        const auto& item = arr.items[i];
        std::string itemStr = cellToString(item, col);
        const bool isStringItem = std::holds_alternative<std::string>(item);
        const bool needsInnerQuote = isStringItem &&
            (itemStr.find(',') != std::string::npos
             || itemStr.find('"') != std::string::npos
             || itemStr.find('[') != std::string::npos
             || itemStr.find(']') != std::string::npos);
        if (needsInnerQuote) {
            result += '"';
            for (char ch : itemStr) { if (ch == '"') result += '"'; result += ch; }
            result += '"';
        } else {
            result += itemStr;
        }
    }
    result += "]";
    return result;
}

// (moved from DatViewer.cpp -- internal interval second-half raw reader)
static CellValue ReadIntervalSecondFromRaw(const DatFile& dat, const std::vector<ColumnDef>& cols,
                                           size_t rowIdx, size_t colIdx) {
    const auto& col = cols[colIdx];
    if (!col.isInterval || col.isArray) return std::monostate{};
    uint32_t colOff = 0;
    for (size_t k = 0; k < colIdx; ++k)
        colOff += static_cast<uint32_t>(GetColumnByteSize(cols[k]));
    const int baseSize = GetTypeSize(col.type);
    const size_t rowStart = dat.fixedOffset + rowIdx * dat.rowLength;
    const size_t secondAbsOff = rowStart + colOff + static_cast<size_t>(baseSize);
    if (secondAbsOff + static_cast<size_t>(baseSize) > dat.rawData.size())
        return std::monostate{};
    const uint8_t* p = dat.rawData.data() + secondAbsOff;
    switch (col.type) {
    case ColumnType::U8: return static_cast<uint32_t>(*p);
    case ColumnType::Bool: return static_cast<bool>(*p != 0);
    case ColumnType::I16:  { int16_t v; std::memcpy(&v, p, 2); return v; }
    case ColumnType::U16:  { uint16_t v; std::memcpy(&v, p, 2); return v; }
    case ColumnType::I32:
    case ColumnType::EnumRow: { int32_t v; std::memcpy(&v, p, 4); return v; }
    case ColumnType::U32:  { uint32_t v; std::memcpy(&v, p, 4); return v; }
    case ColumnType::F32:  { float v; std::memcpy(&v, p, 4); return v; }
    default: return std::monostate{};
    }
}

std::vector<std::vector<CellValue>> DecodeRows(const DatFile& dat, const SchemaTable& table) {
    std::vector<std::vector<CellValue>> rows;
    rows.reserve(dat.rowCount);
    for (uint32_t r = 0; r < dat.rowCount; ++r) {
        std::vector<CellValue> row;
        row.reserve(table.columns.size());
        uint32_t columnOffset = 0;
        for (const auto& col : table.columns) {
            row.push_back(DecodeCell(dat, r, columnOffset, col));
            columnOffset += static_cast<uint32_t>(GetColumnByteSize(col));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

bool WriteCsv(const std::filesystem::path& out, const DatFile& dat,
              const SchemaTable& table, const std::vector<std::vector<CellValue>>& rows) {
    std::ofstream f{out};
    if (!f.is_open()) return false;

    auto cellToStringLambda = [](const CellValue& v, const ColumnDef* c) -> std::string {
        return CellToString(v, c);
    };
    auto writeField = [&f](const std::string& s) {
        if (s.find(',') != std::string::npos || s.find('"') != std::string::npos
            || s.find('\n') != std::string::npos || s.find('\r') != std::string::npos) {
            f << '"';
            for (char ch : s) { if (ch == '"') f << '"'; f << ch; }
            f << '"';
        } else {
            f << s;
        }
    };

    f << "Row";
    for (const auto& col : table.columns) {
        const std::string n = col.name.empty() ? ("Col" + std::to_string(col.index)) : col.name;
        f << ","; writeField(n);
        if (col.isInterval && !col.isArray) { f << ","; writeField(n + "_range"); }
    }
    f << '\n';

    for (size_t r = 0; r < rows.size(); ++r) {
        f << r;
        const auto& row = rows[r];
        for (size_t c = 0; c < table.columns.size(); ++c) {
            const auto& col = table.columns[c];
            f << ",";
            std::string cell;
            if (c < row.size() && std::holds_alternative<CellArray>(row[c])) {
                cell = FormatArrayForCSV(std::get<CellArray>(row[c]), &col, cellToStringLambda);
            } else {
                cell = (c < row.size()) ? CellToString(row[c], &col) : "";
            }
            writeField(cell);
            if (col.isInterval && !col.isArray) {
                CellValue second = ReadIntervalSecondFromRaw(dat, table.columns, r, c);
                std::string secStr = CellToString(second, &col);
                f << ","; writeField(secStr);
            }
        }
        f << '\n';
    }
    return true;
}

bool WriteJson(const std::filesystem::path& out, const DatFile& dat,
               const SchemaTable& table, const std::vector<std::vector<CellValue>>& rows) {
    nlohmann::ordered_json arr = nlohmann::ordered_json::array();
    for (size_t r = 0; r < rows.size(); ++r) {
        nlohmann::ordered_json obj;
        obj["_rid"] = r;
        const auto& row = rows[r];
        for (size_t c = 0; c < table.columns.size(); ++c) {
            const auto& colDef = table.columns[c];
            std::string colName = colDef.name.empty() ? ("Col" + std::to_string(c)) : colDef.name;
            bool isHash = IsHashColumn(colName);
            if (c >= row.size()) { obj[colName] = nullptr; continue; }
            const auto& val = row[c];
            if (colDef.isInterval && !colDef.isArray) {
                obj[colName] = CellValueToJson(val, isHash);
                uint32_t colOff = 0;
                for (size_t k = 0; k < c; ++k)
                    colOff += static_cast<uint32_t>(GetColumnByteSize(table.columns[k]));
                int baseSize = GetTypeSize(colDef.type);
                size_t rowStart = dat.fixedOffset + r * dat.rowLength;
                size_t secondAbsOff = rowStart + colOff + static_cast<size_t>(baseSize);
                CellValue second = std::monostate{};
                if (secondAbsOff + static_cast<size_t>(baseSize) <= dat.rawData.size()) {
                    const uint8_t* p = dat.rawData.data() + secondAbsOff;
                    switch (colDef.type) {
                    case ColumnType::U8: { second = static_cast<uint32_t>(*p); break; }
                    case ColumnType::I16: { int16_t v; std::memcpy(&v, p, 2); second = v; break; }
                    case ColumnType::U16: { uint16_t v; std::memcpy(&v, p, 2); second = v; break; }
                    case ColumnType::I32: case ColumnType::EnumRow: { int32_t v; std::memcpy(&v, p, 4); second = v; break; }
                    case ColumnType::U32: { uint32_t v; std::memcpy(&v, p, 4); second = v; break; }
                    case ColumnType::F32: { float v; std::memcpy(&v, p, 4); second = v; break; }
                    default: break;
                    }
                }
                obj[colName + "_range"] = CellValueToJson(second, isHash);
                continue;
            }
            if (std::holds_alternative<CellArray>(val)) {
                nlohmann::ordered_json jArr = nlohmann::ordered_json::array();
                for (const auto& item : std::get<CellArray>(val).items)
                    jArr.push_back(CellValueToJson(item, isHash));
                obj[colName] = jArr;
            } else {
                obj[colName] = CellValueToJson(val, isHash);
            }
        }
        arr.push_back(std::move(obj));
    }
    std::ofstream f{out};
    if (!f.is_open()) return false;
    f << arr.dump(2);
    return true;
}

bool ConvertBytesToCsv(const std::vector<uint8_t>& bytes, const std::string& tableName,
                       const DatSchema& schema, const std::filesystem::path& out, std::string* err) {
    DatFile dat = Parse(bytes);
    if (!dat.valid) { if (err) *err = dat.error.empty() ? "parse failed" : dat.error; return false; }
    const SchemaTable* table = schema.FindTable(tableName);
    if (!table) { if (err) *err = "no schema for " + tableName; return false; }
    auto rows = DecodeRows(dat, *table);
    if (!WriteCsv(out, dat, *table, rows)) { if (err) *err = "write failed"; return false; }
    return true;
}

bool ConvertBytesToJson(const std::vector<uint8_t>& bytes, const std::string& tableName,
                        const DatSchema& schema, const std::filesystem::path& out, std::string* err) {
    DatFile dat = Parse(bytes);
    if (!dat.valid) { if (err) *err = dat.error.empty() ? "parse failed" : dat.error; return false; }
    const SchemaTable* table = schema.FindTable(tableName);
    if (!table) { if (err) *err = "no schema for " + tableName; return false; }
    auto rows = DecodeRows(dat, *table);
    if (!WriteJson(out, dat, *table, rows)) { if (err) *err = "write failed"; return false; }
    return true;
}

} // namespace GGPKExp::DatExport
