#pragma once
#include "DatParser.h"
#include "DatSchema.h"
#include <string>
#include <vector>

namespace GGPKExp {

struct ImportResult {
    std::vector<std::vector<CellValue>> rows;
    // Second halves of interval columns keyed by MakeIntervalKey(row, col).
    // Empty if no interval columns appeared in the CSV/JSON (e.g. old exports
    // or schemas without intervals). Consumers must call
    // PatchIntervalSecondHalves() on the Serialize() output to avoid zero-
    // filling those bytes.
    IntervalSecondMap intervalSeconds;
    std::vector<std::string> warnings;  // non-fatal (value coercion, missing key)
    std::vector<std::string> errors;    // fatal (structural mismatch, parse fail)
    bool valid = false;                 // true = rows usable (no structural errors)

    size_t rowCount() const { return rows.size(); }
};

class DatImporter {
public:
    // Import full-table CSV. Structural failures (missing columns, wrong types) -> errors[].
    // Bad values -> warnings[] + default value, except invalid u8 values are
    // fatal so an out-of-range byte cannot silently replace game data with zero.
    static ImportResult ImportCSV(const std::string& path, const SchemaTable& table);

    // Import full-table JSON (array of objects per row).
    static ImportResult ImportJSON(const std::string& path, const SchemaTable& table);
};

} // namespace GGPKExp
