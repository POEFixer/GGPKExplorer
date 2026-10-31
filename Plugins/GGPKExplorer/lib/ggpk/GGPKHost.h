#pragma once
// Host hooks of the bundle library: the ONE file that differs between the two copies,
// POEFixer/Smoother/ggpk/ (POEFixer Logger + crash step) and
// Plugins/GGPKExplorer/lib/ggpk/ (this one: debugger output, no crash step). Every other
// file of the two copies is byte-identical (gate: python tools/ggpk_bundle_test/check_copies.py).
#include <string>
#include <Windows.h>

namespace GGPK::Host {

inline void LogInfo(const std::string& message) {
    OutputDebugStringA(("[GGPKExplorer] " + message + "\n").c_str());
}

inline void LogWarn(const std::string& message) {
    OutputDebugStringA(("[GGPKExplorer] " + message + "\n").c_str());
}

/// The plugin has no crash-step channel.
inline void SetCrashStep(const char* /*step*/) {}

} // namespace GGPK::Host
