#pragma once
#include "AocTypes.h"
#include <string>
#include <vector>

namespace GGPKExp { namespace Parsers {

// Populates `out` from a raw AocRawNode tree already resolved by AocResolver.
// Appends any soft errors to `warnings`. Returns true if the block was present,
// false if empty (even then, errors/warnings may still be appended).
bool ParseLights(const AocRawNode&            lightsNode,
                 LightsBlock&                 out,
                 std::vector<std::string>&    warnings,
                 std::vector<std::string>&    errors);

}} // namespace GGPKExp::Parsers
