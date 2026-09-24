#include "SmParser.h"
#include <cstring>
#include <sstream>
#include <cstdlib>

namespace GGPKExp { namespace Parsers {

// Convert entire UTF-16LE buffer to a UTF-8 string
std::string Utf16LEToUtf8(const uint8_t* data, size_t size) {
    std::string result;
    result.reserve(size / 2);
    for (size_t i = 0; i + 1 < size; i += 2) {
        uint16_t ch = 0;
        std::memcpy(&ch, data + i, 2);

        if (ch == 0) {
            // Null in middle of file: treat as end-of-line or skip
            continue;
        } else if (ch == 0x000D) {
            // CR: skip (we'll handle LF)
            continue;
        } else if (ch == 0x000A) {
            result.push_back('\n');
        } else if (ch < 0x80) {
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

// Extract a quoted string from a line: finds first " and last " and returns contents between
std::string ExtractQuoted(const std::string& line) {
    size_t first = line.find('"');
    if (first == std::string::npos) return "";
    size_t last = line.rfind('"');
    if (last == first) return "";
    return line.substr(first + 1, last - first - 1);
}

// Trim leading/trailing whitespace
std::string TrimWS(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

Result<SmData> ParseSm(const uint8_t* data, size_t size) {
    if (!data || size < 4)
        return Result<SmData>::Fail("Data too small for SM file");

    // Check for UTF-16LE BOM or just try to convert
    std::string text = Utf16LEToUtf8(data, size);
    if (text.empty())
        return Result<SmData>::Fail("Failed to decode UTF-16LE content");

    SmData result;
    std::istringstream stream(text);
    std::string line;

    // Parse line-by-line
    while (std::getline(stream, line)) {
        line = TrimWS(line);
        if (line.empty()) continue;

        // "version N"
        if (line.rfind("version", 0) == 0) {
            result.version = static_cast<uint32_t>(std::strtoul(line.c_str() + 7, nullptr, 10));
        }
        // "SkinnedMeshData \"path\""
        else if (line.rfind("SkinnedMeshData", 0) == 0) {
            result.smdPath = ExtractQuoted(line);
        }
        // "Materials N" followed by material lines of form: `"path.mat" N`
        // where N = count of consecutive submeshes this material covers (run-length).
        else if (line.rfind("Materials", 0) == 0) {
            uint32_t matCount = static_cast<uint32_t>(std::strtoul(line.c_str() + 9, nullptr, 10));
            for (uint32_t i = 0; i < matCount && std::getline(stream, line); i++) {
                line = TrimWS(line);
                std::string path = ExtractQuoted(line);
                uint32_t submeshCount = 1;  // default if trailing number missing
                // Trailing number after the closing quote: `"path.mat" N`
                size_t lastQuote = line.rfind('"');
                if (lastQuote != std::string::npos && lastQuote + 1 < line.size()) {
                    std::string tail = TrimWS(line.substr(lastQuote + 1));
                    // Reject negative / sign-prefixed values before strtoul, which would
                    // otherwise silently wrap (e.g. "-1" -> ULONG_MAX, expanding a single
                    // material across every remaining submesh).
                    if (!tail.empty() && tail.front() != '-' && tail.front() != '+') {
                        char* endp = nullptr;
                        unsigned long parsed = std::strtoul(tail.c_str(), &endp, 10);
                        if (endp != tail.c_str() && parsed > 0 && parsed < 0x7FFFFFFFu)
                            submeshCount = static_cast<uint32_t>(parsed);
                    }
                }
                if (!path.empty()) {
                    SmData::MaterialEntry entry;
                    entry.path = std::move(path);
                    entry.submeshCount = submeshCount;
                    result.materialPaths.push_back(std::move(entry));
                }
            }
        }
        // "BoundingBox minX minY minZ maxX maxY maxZ"
        else if (line.rfind("BoundingBox", 0) == 0) {
            const char* p = line.c_str() + 11;
            char* end = nullptr;
            result.bboxMin[0] = std::strtof(p, &end); p = end;
            result.bboxMin[1] = std::strtof(p, &end); p = end;
            result.bboxMin[2] = std::strtof(p, &end); p = end;
            result.bboxMax[0] = std::strtof(p, &end); p = end;
            result.bboxMax[1] = std::strtof(p, &end); p = end;
            result.bboxMax[2] = std::strtof(p, &end);
        }
        // "BoneGroups N"
        else if (line.rfind("BoneGroups", 0) == 0) {
            result.boneGroupCount = static_cast<uint32_t>(std::strtoul(line.c_str() + 10, nullptr, 10));
        }
    }

    if (result.smdPath.empty())
        return Result<SmData>::Fail("No SkinnedMeshData path found");

    return Result<SmData>::Ok(std::move(result));
}

}} // namespace GGPKExp::Parsers
