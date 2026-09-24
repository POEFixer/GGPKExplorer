#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace GGPKExp {

// Apply a stat_descriptions.csd format string to a set of integer values and
// return the human-readable result.
//
// Supported placeholders:
//   {0}, {1}, ...           → decimal value
//   {0:+d} {1:+d} ...       → signed decimal with '+' for non-negative
//   {0:d}                   → same as {0}
// Bracketed keywords:
//   [Word]                  → "Word" (brackets stripped)
//   [Key|Display]           → "Display" (right side of pipe kept)
//
// Example: FormatStat("{0:+d} to [Armour]", {10}) -> "+10 to Armour"
std::string FormatStat(const std::string& fmt, const std::vector<int32_t>& values);

} // namespace GGPKExp
