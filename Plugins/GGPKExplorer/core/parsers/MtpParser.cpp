#include "MtpParser.h"
#include "../BinaryReader.h"
#include <cstring>

namespace GGPKExp { namespace Parsers {

// Convert a null-terminated UTF-16LE substring to UTF-8
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

Result<MtpData> ParseMtp(const uint8_t* data, size_t size) {
    if (!data || size < 16)
        return Result<MtpData>::Fail("Data too small for MTP header (need 16 bytes)");

    BinaryReader reader(data, size);
    MtpData result;

    // Header: 4x uint32 (16 bytes)
    result.version = reader.Read<uint32_t>();
    result.headerField1 = reader.Read<uint32_t>();
    result.headerField2 = reader.Read<uint32_t>();
    result.entryCount = reader.Read<uint32_t>();
    if (!reader.IsValid())
        return Result<MtpData>::Fail("Failed to read MTP header");

    if (result.entryCount > 100000)
        return Result<MtpData>::Fail("Entry count too large: " + std::to_string(result.entryCount));

    // Each entry is 20 bytes: u32 + 8x u16
    constexpr size_t entrySize = 20;
    const size_t entriesBytes = static_cast<size_t>(result.entryCount) * entrySize;
    if (reader.Remaining() < entriesBytes)
        return Result<MtpData>::Fail("Not enough data for " + std::to_string(result.entryCount) + " entries");

    result.entries.reserve(result.entryCount);
    for (uint32_t i = 0; i < result.entryCount; ++i) {
        MtpEntry entry;
        entry.tileId    = reader.Read<uint32_t>();
        entry.variant   = reader.Read<uint16_t>();
        entry.tileWidth = reader.Read<uint16_t>();
        entry.tileRows  = reader.Read<uint16_t>();
        entry.atlasX    = reader.Read<uint16_t>();
        entry.atlasY    = reader.Read<uint16_t>();
        entry.atlasYEnd = reader.Read<uint16_t>();
        entry.atlasXEnd = reader.Read<uint16_t>();
        entry.reserved  = reader.Read<uint16_t>();
        result.entries.push_back(entry);
    }

    if (!reader.IsValid())
        return Result<MtpData>::Fail("Failed reading entries");

    // String section: u32 total wchar count, then UTF-16LE data
    if (reader.Remaining() < 4)
        return Result<MtpData>::Ok(std::move(result)); // no string section

    uint32_t totalWcharCount = reader.Read<uint32_t>();
    if (totalWcharCount == 0 || reader.Remaining() < static_cast<size_t>(totalWcharCount) * 2)
        return Result<MtpData>::Ok(std::move(result));

    std::u16string wcharData = reader.ReadWString(totalWcharCount);
    if (!reader.IsValid())
        return Result<MtpData>::Ok(std::move(result));

    // Split on null terminator to extract two strings
    size_t nullPos = wcharData.find(u'\0');
    if (nullPos != std::u16string::npos) {
        result.minimapPath = Utf16ToUtf8(wcharData.c_str(), nullPos);
        // Second string starts after the null terminator
        size_t secondStart = nullPos + 1;
        if (secondStart < wcharData.size()) {
            size_t secondNull = wcharData.find(u'\0', secondStart);
            size_t secondLen = (secondNull != std::u16string::npos)
                ? secondNull - secondStart
                : wcharData.size() - secondStart;
            result.terrainDataPath = Utf16ToUtf8(wcharData.c_str() + secondStart, secondLen);
        }
    }

    return Result<MtpData>::Ok(std::move(result));
}

}} // namespace GGPKExp::Parsers
