#pragma once
#include "../../core/parsers/AocTypes.h"
#include <array>
#include <string>
#include <vector>

namespace GGPKExp { namespace Aoc {

class AocLightSystem {
public:
    void SetLights(Parsers::LightsBlock block);
    void Clear();

    // Per-frame state selection (matches by state name; first light whose
    // states contain that name uses it, others fall back to their default).
    void SetActiveStateName(std::string name);
    const std::string& ActiveStateName() const { return m_activeStateName; }

    // Collects all unique state names across all lights (union, sorted).
    std::vector<std::string> CollectStateNames() const;

    std::size_t LightCount() const { return m_block.lights.size(); }
    const Parsers::LightsBlock& Block() const { return m_block; }

    // Returns index into m_block.lights of the first spot_light with
    // castsShadow=true, or -1 if none (caller falls back to Phase 1 sun).
    int DominantShadowCasterIndex() const;

private:
    Parsers::LightsBlock m_block;
    std::string          m_activeStateName;    // empty == use defaultStateIndex
};

}} // namespace GGPKExp::Aoc
