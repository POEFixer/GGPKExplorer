#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace GGPKExp { namespace Animation {

// Case-insensitive ASCII compare: `a` equal to null-terminated `b`.
bool IEquals(const std::string& a, const char* b);

// Case-insensitive ASCII prefix match: does `a` start with null-terminated `prefix`?
bool IStartsWith(const std::string& a, const char* prefix);

// Returns the index of the preferred default animation given a list of names.
// Selection priority:
//   1) Exact match "idle" (case-insensitive) - first occurrence wins
//   2) Prefix "idle" (case-insensitive) - first occurrence wins
//   3) Index 0 (first animation in list)
// Returns static_cast<std::size_t>(-1) iff `names` is empty.
std::size_t PickDefaultAnimIndex(const std::vector<std::string>& names);

}} // namespace GGPKExp::Animation
