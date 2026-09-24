#pragma once

#include "../Result.h"
#include <cstdint>
#include <string>
#include <vector>

namespace GGPKExp { namespace Parsers {

struct SmdHeaderInfo {
    uint32_t vertexStride = 0;
    uint32_t indexStride = 0;
    float bboxMin[3] = {};
    float bboxMax[3] = {};
    std::vector<std::string> submeshNames;
};

Result<SmdHeaderInfo> ParseSmdHeader(const uint8_t* data, size_t size);

}} // namespace GGPKExp::Parsers
