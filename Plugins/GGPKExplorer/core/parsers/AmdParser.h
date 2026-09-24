#pragma once

#include "../Result.h"
#include <cstdint>
#include <string>
#include <vector>

namespace GGPKExp { namespace Parsers {

struct AmdClip {
    std::string name;
    std::string playMode; // "once" or "loop"
    uint32_t frameCount = 0;
};

struct AmdData {
    uint32_t version = 0;
    std::vector<AmdClip> clips;
};

Result<AmdData> ParseAmd(const uint8_t* data, size_t size);

}} // namespace GGPKExp::Parsers
