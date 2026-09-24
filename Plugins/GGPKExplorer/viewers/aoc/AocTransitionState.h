#pragma once
#include "../../core/parsers/AocTypes.h"
#include <string>
#include <vector>

namespace GGPKExp {
class GGPKManager;
namespace Aoc {

// Owns the Transitionable view for one loaded .aoc file. Discovery finds
// sibling .ot / .otc via filename convention; manual override (user-picked path)
// supersedes convention when set.
class AocTransitionState {
public:
    AocTransitionState() = default;

    // Populate from sibling files. Returns true iff Transitionable data was
    // found after merge. Either argument may be empty.
    //   aocPath      — current .aoc GGPK path (used to derive convention siblings).
    //   overridePath — when non-empty, skip convention and parse this path directly.
    bool Populate(GGPKManager* mgr,
                  const std::string& aocPath,
                  const std::string& overridePath);

    void Clear();

    bool IsAvailable()  const { return m_otFile.transitionable != nullptr; }
    int  NumStates()    const { return m_otFile.transitionable ? m_otFile.transitionable->numStates : 0; }
    int  CurrentState() const { return m_currentState; }
    void SetCurrentState(int n);

    // Resolve state index → clip name.
    //   Order: on_transition_to_N handler → create_state_N handler → clipNamesFromAmd[N] → "".
    std::string ResolveClipForState(int n,
                                    const std::vector<std::string>& clipNamesFromAmd) const;

    const Parsers::OtFile&           OtFile()            const { return m_otFile; }
    const std::string&               DiscoveredOtPath()  const { return m_discoveredOtPath; }
    const std::string&               DiscoveredOtcPath() const { return m_discoveredOtcPath; }
    const std::string&               OverrideOtPath()    const { return m_overrideOtPath; }
    const std::vector<std::string>&  Warnings()          const { return m_warnings; }

private:
    Parsers::OtFile          m_otFile;
    int                      m_currentState = 0;
    std::string              m_discoveredOtPath;
    std::string              m_discoveredOtcPath;
    std::string              m_overrideOtPath;
    std::vector<std::string> m_warnings;
};

}} // namespace GGPKExp::Aoc
