#include "DatSchema.h"
#include <json.hpp>
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace GGPKExp {

std::string ExtractTableName(const std::string& filePath) {
    std::filesystem::path p(filePath);
    std::string stem = p.stem().string();
    // Handle .datc64 -> stem is "Name.datc64" minus last ext, but .datc64 has double ext pattern
    // Actually stem() already strips the last extension, so "Foo.datc64" -> stem = "Foo"
    // But "Foo.dat64" -> stem = "Foo", "Foo.dat" -> stem = "Foo"
    return stem;
}

ColumnType DatSchema::ParseColumnType(const std::string& typeStr) const {
    if (typeStr == "bool") return ColumnType::Bool;
    if (typeStr == "u8") return ColumnType::U8;
    if (typeStr == "i16") return ColumnType::I16;
    if (typeStr == "u16") return ColumnType::U16;
    if (typeStr == "i32") return ColumnType::I32;
    if (typeStr == "u32") return ColumnType::U32;
    if (typeStr == "f32") return ColumnType::F32;
    if (typeStr == "string") return ColumnType::String;
    if (typeStr == "row") return ColumnType::Row;
    if (typeStr == "foreignrow") return ColumnType::ForeignRow;
    if (typeStr == "enumrow") return ColumnType::EnumRow;
    if (typeStr == "array") return ColumnType::Array;
    return ColumnType::I32;
}

std::string DatSchema::TypeToString(const ColumnDef& col) const {
    if (!col.refTable.empty()) return col.refTable;
    switch (col.type) {
    case ColumnType::Bool:       return "bool";
    case ColumnType::U8:         return "u8";
    case ColumnType::I16:        return "i16";
    case ColumnType::U16:        return "u16";
    case ColumnType::I32:        return "i32";
    case ColumnType::U32:        return "u32";
    case ColumnType::F32:        return "f32";
    case ColumnType::String:     return "string";
    case ColumnType::Row:        return "row";
    case ColumnType::ForeignRow: return "rid";
    case ColumnType::EnumRow:    return "enumrow";
    case ColumnType::Array:      return "array";
    default:                     return "unknown";
    }
}

bool DatSchema::Load(const std::filesystem::path& jsonPath) {
    // fs::path has a native (wide on Windows) representation, so the
    // ifstream ctor opens the actual file even when the parent directory
    // contains non-ASCII chars.
    std::ifstream fileStream{jsonPath};
    if (!fileStream.is_open()) {
        try {
            // path::string() throws under the default C locale when the path
            // has non-ASCII chars. Best-effort error message — if it throws,
            // fall back to a generic message.
            m_error = "Cannot open schema file: " + jsonPath.string();
        } catch (...) {
            m_error = "Cannot open schema file (non-ASCII path)";
        }
        return false;
    }

    nlohmann::json root;
    try {
        fileStream >> root;
    } catch (const std::exception& e) {
        m_error = std::string("JSON parse error: ") + e.what();
        return false;
    }
    fileStream.close();

    if (!root.contains("tables") || !root["tables"].is_array()) {
        m_error = "Schema missing 'tables' array";
        return false;
    }

    m_tables.clear();

    for (const auto& tbl : root["tables"]) {
        if (!tbl.contains("name") || !tbl["name"].is_string()) continue;

        // Filter by validFor & 2 (PoE2)
        if (tbl.contains("validFor") && tbl["validFor"].is_number_integer()) {
            int validFor = tbl["validFor"].get<int>();
            if ((validFor & 2) == 0) continue;
        }

        SchemaTable st;
        st.name = tbl["name"].get<std::string>();

        if (tbl.contains("columns") && tbl["columns"].is_array()) {
            int idx = 0;
            for (const auto& colJson : tbl["columns"]) {
                ColumnDef cd;
                if (colJson.contains("name") && colJson["name"].is_string())
                    cd.name = colJson["name"].get<std::string>();

                if (colJson.contains("type") && colJson["type"].is_string())
                    cd.type = ParseColumnType(colJson["type"].get<std::string>());

                if (colJson.contains("array") && colJson["array"].is_boolean())
                    cd.isArray = colJson["array"].get<bool>();

                if (colJson.contains("interval") && colJson["interval"].is_boolean())
                    cd.isInterval = colJson["interval"].get<bool>();

                if (colJson.contains("references") && colJson["references"].is_object()) {
                    auto& ref = colJson["references"];
                    if (ref.contains("table") && ref["table"].is_string())
                        cd.refTable = ref["table"].get<std::string>();
                }

                if (colJson.contains("file") && colJson["file"].is_string())
                    cd.refFile = colJson["file"].get<std::string>();

                if (colJson.contains("files") && colJson["files"].is_array()) {
                    for (const auto& f : colJson["files"]) {
                        if (f.is_string())
                            cd.refFiles.push_back(f.get<std::string>());
                    }
                }

                cd.index = idx++;
                st.columns.push_back(std::move(cd));
            }
        }

        int totalSize = 0;
        for (const auto& c : st.columns)
            totalSize += GetColumnByteSize(c);
        st.totalRowSize = totalSize;

        std::string key = st.name;
        std::transform(key.begin(), key.end(), key.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        m_tables[key] = std::move(st);
    }

    m_loaded = true;
    return true;
}

const SchemaTable* DatSchema::FindTable(const std::string& datFileName) const {
    std::string key = datFileName;
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto it = m_tables.find(key);
    return (it != m_tables.end()) ? &it->second : nullptr;
}

std::string DatSchema::GenerateSchemaText(const SchemaTable& table) const {
    std::string text = "type " + table.name + " {\n";
    for (const auto& col : table.columns) {
        text += "  " + col.name + ": ";
        if (col.isArray) text += "array<";
        text += TypeToString(col);
        if (col.isArray) text += ">";
        if (col.isInterval) text += " (interval)";
        text += "\n";
    }
    text += "}\n";
    text += "\nRow size: " + std::to_string(table.totalRowSize) + " bytes\n";
    text += "Columns: " + std::to_string(table.columns.size()) + "\n";
    return text;
}

} // namespace GGPKExp
