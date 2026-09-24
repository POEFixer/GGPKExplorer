#pragma once

#include "../Result.h"
#include <cstdint>
#include <string>
#include <vector>

namespace GGPKExp { namespace Parsers {

// Base tile height in pixels (constant in the format)
inline constexpr uint16_t MTP_BASE_TILE_HEIGHT = 40;

struct MtpEntry {
    uint32_t tileId = 0;       // terrain tile type ID (index into .tdt)
    uint16_t variant = 0;      // rotation/variant index (typically 0-3)
    uint16_t tileWidth = 0;    // tile width in atlas pixels
    uint16_t tileRows = 0;     // sub-tile row count (pixel height = tileRows * 40)
    uint16_t atlasX = 0;       // X start position in atlas (pixels)
    uint16_t atlasY = 0;       // Y start position in atlas (pixels)
    uint16_t atlasYEnd = 0;    // Y end = atlasY + tileRows * 40
    uint16_t atlasXEnd = 0;    // X end = atlasX + tileWidth
    uint16_t reserved = 0;     // always 0

    uint16_t pixelHeight() const { return tileRows * MTP_BASE_TILE_HEIGHT; }
};

struct MtpData {
    uint32_t version = 0;      // format version (1)
    uint32_t headerField1 = 0; // always 1
    uint32_t headerField2 = 0; // always 0
    uint32_t entryCount = 0;
    std::vector<MtpEntry> entries;
    std::string minimapPath;     // path to .dds minimap atlas texture
    std::string terrainDataPath; // path to .tdt terrain data file
};

Result<MtpData> ParseMtp(const uint8_t* data, size_t size);

}} // namespace GGPKExp::Parsers
