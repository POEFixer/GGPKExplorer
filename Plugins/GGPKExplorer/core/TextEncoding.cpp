#include "TextEncoding.h"
#include <algorithm>
#include <Windows.h>

namespace GGPKExp {

TextEncoding DetectEncoding(const uint8_t* data, size_t size) {
    if (!data || size == 0)
        return TextEncoding::Binary;

    // Check BOM signatures
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
        return TextEncoding::UTF8;

    if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE)
        return TextEncoding::UTF16LE;

    if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF)
        return TextEncoding::UTF16BE;

    // Heuristic: UTF-16 LE without BOM (common in PoE2 .csd/.tsi/.clt files)
    // Check if alternating null bytes exist in odd positions (little-endian ASCII chars)
    if (size >= 8) {
        int samples = static_cast<int>(std::min<size_t>(size, 4096)) & ~1;
        int nullAtOdd = 0;
        int nullAtEven = 0;
        int totalPairs = samples / 2;

        for (int i = 0; i < samples; i += 2) {
            if (data[i] == 0) nullAtEven++;
            if (data[i + 1] == 0) nullAtOdd++;
        }

        // UTF-16 LE: most high bytes (odd positions) are null for ASCII-range chars
        if (nullAtOdd > totalPairs / 2 && nullAtEven < totalPairs / 10)
            return TextEncoding::UTF16LE;

        // UTF-16 BE: most low bytes (even positions) are null
        if (nullAtEven > totalPairs / 2 && nullAtOdd < totalPairs / 10)
            return TextEncoding::UTF16BE;
    }

    // Check for ASCII (all bytes < 0x80, no nulls)
    bool hasNull = false;
    bool hasHighByte = false;
    size_t checkLen = std::min<size_t>(size, 8192);
    for (size_t i = 0; i < checkLen; ++i) {
        if (data[i] == 0) { hasNull = true; break; }
        if (data[i] >= 0x80) hasHighByte = true;
    }

    if (hasNull)
        return TextEncoding::Binary;

    if (!hasHighByte)
        return TextEncoding::ASCII;

    // Has high bytes but no nulls: assume UTF-8
    return TextEncoding::UTF8;
}

std::string DecodeToUtf8(const uint8_t* data, size_t size, TextEncoding encoding) {
    if (!data || size == 0)
        return {};

    switch (encoding) {
    case TextEncoding::UTF8: {
        // Skip BOM if present
        size_t offset = 0;
        if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
            offset = 3;
        return std::string(reinterpret_cast<const char*>(data + offset), size - offset);
    }

    case TextEncoding::ASCII:
        return std::string(reinterpret_cast<const char*>(data), size);

    case TextEncoding::UTF16LE: {
        // Skip BOM if present
        size_t offset = 0;
        if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE)
            offset = 2;

        const wchar_t* wstr = reinterpret_cast<const wchar_t*>(data + offset);
        int wlen = static_cast<int>((size - offset) / sizeof(wchar_t));
        if (wlen <= 0)
            return {};

        int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, nullptr, 0, nullptr, nullptr);
        if (needed <= 0)
            return {};

        std::string result(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, result.data(), needed, nullptr, nullptr);
        return result;
    }

    case TextEncoding::UTF16BE: {
        // Skip BOM if present
        size_t offset = 0;
        if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF)
            offset = 2;

        size_t byteCount = size - offset;
        size_t wcharCount = byteCount / 2;
        if (wcharCount == 0)
            return {};

        // Byte-swap BE -> LE, then use WideCharToMultiByte
        std::wstring leStr(wcharCount, L'\0');
        const uint8_t* src = data + offset;
        for (size_t i = 0; i < wcharCount; ++i) {
            leStr[i] = static_cast<wchar_t>(
                (static_cast<uint16_t>(src[i * 2]) << 8) | src[i * 2 + 1]);
        }

        int needed = WideCharToMultiByte(CP_UTF8, 0, leStr.c_str(),
                                          static_cast<int>(wcharCount),
                                          nullptr, 0, nullptr, nullptr);
        if (needed <= 0)
            return {};

        std::string result(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, leStr.c_str(),
                            static_cast<int>(wcharCount),
                            result.data(), needed, nullptr, nullptr);
        return result;
    }

    case TextEncoding::Binary:
    default:
        // Binary data: return raw bytes as-is (caller should use hex viewer)
        return std::string(reinterpret_cast<const char*>(data), size);
    }
}

std::vector<uint8_t> EncodeFromUtf8(const std::string& utf8, TextEncoding encoding, bool hasBom) {
    switch (encoding) {
    case TextEncoding::UTF16LE: {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                        static_cast<int>(utf8.size()), nullptr, 0);
        if (wlen <= 0)
            return {utf8.begin(), utf8.end()};

        std::vector<uint8_t> result;
        result.reserve((hasBom ? 2 : 0) + static_cast<size_t>(wlen) * 2);

        if (hasBom) {
            result.push_back(0xFF);
            result.push_back(0xFE);
        }

        std::wstring wstr(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                            static_cast<int>(utf8.size()), wstr.data(), wlen);

        const auto* raw = reinterpret_cast<const uint8_t*>(wstr.data());
        result.insert(result.end(), raw, raw + static_cast<size_t>(wlen) * 2);
        return result;
    }

    case TextEncoding::UTF16BE: {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                        static_cast<int>(utf8.size()), nullptr, 0);
        if (wlen <= 0)
            return {utf8.begin(), utf8.end()};

        std::wstring wstr(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                            static_cast<int>(utf8.size()), wstr.data(), wlen);

        std::vector<uint8_t> result;
        result.reserve((hasBom ? 2 : 0) + static_cast<size_t>(wlen) * 2);

        if (hasBom) {
            result.push_back(0xFE);
            result.push_back(0xFF);
        }

        for (int i = 0; i < wlen; ++i) {
            uint16_t ch = static_cast<uint16_t>(wstr[i]);
            result.push_back(static_cast<uint8_t>((ch >> 8) & 0xFF));
            result.push_back(static_cast<uint8_t>(ch & 0xFF));
        }
        return result;
    }

    case TextEncoding::UTF8:
        if (hasBom) {
            std::vector<uint8_t> result;
            result.reserve(3 + utf8.size());
            result.push_back(0xEF);
            result.push_back(0xBB);
            result.push_back(0xBF);
            result.insert(result.end(), utf8.begin(), utf8.end());
            return result;
        }
        return {utf8.begin(), utf8.end()};

    case TextEncoding::ASCII:
    case TextEncoding::Binary:
    default:
        return {utf8.begin(), utf8.end()};
    }
}

} // namespace GGPKExp
