#include "AocPersistedAttachments.h"

#include "AocSceneGraph.h"
#include "AocTransitionState.h"
#include "../../core/GGPKManager.h"
#include "../../core/parsers/AocTypes.h"
#include "../../lib/json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool HasSocketInDoc(const GGPKExp::Parsers::AocDocument& doc, const std::string& socketName) {
    for (const auto& blk : doc.merged.blocks) {
        if (!blk.clientAnim) continue;
        for (const auto& sk : blk.clientAnim->sockets) {
            if (sk.name == socketName) return true;
        }
    }
    return false;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (ca != cb) return false;
    }
    return true;
}

// Phase 5.2.1 — read a 3-float array from JSON with finite + non-NaN check.
// Any invalid component forces identity for that triple and appends a warning.
// Returns whether the read succeeded cleanly (true) or fell back (false).
bool ReadFloat3Clean(const nlohmann::json&     j,
                     const char*               fieldLabel,   // "t" / "r" / "s"
                     float                     identityValue, // 0.0f for t/r, 1.0f for s
                     std::array<float, 3>&     out,
                     std::vector<std::string>& warnings,
                     const std::string&        socketName) {
    if (!j.is_array() || j.size() != 3) {
        warnings.push_back("Attachment '" + socketName + "': transform." +
                           fieldLabel + " malformed (not a 3-element array), reset to identity");
        out = { identityValue, identityValue, identityValue };
        return false;
    }
    std::array<float, 3> tmp = { identityValue, identityValue, identityValue };
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& elem = j[i];
        if (!elem.is_number()) {
            warnings.push_back("Attachment '" + socketName + "': transform." +
                               fieldLabel + "[" + std::to_string(i) +
                               "] not a number, reset to identity");
            out = { identityValue, identityValue, identityValue };
            return false;
        }
        const float v = elem.get<float>();
        if (!std::isfinite(v)) {
            warnings.push_back("Attachment '" + socketName + "': transform." +
                               fieldLabel + "[" + std::to_string(i) +
                               "] non-finite, reset to identity");
            out = { identityValue, identityValue, identityValue };
            return false;
        }
        tmp[i] = v;
    }
    out = tmp;
    return true;
}

} // anonymous namespace

namespace GGPKExp {
namespace Aoc {

bool AocPersistedAttachments::Populate(const std::string&          pluginDir,
                                       const std::string&          aocPath,
                                       GGPKManager*                mgr,
                                       AocSceneGraph&              sceneGraph,
                                       const Parsers::AocDocument& parsedDoc,
                                       AocTransitionState&         transitions,
                                       LoadResult&                 outResult) {
    outResult = LoadResult{};
    Clear();

    m_jsonPath = DeriveJsonPath(pluginDir, aocPath);
    if (m_jsonPath.empty()) return false;

    const std::filesystem::path jsonFsPath(m_jsonPath);
    std::error_code ec;
    if (!std::filesystem::exists(jsonFsPath, ec)) {
        return true;  // No JSON -> no-op, success.
    }
    m_jsonExists = true;

    std::ifstream in(jsonFsPath);
    if (!in.good()) {
        m_warnings.push_back("Failed to open JSON file");
        return false;
    }

    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        m_warnings.push_back(std::string("JSON parse error: ") + e.what());
        return false;
    }

    // Version tolerance.
    int version = 1;
    if (j.contains("version") && j["version"].is_number_integer()) {
        version = j["version"].get<int>();
        if (version < 1) {
            m_warnings.push_back("JSON version " + std::to_string(version) + " < 1, treating as v1");
            version = 1;
        }
    } else {
        m_warnings.push_back("JSON missing 'version' field, treating as v1");
    }
    if (version > 1) {
        m_warnings.push_back("JSON version " + std::to_string(version) + " > 1, best-effort parse");
    }

    // aocPath sanity check (non-fatal, case-insensitive).
    if (j.contains("aocPath") && j["aocPath"].is_string()) {
        const std::string jsonAocPath = j["aocPath"].get<std::string>();
        if (!EqualsIgnoreCase(jsonAocPath, aocPath)) {
            m_warnings.push_back("JSON aocPath mismatch: '" + jsonAocPath + "' vs '" + aocPath + "'");
        }
    }

