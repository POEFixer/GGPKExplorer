#pragma once

#include "../Result.h"
#include "AocTokenizer.h"
#include "AocTypes.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace GGPKExp { namespace Parsers {

// Parses a stream of AocLine tokens into a raw AocFile AST.
// Does NOT follow extends — caller uses AocResolver for that.
// On success: returns AocFile with:
//   - version, extendsPath from header
//   - blocks[] with each top-level block
//   - each block's `properties` (generic AocRawNode tree)
//   - typed views (clientAnim, skinMesh, animController) populated for the 3 known block types
//   - warnings[] for soft issues (unknown keys, duplicate socket names, etc.)
Result<AocFile> ParseAoc(const std::vector<AocLine>& lines);

}} // namespace GGPKExp::Parsers
