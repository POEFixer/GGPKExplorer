#include "AocTransitionState.h"

#include "../../core/GGPKManager.h"
#include "../../core/parsers/OtParser.h"

#include <algorithm>
#include <cstring>

namespace GGPKExp {
namespace Aoc {

namespace {

bool EndsWithCaseInsensitive(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[s.size() - n + i])));
        char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b) return false;
    }
    return true;
}

std::string StripKnownExt(const std::string& p) {
    for (const char* ext : {".aoc", ".otc", ".ot"}) {
        if (EndsWithCaseInsensitive(p, ext)) {
            return p.substr(0, p.size() - std::strlen(ext));
        }
    }
    return p;
}

std::vector<uint8_t> ReadBytes(GGPKManager* mgr, const std::string& path) {
    if (!mgr) return {};
    void* node = mgr->FindNodeByPath(path);
    if (!node) return {};
    auto result = mgr->ReadFile(node);
    if (!result.success) return {};
    return std::move(result.value);
}

} // anonymous namespace

void AocTransitionState::Clear() {
    m_otFile = Parsers::OtFile{};
    m_currentState = 0;
    m_discoveredOtPath.clear();
    m_discoveredOtcPath.clear();
    // m_overrideOtPath is NOT cleared here — callers manage its lifetime.
    m_warnings.clear();
}

void AocTransitionState::SetCurrentState(int n) {
    const int count = NumStates();
    if (count <= 0) { m_currentState = 0; return; }
    if (n < 0) n = 0;
    if (n > count - 1) n = count - 1;
    m_currentState = n;
}

bool AocTransitionState::Populate(GGPKManager* mgr,
                                   const std::string& aocPath,
                                   const std::string& overridePath) {
    const int preservedState = m_currentState;
    Clear();

    Parsers::OtFile otFile, otcFile;

    if (!overridePath.empty()) {
        m_overrideOtPath = overridePath;
        auto bytes = ReadBytes(mgr, overridePath);
        if (bytes.empty()) {
            m_warnings.push_back("Override .ot not readable: " + overridePath);
            return false;
        }
        if (EndsWithCaseInsensitive(overridePath, ".otc")) {
            Parsers::ParseOtFile(bytes, overridePath, otcFile);
            m_discoveredOtcPath = overridePath;
        } else {
            Parsers::ParseOtFile(bytes, overridePath, otFile);
            m_discoveredOtPath = overridePath;
        }
    } else {
        // m_overrideOtPath is NOT cleared here — callers manage its lifetime
        // (AocViewer maintains the single source of truth via
        // m_transitionOverrideOt). This matches the invariant documented in
        // Clear(): callers pass empty overridePath to mean "use discovered
        // sibling .ot/.otc", not "drop override state".
        const std::string base = StripKnownExt(aocPath);
        const std::string otPath  = base + ".ot";
        const std::string otcPath = base + ".otc";
        if (mgr && mgr->FindNodeByPath(otPath)) {
            auto bytes = ReadBytes(mgr, otPath);
            if (!bytes.empty()) {
                Parsers::ParseOtFile(bytes, otPath, otFile);
                m_discoveredOtPath = otPath;
            }
        }
        if (mgr && mgr->FindNodeByPath(otcPath)) {
            auto bytes = ReadBytes(mgr, otcPath);
            if (!bytes.empty()) {
                Parsers::ParseOtFile(bytes, otcPath, otcFile);
                m_discoveredOtcPath = otcPath;
            }
        }
    }

    if (m_discoveredOtPath.empty() && m_discoveredOtcPath.empty()) {
        return false;
    }

    Parsers::MergeOtFiles(otFile, otcFile, m_otFile);
    // Phase 4 M2 — warnings from otFile + otcFile are already copied into
    // m_otFile.warnings by MergeOtFiles (see OtParser.cpp). Pulling them again
    // from the pre-merge inputs caused every warning to appear twice.
    m_warnings = m_otFile.warnings;

    const int count = NumStates();
    if (count <= 0)                     m_currentState = 0;
    else if (preservedState >= count)   m_currentState = 0;
    else                                 m_currentState = std::max(0, preservedState);

    return IsAvailable();
}

std::string AocTransitionState::ResolveClipForState(
        int n,
        const std::vector<std::string>& clipNamesFromAmd) const {
    auto fallbackIdx = [&]() -> std::string {
        if (n >= 0 && n < static_cast<int>(clipNamesFromAmd.size())) return clipNamesFromAmd[n];
        return {};
    };
    if (!IsAvailable()) return fallbackIdx();

    const auto& handlers = m_otFile.transitionable->handlers;
    for (const auto& h : handlers) {
        if (h.isTransition && h.stateIndex == n && !h.extractedClip.empty())
            return h.extractedClip;
    }
    for (const auto& h : handlers) {
        if (!h.isTransition && h.stateIndex == n && !h.extractedClip.empty())
            return h.extractedClip;
    }
    return fallbackIdx();
}

} // namespace Aoc
} // namespace GGPKExp
