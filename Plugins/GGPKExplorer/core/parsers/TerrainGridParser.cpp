#include "TerrainGridParser.h"
#include "../BinaryReader.h"
#include <cstring>

namespace GGPKExp { namespace Parsers {

static std::string Utf16ToUtf8(const char16_t* str, size_t maxLen) {
    std::string result;
    for (size_t i = 0; i < maxLen && str[i] != 0; ++i) {
        char16_t ch = str[i];
        if (ch < 0x80) {
            result.push_back(static_cast<char>(ch));
        } else if (ch < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            result.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xE0 | (ch >> 12)));
            result.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }
    return result;
}

Result<TdtData> ParseTdt(const uint8_t* data, size_t size) {
    if (!data || size < 8)
        return Result<TdtData>::Fail("Data too small for TDT header (need 8 bytes)");

    BinaryReader reader(data, size);
    TdtData result;
    result.rawData.assign(data, data + size);

    // Header: version + total wchar count
    result.version = reader.Read<uint32_t>();
    uint32_t totalWcharCount = reader.Read<uint32_t>();
    if (!reader.IsValid())
        return Result<TdtData>::Fail("Failed to read TDT header");

    if (totalWcharCount > 100000)
        return Result<TdtData>::Fail("Wchar count too large: " + std::to_string(totalWcharCount));

    // Read the UTF-16LE string section
    size_t stringBytesNeeded = static_cast<size_t>(totalWcharCount) * 2;
    if (reader.Remaining() < stringBytesNeeded)
        return Result<TdtData>::Fail("Not enough data for string section");

    std::u16string wcharData = reader.ReadWString(totalWcharCount);
    if (!reader.IsValid())
        return Result<TdtData>::Fail("Failed to read string section");

    // Split on null terminators to extract up to 3 strings
    size_t pos = 0;
    auto extractNext = [&]() -> std::string {
        if (pos >= wcharData.size()) return {};
        size_t nullPos = wcharData.find(u'\0', pos);
        size_t len = (nullPos != std::u16string::npos) ? nullPos - pos : wcharData.size() - pos;
        std::string s = Utf16ToUtf8(wcharData.c_str() + pos, len);
        pos = (nullPos != std::u16string::npos) ? nullPos + 1 : wcharData.size();
        return s;
    };

    result.tgtPath = extractNext();        // always present
    result.tileName = extractNext();       // optional
    result.groundTypePath = extractNext(); // optional

    // Data section starts here
    result.dataSectionOffset = reader.Position();

    // Parse known data section fields if enough data remains
    // Layout: u32 gridW, u32 zero, u32 gridH, 14B edges, 2B tile_dims, 16B ground_refs
    constexpr size_t minDataSection = 4 + 4 + 4 + 14 + 2 + 16; // = 44 bytes
    if (reader.Remaining() >= minDataSection) {
        result.gridWidth = reader.Read<uint32_t>();
        reader.Read<uint32_t>(); // always 0
        result.gridHeight = reader.Read<uint32_t>();

        // 14 bytes edge data
        auto edgeBytes = reader.ReadBytes(14);
        if (edgeBytes.size() == 14)
            std::memcpy(result.edgeData, edgeBytes.data(), 14);

        // 2 bytes tile dimensions
        result.tileWidth = reader.Read<uint8_t>();
        result.tileHeight = reader.Read<uint8_t>();

        // 4 uint32 ground type references
        for (int i = 0; i < 4; ++i)
            result.groundTypeRefs[i] = reader.Read<uint32_t>();
    }

    return Result<TdtData>::Ok(std::move(result));
}

}} // namespace GGPKExp::Parsers
