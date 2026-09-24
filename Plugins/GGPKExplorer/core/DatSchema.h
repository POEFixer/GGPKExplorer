#pragma once
#include "DatParser.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>

namespace GGPKExp {

struct SchemaTable {
    std::string name;
    std::vector<ColumnDef> columns;
    int totalRowSize = 0;
};

class DatSchema {
public:
    // fs::path overload — Unicode-safe on Windows (native wide internally).
    // Use this for paths that may contain non-ASCII (e.g. exe path with
    // Cyrillic parent directory). path::string() would throw under the
    // default C locale, so callers must avoid that conversion.
    bool Load(const std::filesystem::path& jsonPath);
    bool IsLoaded() const { return m_loaded; }
    const SchemaTable* FindTable(const std::string& datFileName) const;
    std::string GenerateSchemaText(const SchemaTable& table) const;
    const std::string& GetError() const { return m_error; }

private:
    std::unordered_map<std::string, SchemaTable> m_tables;
    bool m_loaded = false;
    std::string m_error;
    ColumnType ParseColumnType(const std::string& typeStr) const;
    std::string TypeToString(const ColumnDef& col) const;
};

std::string ExtractTableName(const std::string& filePath);

} // namespace GGPKExp
