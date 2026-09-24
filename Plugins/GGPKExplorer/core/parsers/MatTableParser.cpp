#include "MatTableParser.h"
#include "../BinaryReader.h"

namespace GGPKExp { namespace Parsers {

// File format (matches sub_7FF7AE5D6CC0 in PathOfExileSteam.exe v0.4.0j):
//
//   u32 version                                    (== 2)
//   u32 matCount
//   { u64 keyHash; u64 valueHash; }  × matCount     (16 bytes each — Section 1)
//   u32 comboCount
//   { u64 comboKey; u32 paramOffset;
//     u16 paramCount; }              × comboCount   (14 bytes each — Section 2)
//   u32 paramIdCount
//   u16 paramId                      × paramIdCount (2 bytes each  — Section 3)

// Conservative upper bounds. Actual shipped values are ~250k / ~85k / ~1.3M.
static constexpr uint32_t kMatCapCount    = 10'000'000u;
static constexpr uint32_t kComboCapCount  = 10'000'000u;
static constexpr uint32_t kParamIdCapCount = 100'000'000u;

Result<MatTableData> ParseMatTable(const uint8_t* data, size_t size) {
    if (!data || size < 4)
        return Result<MatTableData>::Fail("Data too small for mat_table header (need >= 4 bytes for version)");

    BinaryReader reader(data, size);
    MatTableData result;

    result.version = reader.Read<uint32_t>();
    if (!reader.IsValid())
        return Result<MatTableData>::Fail("Failed to read mat_table version");

    // Engine treats any version != 2 as invalid and ignores the file entirely.
    if (result.version != 2) {
        return Result<MatTableData>::Fail(
            "Unsupported mat_table version " + std::to_string(result.version) +
            " (engine expects 2)");
    }

    // ---------------------------------------------------------------------
    // Section 1: main mat entries, 16 bytes per record.
    // ---------------------------------------------------------------------
    uint32_t matCount = reader.Read<uint32_t>();
    if (!reader.IsValid())
        return Result<MatTableData>::Fail("Failed to read Section 1 count");
    if (matCount > kMatCapCount)
        return Result<MatTableData>::Fail(
            "Section 1 count exceeds safety cap: " + std::to_string(matCount));

    const size_t matBytes = static_cast<size_t>(matCount) * sizeof(uint64_t) * 2u;
    if (matBytes > reader.Remaining())
        return Result<MatTableData>::Fail(
            "Section 1 truncated: need " + std::to_string(matBytes) +
            " bytes, have " + std::to_string(reader.Remaining()));

    result.mats.reserve(matCount);
    for (uint32_t i = 0; i < matCount; ++i) {
        MatEntry e;
        e.keyHash   = reader.Read<uint64_t>();
        e.valueHash = reader.Read<uint64_t>();
        result.mats.push_back(e);
    }
    if (!reader.IsValid())
        return Result<MatTableData>::Fail(
            "Section 1 read failed at offset " + std::to_string(reader.Position()));

    // ---------------------------------------------------------------------
    // Section 2: graph combinations, 14 bytes per record (u64 + u32 + u16).
    // ---------------------------------------------------------------------
    uint32_t comboCount = reader.Read<uint32_t>();
    if (!reader.IsValid())
        return Result<MatTableData>::Fail("Failed to read Section 2 count");
    if (comboCount > kComboCapCount)
        return Result<MatTableData>::Fail(
            "Section 2 count exceeds safety cap: " + std::to_string(comboCount));

    const size_t comboBytes =
        static_cast<size_t>(comboCount) * (sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint16_t));
    if (comboBytes > reader.Remaining())
        return Result<MatTableData>::Fail(
            "Section 2 truncated: need " + std::to_string(comboBytes) +
            " bytes, have " + std::to_string(reader.Remaining()));

    result.combos.reserve(comboCount);
    for (uint32_t i = 0; i < comboCount; ++i) {
        GraphComboEntry c;
        c.comboKey    = reader.Read<uint64_t>();
        c.paramOffset = reader.Read<uint32_t>();
        c.paramCount  = reader.Read<uint16_t>();
        result.combos.push_back(c);
    }
    if (!reader.IsValid())
        return Result<MatTableData>::Fail(
            "Section 2 read failed at offset " + std::to_string(reader.Position()));

    // ---------------------------------------------------------------------
    // Section 3: flat u16 parameter IDs, 2 bytes per record.
    // ---------------------------------------------------------------------
    uint32_t paramIdCount = reader.Read<uint32_t>();
    if (!reader.IsValid())
        return Result<MatTableData>::Fail("Failed to read Section 3 count");
    if (paramIdCount > kParamIdCapCount)
        return Result<MatTableData>::Fail(
            "Section 3 count exceeds safety cap: " + std::to_string(paramIdCount));

    const size_t paramBytes = static_cast<size_t>(paramIdCount) * sizeof(uint16_t);
    if (paramBytes > reader.Remaining())
        return Result<MatTableData>::Fail(
            "Section 3 truncated: need " + std::to_string(paramBytes) +
            " bytes, have " + std::to_string(reader.Remaining()));

    result.paramIds.reserve(paramIdCount);
    for (uint32_t i = 0; i < paramIdCount; ++i)
        result.paramIds.push_back(reader.Read<uint16_t>());

    if (!reader.IsValid())
        return Result<MatTableData>::Fail(
            "Section 3 read failed at offset " + std::to_string(reader.Position()));

    return Result<MatTableData>::Ok(std::move(result));
}

}} // namespace GGPKExp::Parsers
