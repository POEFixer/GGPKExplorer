#include "AocLightSystem.h"

#include <set>

namespace GGPKExp { namespace Aoc {

void AocLightSystem::SetLights(Parsers::LightsBlock block) { m_block = std::move(block); }

void AocLightSystem::Clear() {
    m_block = {};
    m_activeStateName.clear();
}

void AocLightSystem::SetActiveStateName(std::string name) { m_activeStateName = std::move(name); }

std::vector<std::string> AocLightSystem::CollectStateNames() const {
    std::set<std::string> names;
    for (const auto& ld : m_block.lights) {
        for (const auto& st : ld.states) {
            if (!st.name.empty()) names.insert(st.name);
        }
    }
    return std::vector<std::string>(names.begin(), names.end());
}

int AocLightSystem::DominantShadowCasterIndex() const {
    for (std::size_t i = 0; i < m_block.lights.size(); ++i) {
        if (m_block.lights[i].type == Parsers::LightType::Spot && m_block.lights[i].castsShadow)
            return static_cast<int>(i);
    }
    return -1;
}

}} // namespace GGPKExp::Aoc
