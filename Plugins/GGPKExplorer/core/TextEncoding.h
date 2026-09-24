#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace GGPKExp {

enum class TextEncoding { UTF8, UTF16LE, UTF16BE, ASCII, Binary };

TextEncoding DetectEncoding(const uint8_t* data, size_t size);
std::string DecodeToUtf8(const uint8_t* data, size_t size, TextEncoding encoding);

/// Encode UTF-8 string back to the specified encoding (reverse of DecodeToUtf8).
/// Preserves BOM for UTF-16 if hasBom is true.
std::vector<uint8_t> EncodeFromUtf8(const std::string& utf8, TextEncoding encoding, bool hasBom = false);

} // namespace GGPKExp
