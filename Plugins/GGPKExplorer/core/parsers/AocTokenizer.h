#pragma once

#include "../Result.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace GGPKExp { namespace Parsers {

struct AocLine {
    enum class Kind {
        Empty,
        HeaderKV,     // "version N" or "extends "..." "
        BlockOpen,    // bare identifier on its own line (followed by "{" line)
        BraceOpen,    // just "{"
        BlockClose,   // just "}"
        Property,     // key = value, potentially inside a block
    };

    int         indent     = 0;      // leading TAB count
    std::string raw;                  // original line text (for error messages)
    std::string key;                  // parsed identifier or block name
    std::string value;                // parsed value (quotes stripped); empty for BlockOpen/BraceOpen/BlockClose
    Kind        kind       = Kind::Empty;
    std::size_t lineNumber = 0;       // 1-based, logical line number (multi-line '...' strings count as one)
};

// Tokenizes a UTF-16LE .aoc file (with BOM) into a vector of logical lines.
// Handles:
//   - BOM detection (0xFF 0xFE)
//   - UTF-16LE → UTF-8 conversion
//   - CRLF → LF normalization
//   - Single-quoted multi-line string values ('...' may span lines — merged to one logical line)
//   - TAB-based indent counting; space-based indent → hard error
//   - Line comment stripping (// to EOL, not inside "..." or '...')
//
// Returns Result<vector<AocLine>>:
//   - Ok with lines if well-formed
//   - Error with line number and description if format broken
Result<std::vector<AocLine>> TokenizeAoc(const uint8_t* bytes, std::size_t length);

}} // namespace GGPKExp::Parsers