    // Apply transitionState FIRST (it may re-Populate transitions if override present).
    if (j.contains("transitionState") && j["transitionState"].is_object()) {
        const auto& st = j["transitionState"];
        std::string overridePath;
        if (st.contains("overrideOtPath") && st["overrideOtPath"].is_string()) {
            overridePath = st["overrideOtPath"].get<std::string>();
        }
        if (!overridePath.empty()) {
            if (!transitions.Populate(mgr, aocPath, overridePath)) {
                // T7 (Bug B5) — do NOT silently clear the override.
                // AocTransitionState::Populate already preserves m_overrideOtPath
                // across Clear()/failure, and AocTransitionState::Populate with
                // an empty overridePath leaves m_overrideOtPath untouched, so
                // the sibling-discovery fallback does not erase it. Flag the
                // failure on the caller-visible result so a toast can surface,
                // and also record it in the persistent warning list for the
                // Info/Diagnostic tabs.
                const std::string msg =
                    "Override .ot '" + overridePath + "' failed to parse; preserving override path, falling back to sibling discovery";
                m_warnings.push_back(msg);
                outResult.overrideOtFailed = true;
                outResult.overrideOtWarnings.push_back("Override .ot failed to parse: " + overridePath);
                transitions.Populate(mgr, aocPath, "");
            }
        }
        if (transitions.IsAvailable() &&
            st.contains("currentStateIndex") && st["currentStateIndex"].is_number_integer()) {
            const int idx = st["currentStateIndex"].get<int>();
            transitions.SetCurrentState(idx);
            outResult.transitionStateRestored = true;
        }
        m_hasState = outResult.transitionStateRestored;   // Phase 5.1 fix — reflect actual restore result
    }

    // Apply attachments.
    if (j.contains("attachments") && j["attachments"].is_array()) {
        for (const auto& a : j["attachments"]) {
            if (!a.is_object()) continue;
            Entry e;
            if (a.contains("socket") && a["socket"].is_string())
                e.socket = a["socket"].get<std::string>();
            if (a.contains("sourcePath") && a["sourcePath"].is_string())
                e.sourcePath = a["sourcePath"].get<std::string>();
            if (e.socket.empty() || e.sourcePath.empty()) {
                e.status = EntryStatus::ParseFailed;
                e.failureReason = "malformed JSON entry";
                m_entries.push_back(std::move(e));
                outResult.skippedAttachments++;
                continue;
            }

            if (!HasSocketInDoc(parsedDoc, e.socket)) {
                e.status = EntryStatus::MissingSocket;
                e.failureReason = "socket '" + e.socket + "' not in parsed doc";
                m_entries.push_back(std::move(e));
                outResult.skippedAttachments++;
                continue;
            }

            void* node = mgr ? mgr->FindNodeByPath(e.sourcePath) : nullptr;
            if (!node) {
                e.status = EntryStatus::MissingSource;
                e.failureReason = "source path not found in GGPK";
                m_entries.push_back(std::move(e));
                outResult.skippedAttachments++;
                continue;
            }

            auto readRes = mgr->ReadFile(node);
            if (!readRes.success) {
                e.status = EntryStatus::MissingSource;
                e.failureReason = "GGPK read failed";
                m_entries.push_back(std::move(e));
                outResult.skippedAttachments++;
                continue;
            }

            std::string attachErr;
            if (!sceneGraph.AttachChild(e.socket, e.sourcePath, readRes.value, mgr, attachErr)) {
                e.status = EntryStatus::ParseFailed;
                e.failureReason = attachErr.empty() ? "AttachChild returned false" : attachErr;
                m_entries.push_back(std::move(e));
                outResult.skippedAttachments++;
                continue;
            }

            // Phase 5.2.1 — apply optional transform override if present in JSON.
            if (a.contains("transform") && a["transform"].is_object()) {
                Aoc::AocChildMesh* live = sceneGraph.FindChildBySocket(e.socket);
                if (live) {
                    const auto& tj = a["transform"];
                    if (tj.contains("t")) {
                        ReadFloat3Clean(tj["t"], "t", 0.0f, live->overrideT,
                                        m_warnings, e.socket);
                    }
                    if (tj.contains("r")) {
                        ReadFloat3Clean(tj["r"], "r", 0.0f, live->overrideR,
                                        m_warnings, e.socket);
                    }
                    if (tj.contains("s")) {
                        ReadFloat3Clean(tj["s"], "s", 1.0f, live->overrideS,
                                        m_warnings, e.socket);
                        // Scale clamp — [0.001, 100.0] per spec §6.3.
                        for (std::size_t k = 0; k < 3; ++k) {
                            const float v = live->overrideS[k];
                            if (v < 0.001f || v > 100.0f) {
                                m_warnings.push_back(
                                    "Attachment '" + e.socket + "': transform.s[" +
                                    std::to_string(k) + "] out of range [0.001, 100.0], clamped");
                                live->overrideS[k] = std::max(0.001f, std::min(100.0f, v));
                            }
                        }
                    }
                }
            }

            e.status = EntryStatus::Applied;
            m_entries.push_back(std::move(e));
            outResult.restoredAttachments++;
        }
    }

