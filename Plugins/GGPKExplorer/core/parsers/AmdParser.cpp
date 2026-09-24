#include "AmdParser.h"
#include "SmParser.h" // for Utf16LEToUtf8, TrimWS, ExtractQuoted
#include <sstream>
#include <cstdlib>

namespace GGPKExp { namespace Parsers {

Result<AmdData> ParseAmd(const uint8_t* data, size_t size) {
    if (!data || size < 4)
        return Result<AmdData>::Fail("Data too small for AMD file");

    // Convert UTF-16LE to UTF-8 text
    std::string text = Utf16LEToUtf8(data, size);
    if (text.empty())
        return Result<AmdData>::Fail("Failed to decode UTF-16LE content");

    AmdData result;
    std::istringstream stream(text);
    std::string line;

    // Parse "version N"
    while (std::getline(stream, line)) {
        line = TrimWS(line);
        if (line.empty()) continue;
        if (line.rfind("version", 0) == 0) {
            result.version = static_cast<uint32_t>(std::strtoul(line.c_str() + 7, nullptr, 10));
            break;
        }
    }

    // Read clip count
    uint32_t clipCount = 0;
    while (std::getline(stream, line)) {
        line = TrimWS(line);
        if (line.empty()) continue;
        // The count line should be just a number
        char* end = nullptr;
        unsigned long val = std::strtoul(line.c_str(), &end, 10);
        if (end != line.c_str() && val > 0) {
            clipCount = static_cast<uint32_t>(val);
            break;
        }
    }

    // Parse each clip: quoted name, tab+playmode, tab+framecount, 3 zero lines, empty separator
    for (uint32_t i = 0; i < clipCount; i++) {
        AmdClip clip;

        // Clip name (quoted)
        while (std::getline(stream, line)) {
            line = TrimWS(line);
            if (line.empty()) continue;
            clip.name = ExtractQuoted(line);
            if (clip.name.empty()) clip.name = line; // fallback: use raw line
            break;
        }

        // Play mode line
        while (std::getline(stream, line)) {
            line = TrimWS(line);
            if (line.empty()) continue;
            clip.playMode = line;
            break;
        }

        // Frame count line
        while (std::getline(stream, line)) {
            line = TrimWS(line);
            if (line.empty()) continue;
            clip.frameCount = static_cast<uint32_t>(std::strtoul(line.c_str(), nullptr, 10));
            break;
        }

        // Skip remaining lines until empty separator (3 zero lines + empty line)
        int zeroLinesSeen = 0;
        while (std::getline(stream, line)) {
            line = TrimWS(line);
            if (line.empty()) break; // separator
            if (line == "0") {
                zeroLinesSeen++;
                if (zeroLinesSeen >= 3) {
                    // Read until empty line
                    while (std::getline(stream, line)) {
                        line = TrimWS(line);
                        if (line.empty()) break;
                    }
                    break;
                }
            }
        }

        if (!clip.name.empty()) {
            result.clips.push_back(clip);
        }
    }

    if (result.clips.empty())
        return Result<AmdData>::Fail("No animation clips found");

    return Result<AmdData>::Ok(std::move(result));
}

}} // namespace GGPKExp::Parsers
