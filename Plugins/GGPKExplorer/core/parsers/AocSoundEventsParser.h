#pragma once
#include "AocTypes.h"
#include <string>
#include <vector>

namespace GGPKExp { namespace Parsers {

bool ParseSoundEvents(const AocRawNode&         soundNode,
                      SoundEventsBlock&         out,
                      std::vector<std::string>& warnings,
                      std::vector<std::string>& errors);

}} // namespace GGPKExp::Parsers