    m_loaded = true;
    return true;
}

void AocPersistedAttachments::Save(const std::string&        pluginDir,
                                   const std::string&        aocPath,
                                   const AocSceneGraph&      sceneGraph,
                                   const AocTransitionState& transitions) {
    const std::string jsonPath = DeriveJsonPath(pluginDir, aocPath);
    if (jsonPath.empty()) return;

    const bool noAttachments   = (sceneGraph.ChildCount() == 0);
    const bool stateDefault    = IsStateDefault(transitions);
    // T7 (Bug B5) — an override path that currently fails to parse is not
    // reflected by IsAvailable(). Persist the state block whenever there's
    // anything worth keeping: available Transitionable data, a manually
    // selected override path (even if parsing failed this session), or a
    // non-default current state index.
    const bool hasOverridePath = !transitions.OverrideOtPath().empty();
    const bool hasState        = transitions.IsAvailable() || hasOverridePath;

    // Empty-state policy: nothing to persist -> delete existing JSON.
    if (noAttachments && stateDefault) {
        std::error_code ec;
        std::filesystem::remove(jsonPath, ec);
        m_jsonExists = false;
        return;
    }

    nlohmann::json j;
    j["version"] = 1;
    j["aocPath"] = aocPath;

    nlohmann::json attachmentsArr = nlohmann::json::array();
    for (const auto& child : sceneGraph.Children()) {
        nlohmann::json entry;
        entry["socket"]     = child.socketName;
        entry["sourcePath"] = child.sourcePath;

        // Phase 5.2.1 — write transform only when non-identity (keeps JSON
        // clean for default attachments; matches Phase 5.1 file layout when
        // no user edits have happened).
        if (!Aoc::IsTransformIdentity(child)) {
            nlohmann::json t = nlohmann::json::object();
            t["t"] = nlohmann::json::array({ child.overrideT[0], child.overrideT[1], child.overrideT[2] });
            t["r"] = nlohmann::json::array({ child.overrideR[0], child.overrideR[1], child.overrideR[2] });
            t["s"] = nlohmann::json::array({ child.overrideS[0], child.overrideS[1], child.overrideS[2] });
            entry["transform"] = std::move(t);
        }

        attachmentsArr.push_back(std::move(entry));
    }
    j["attachments"] = std::move(attachmentsArr);

    if (hasState) {
        nlohmann::json stateObj;
        stateObj["currentStateIndex"] = transitions.CurrentState();
        // T7 (Bug B5) — always write overrideOtPath when it's non-empty, even
        // if parsing failed this session. Losing it here is exactly the silent
        // data loss this task fixes.
        stateObj["overrideOtPath"]    = transitions.OverrideOtPath();
        j["transitionState"] = stateObj;
    }

    const std::filesystem::path jsonFsPath(jsonPath);
    std::error_code ec;
    std::filesystem::create_directories(jsonFsPath.parent_path(), ec);
    if (ec) return;  // Non-fatal: session state preserved.

    std::ofstream out(jsonFsPath, std::ios::trunc);
    if (!out.good()) return;
    out << j.dump(2);
    if (!out.good()) return;   // Partial write — do not claim success.
    m_jsonExists = true;
}

void AocPersistedAttachments::ClearPersistence(const std::string& pluginDir,
                                               const std::string& aocPath) {
    const std::string path = DeriveJsonPath(pluginDir, aocPath);
    if (!path.empty()) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        // Ignore ec - missing file is non-fatal.
    }
    Clear();
}

void AocPersistedAttachments::Clear() {
    m_loaded     = false;
    m_jsonExists = false;
    m_hasState   = false;
    m_jsonPath.clear();
    m_entries.clear();
    m_warnings.clear();
}

std::string AocPersistedAttachments::DeriveJsonPath(const std::string& pluginDir,
                                                    const std::string& aocPath) {
    if (pluginDir.empty() || aocPath.empty()) return {};
    std::filesystem::path p(pluginDir);
    p /= "user_data";
    p /= "attachments";
    p /= aocPath;      // forward-slash GGPK path works on Windows via filesystem::path
    p += ".json";
    return p.string();
}

bool AocPersistedAttachments::IsStateDefault(const AocTransitionState& t) {
    if (!t.IsAvailable()) return true;          // no Transitionable -> trivially default
    if (t.CurrentState() != 0) return false;
    if (!t.OverrideOtPath().empty()) return false;
    return true;
}

} // namespace Aoc
} // namespace GGPKExp
