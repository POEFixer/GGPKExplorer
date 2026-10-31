#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdint>

namespace GGPKExp { namespace Parsers {

// Parsed default (English) format strings keyed by stat id.
// Example: { "base_physical_damage_reduction_rating" -> "{0:+d} to [Armour]" }
using CsdFormatMap = std::unordered_map<std::string, std::string>;

// Per-language stat format maps. Key = language name as it appears in the
// .csd source ("English" for the default/untagged block; other values such
// as "French", "German", "Russian", "Thai", "Traditional Chinese",
// "Simplified Chinese", "Korean", "Portuguese", "Japanese" match the
// exact string stored after `lang "..."` in the .csd file).
using CsdMultiLangFormatMap = std::unordered_map<std::string, CsdFormatMap>;

// Parse a single .csd file (UTF-16LE bytes or UTF-8 text).
// Populates `out` with stat_id -> format string mappings for the default
// (English) language. `outIncludes` collects `include "<path>"` references
// found in the file so the caller can follow them.
void ParseCsd(const uint8_t* data,
              size_t size,
              CsdFormatMap& out,
              std::vector<std::string>& outIncludes);

// Same as ParseCsd but captures every language block. Each `lang "X"` line
// in the .csd file switches the active target map; the English default
// block (before any `lang` directive) is stored under the key "English".
// Caller's `out` accumulates across multiple files (merge-into semantics).
void ParseCsdMultiLang(const uint8_t* data,
                       size_t size,
                       CsdMultiLangFormatMap& out,
                       std::vector<std::string>& outIncludes);

}} // namespace GGPKExp::Parsers
