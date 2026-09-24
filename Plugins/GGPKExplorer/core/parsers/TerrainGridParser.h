#pragma once

#include "../Result.h"
#include <cstdint>
#include <string>
#include <vector>

namespace GGPKExp { namespace Parsers {

struct TdtData {
    uint32_t version = 0;            // always 14 (0x0E)

    // Referenced files (from UTF-16LE string section)
    std::string tgtPath;             // .tgt geometry file path
    std::string tileName;            // short tile name (optional)
    std::string groundTypePath;      // .gt ground type path (optional)

    // Grid/tile dimensions (from data section)
    uint32_t gridWidth = 0;          // grid dimension W
    uint32_t gridHeight = 0;         // grid dimension H
    uint8_t  tileWidth = 0;          // tile size in grid units (1, 3, etc.)
    uint8_t  tileHeight = 0;         // tile size in grid units

    // Edge data (14 bytes, mostly 0xFF for passable borders)
    uint8_t  edgeData[14]{};

    // Ground type references (4 uint32 values, 0xFFFFFFFF = none)
    uint32_t groundTypeRefs[4]{};

    // Raw data section for hex display
    size_t   dataSectionOffset = 0;  // offset in file where data section starts
    std::vector<uint8_t> rawData;    // full file data for hex display
};

Result<TdtData> ParseTdt(const uint8_t* data, size_t size);

}} // namespace GGPKExp::Parsers
