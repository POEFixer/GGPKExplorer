#pragma once

#include "../Result.h"
#include <cstdint>
#include <string>
#include <vector>

namespace GGPKExp { namespace Parsers {

struct SmData {
    uint32_t version = 0;
    std::string smdPath;

    /// Run-length encoded material-to-submesh binding parsed from `.sm` Materials block.
    /// Each entry: one `.mat` path and the number of consecutive submeshes (submeshCount)
    /// it covers in submesh order. For V01/V02 sum(submeshCount) typically equals submesh
    /// count. For DOLm the numbers may be inflated; expansion should cap at remaining
    /// submesh count.
    struct MaterialEntry {
        std::string path;
        uint32_t    submeshCount = 1;  // run-length; default 1 if number missing
    };
    std::vector<MaterialEntry> materialPaths;

    float bboxMin[3] = {};
    float bboxMax[3] = {};
    uint32_t boneGroupCount = 0;
};

// Shared text utility functions (used by SmParser)
std::string Utf16LEToUtf8(const uint8_t* data, size_t size);
std::string ExtractQuoted(const std::string& line);
std::string TrimWS(const std::string& s);

Result<SmData> ParseSm(const uint8_t* data, size_t size);

}} // namespace GGPKExp::Parsers
