#pragma once
#include "DatParser.h"   // CellValue, CellArray, DatFile, ColumnDef, ColumnType, helpers
#include "DatSchema.h"   // SchemaTable, DatSchema
#include "../lib/json.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace GGPKExp::DatExport {

// Stringify one decoded cell as plain text (hash columns -> hex). Recurses for arrays.
std::string CellToString(const CellValue& v, const ColumnDef* col);

// Convert one decoded cell to JSON (hash columns -> hex string when isHash true).
nlohmann::json CellValueToJson(const CellValue& v, bool isHash);

// Decode every row of a parsed DAT against a schema table.
std::vector<std::vector<CellValue>> DecodeRows(const DatFile& dat, const SchemaTable& table);

// Write decoded rows as CSV / JSON. Returns false on file-open / I/O failure.
bool WriteCsv(const std::filesystem::path& out, const DatFile& dat,
              const SchemaTable& table, const std::vector<std::vector<CellValue>>& rows);
bool WriteJson(const std::filesystem::path& out, const DatFile& dat,
               const SchemaTable& table, const std::vector<std::vector<CellValue>>& rows);

// One-shot: parse bytes -> resolve schema table by name -> decode -> write.
// On failure sets *err (when non-null). A "no schema for <table>" message is the
// signal the batch exporter treats as a skip rather than a hard failure.
bool ConvertBytesToCsv(const std::vector<uint8_t>& bytes, const std::string& tableName,
                       const DatSchema& schema, const std::filesystem::path& out, std::string* err);
bool ConvertBytesToJson(const std::vector<uint8_t>& bytes, const std::string& tableName,
                        const DatSchema& schema, const std::filesystem::path& out, std::string* err);

} // namespace GGPKExp::DatExport
