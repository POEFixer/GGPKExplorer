#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <unordered_map>

namespace GGPKExp {

enum class ColumnType {
    Bool, I16, U16, I32, U32, F32, String, Row, ForeignRow, EnumRow, Array,
    U8 // Appended to preserve type ordinals; stored as the existing uint32_t variant.
};

int GetTypeSize(ColumnType type);

struct ColumnDef {
    std::string name;
    ColumnType type = ColumnType::I32;
    bool isArray = false;
    bool isInterval = false;
    std::string refTable;
    std::string refFile;              // NEW: single file-ref hint from schema (e.g. ".csd")
    std::vector<std::string> refFiles; // NEW: multiple file-ref hints
    int index = 0;
};

constexpr uint32_t NULL_MARKER_32 = 0xFEFEFEFE;
constexpr uint64_t NULL_MARKER_64 = 0xFEFEFEFEFEFEFEFE;

struct CellArray;

using CellValue = std::variant<
    std::monostate, bool, int16_t, uint16_t, int32_t, uint32_t,
    float, std::string, int64_t, CellArray
>;

struct CellArray {
    std::vector<CellValue> items;
};

struct DatFile {
    uint32_t rowCount = 0;
    uint32_t rowLength = 0;
    std::vector<uint8_t> rawData;
    uint32_t fixedOffset = 4;
    uint32_t varOffset = 0;
    bool valid = false;
    std::string error;
};

DatFile Parse(const std::vector<uint8_t>& data);
CellValue DecodeCell(const DatFile& dat, uint32_t rowIndex, uint32_t columnOffset, const ColumnDef& col);
std::string DecodeString(const DatFile& dat, uint64_t varSectionOffset);
int GetColumnByteSize(const ColumnDef& col);

// Checked byte conversion shared by editing, importing, serialization and patches.
uint32_t ParseU8Value(const std::string& text);
uint8_t EncodeU8Value(const CellValue& value);

// Serialize decoded rows back into .datc64 binary
std::vector<uint8_t> Serialize(const std::vector<std::vector<CellValue>>& rows,
                               const std::vector<ColumnDef>& columns);

// Interval-second-half side map: key = (row<<32)|col, value = the "_range" half.
// CellValue is expected to match col.type (int32_t for I32/EnumRow, etc.).
using IntervalSecondMap = std::unordered_map<uint64_t, CellValue>;

inline uint64_t MakeIntervalKey(uint32_t row, uint32_t col) {
    return (static_cast<uint64_t>(row) << 32) | col;
}

// Read the second halves of every interval column out of an already-parsed
// DatFile. Returned map can be fed to PatchIntervalSecondHalves after a
// round-trip Serialize to preserve interval data that DecodeCell does not
// return. Columns whose layout is unexpected (array/string) are skipped.
IntervalSecondMap ExtractIntervalSeconds(const DatFile& dat,
                                         const std::vector<ColumnDef>& columns);

// Overwrite the interval secondary bytes in `bytes` (assumed to be the output
// of Serialize with the same `columns`) using values from `seconds`. Entries
// that do not reference an interval column or whose variant doesn't match the
// column type are silently ignored (with an assert in debug). Safe if
// `seconds` is empty — in that case this is a no-op.
void PatchIntervalSecondHalves(std::vector<uint8_t>& bytes,
                               uint32_t rowCount,
                               const std::vector<ColumnDef>& columns,
                               const IntervalSecondMap& seconds);

// Edit descriptor for patch-based save (see ApplyEdits below).
struct DatEdit {
    uint32_t rowIndex;
    uint32_t columnIndex;   // Index into the ColumnDef vector passed to ApplyEdits.
    CellValue newValue;
    bool editInterval2 = false; // true → edit the second half of an isInterval column.
};

// Patch-based save. Returns a new buffer where unedited bytes are byte-identical
// to dat.rawData; edited cells are rewritten in-place for fixed-width types, or
// written as new entries appended to the end of the var section for string/array
// types (with deduplication against existing strings).
// Returns empty vector if dat is invalid or schema row-length doesn't match dat.rowLength.
// If edits.empty(), returns a copy of dat.rawData with no modifications.
std::vector<uint8_t> ApplyEdits(const DatFile& dat,
                                 const std::vector<ColumnDef>& columns,
                                 const std::vector<DatEdit>& edits);

} // namespace GGPKExp
