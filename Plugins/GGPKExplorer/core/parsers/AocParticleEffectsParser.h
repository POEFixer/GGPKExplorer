#pragma once
#include "AocTypes.h"
#include <string>
#include <vector>

namespace GGPKExp { namespace Parsers {

// Parse the ParticleEffects block. Returns true iff at least one clip was
// extracted successfully. Warnings (unknown event types, missing fields) are
// appended to `warnings`; fatal errors are appended to `errors`.
bool ParseParticleEffects(const AocRawNode&         node,
                          ParticleEffectsBlock&     out,
                          std::vector<std::string>& warnings,
                          std::vector<std::string>& errors);

}} // namespace GGPKExp::Parsers
