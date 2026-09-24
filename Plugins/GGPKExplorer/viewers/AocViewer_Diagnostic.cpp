#include "AocViewer.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GGPKExp {

// ---------------------------------------------------------------------------
// Diagnostic tab — builds a deterministic markdown dump describing the
// current .aoc file, parser output, scene-graph state, and a hex preview
// of the raw bytes. The point is to have *one click* to produce a report
// that can be pasted straight into Claude (or an issue tracker) when a
// user hits an unexpected issue with a specific file.
//
// T12 — markdown is split into four sub-builders (Summary / Render /
// Attachments / Full) feeding an ImGui tab bar. The Full builder
// concatenates all sections so the original "paste everything to Claude"
// workflow still produces an equivalent dump.
//
// Rules for the markdown:
//   * Deterministic across runs on the same file (no wall-clock timestamps
//     in the body — the header is all that varies per-session and we keep
//     that to a plain static title).
//   * Use fenced code blocks for hex dumps and any raw data.
//   * Keep section order stable so diffs between two captures are readable.
// ---------------------------------------------------------------------------

namespace {

// Dynamic-size printf helper — replaces fixed char buf[N] + snprintf patterns.
// Sizes buffer via snprintf's two-pass trick, so no truncation risk.
template<typename... Args>
std::string FormatString(const char* fmt, Args&&... args) {
    const int n = std::snprintf(nullptr, 0, fmt, std::forward<Args>(args)...);
    if (n <= 0) return {};
    std::string s(static_cast<std::size_t>(n), '\0');
    std::snprintf(s.data(), static_cast<std::size_t>(n) + 1, fmt, std::forward<Args>(args)...);
    return s;
}

// Detect the byte encoding of m_rawBytes by looking for a BOM. The PoE2
// .aoc format is UTF-16LE+BOM on disk so this usually returns "UTF-16 LE
// (BOM)". Empty files and anything without a recognised BOM fall through
// to "unknown/raw bytes" rather than guessing.
const char* DetectEncoding(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        return "UTF-16 LE (BOM)";
    }
    if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        return "UTF-16 BE (BOM)";
    }
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        return "UTF-8 (BOM)";
    }
    if (bytes.empty()) {
        return "(empty)";
    }
    return "unknown (no BOM)";
}

// Human label for AocGridAnchor enum — the underlying int is persisted so
// we prefer to surface the name rather than a magic number.
const char* GridAnchorLabel(int v) {
    switch (static_cast<AocGridAnchor>(v)) {
        case AocGridAnchor::MeshBounds:  return "MeshBounds";
        case AocGridAnchor::Skeleton:    return "Skeleton";
        case AocGridAnchor::WorldOrigin: return "WorldOrigin";
    }
    return "?";
}

} // namespace

// ---------------------------------------------------------------------------
// Summary — File metadata, parser status, resolver state, warnings/errors,
// extends chain. Everything a human needs to understand "what file is this
// and did it parse cleanly?" before diving into render/attachment details.
// ---------------------------------------------------------------------------
void AocViewer::BuildDiagnosticMarkdown_Summary(std::string& out) const {
    const auto& s = m_state;

    out.clear();
    out.reserve(4 * 1024);

    auto addLine = [&](const char* line) {
        out += line;
        out += '\n';
    };

    // ---- Header ----------------------------------------------------------
    addLine("# AocViewer Diagnostic");
    addLine("");

    // ---- File ------------------------------------------------------------
    addLine("## File");
    out += FormatString("- Path: `%s`\n",
                        s.m_filePath.empty() ? "(none)" : s.m_filePath.c_str());
    out += FormatString("- Size: %zu bytes\n", s.m_rawBytes.size());
    out += FormatString("- SHA256: `%s`\n",
                        s.m_sha256Hex.empty() ? "(not computed)" : s.m_sha256Hex.c_str());
    out += FormatString("- Encoding: %s\n", DetectEncoding(s.m_rawBytes));
    addLine("");

    // ---- Parser ----------------------------------------------------------
    // Aggregate sockets / bone groups across every ClientAnimationController
    // in the merged document. There is usually only one, but walking all
    // blocks keeps us honest on multi-CAC variants.
    std::size_t socketsTotal    = 0;
    std::size_t boneGroupsTotal = 0;
    std::size_t clientAnimCount = 0;
    std::size_t skinMeshCount   = 0;
    for (const auto& b : s.m_parsedDoc.merged.blocks) {
        if (b.clientAnim) {
            ++clientAnimCount;
            socketsTotal    += b.clientAnim->sockets.size();
            boneGroupsTotal += b.clientAnim->boneGroups.size();
        }
        if (b.skinMesh) {
            ++skinMeshCount;
        }
    }

    addLine("## Parser");
    out += FormatString("- AOC version: %d\n", s.m_parsedDoc.merged.version);
    out += FormatString("- Blocks: %zu\n", s.m_parsedDoc.merged.blocks.size());
    out += FormatString("- ClientAnimationController blocks: %zu\n", clientAnimCount);
    out += FormatString("- SkinMesh blocks: %zu\n", skinMeshCount);
    out += FormatString("- Sockets: %zu\n", socketsTotal);
    out += FormatString("- Bone Groups: %zu\n", boneGroupsTotal);
    out += FormatString("- Extends: `%s`\n",
                        s.m_parsedDoc.merged.extendsPath.empty()
                            ? "(none)" : s.m_parsedDoc.merged.extendsPath.c_str());

    // inheritanceChain is populated by AocResolver (leaf → ancestor order).
    if (!s.m_parsedDoc.inheritanceChain.empty()) {
        addLine("- Inheritance chain (leaf -> root):");
        for (std::size_t i = 0; i < s.m_parsedDoc.inheritanceChain.size(); ++i) {
            out += FormatString("  - [%zu] %s\n",
                                i, s.m_parsedDoc.inheritanceChain[i].c_str());
        }
    }
    addLine("");

    // ---- Warnings & Errors ----------------------------------------------
    // Conditional section: only emitted when at least one of the three
    // sources has something to report. Keeping it out of the markdown
    // when everything's clean avoids empty-header noise in the paste.
    const auto& sceneWarnings = s.m_sceneGraph.Warnings();
    const bool hasAnyWarning =
        !s.m_parseWarnings.empty() ||
        !s.m_parseErrors.empty()  ||
        !sceneWarnings.empty()    ||
        !s.m_sceneGraph.Error().empty();

    if (hasAnyWarning) {
        addLine("## Warnings & Errors");
        for (const auto& w : s.m_parseWarnings) {
            out += "- W (parser): ";
            out += w;
            out += '\n';
        }
        for (const auto& w : sceneWarnings) {
            out += "- W (scene): ";
            out += w;
            out += '\n';
        }
        for (const auto& e : s.m_parseErrors) {
            out += "- E (parser): ";
            out += e;
            out += '\n';
        }
        if (!s.m_sceneGraph.Error().empty()) {
            out += "- E (scene): ";
            out += s.m_sceneGraph.Error();
            out += '\n';
        }
        addLine("");
    }

    // ---- Resolved References --------------------------------------------
    addLine("## Resolved References");
    const auto& refs = s.m_sceneGraph.ResolvedRefs();
    if (refs.empty()) {
        addLine("- _(none)_");
    } else {
        for (const auto& r : refs) {
            out += "- ";
            out += r;
            out += '\n';
        }
    }
    addLine("");
}

// ---------------------------------------------------------------------------
// Render — render state toggles + hex preview of the raw bytes. Keeps
// low-level rendering diagnostics separate from parser metadata so
// users hunting a render-only issue aren't scrolled past the header.
// ---------------------------------------------------------------------------
void AocViewer::BuildDiagnosticMarkdown_Render(std::string& out) const {
    const auto& s = m_state;

    out.clear();
    out.reserve(2 * 1024);

    auto addLine = [&](const char* line) {
        out += line;
        out += '\n';
    };

    // ---- Render State ---------------------------------------------------
    addLine("## Render State");
    out += FormatString("- Mesh loaded: %s\n",
                        s.m_sceneGraph.HasMesh() ? "yes" : "no");
    if (s.m_sceneGraph.HasMesh()) {
        out += FormatString("- Mesh vertices: %zu\n",
                            s.m_sceneGraph.Mesh().vertices.size());
        out += FormatString("- Submeshes: %zu\n",
                            s.m_sceneGraph.SmData().materialPaths.size());
    }
    out += FormatString("- Skeleton bones: %zu\n",
                        s.m_resolvedSkel.bones.size());
    out += FormatString("- Grid anchor: %d (%s)\n",
                        s.m_view.gridAnchor, GridAnchorLabel(s.m_view.gridAnchor));
    out += FormatString("- Show grid: %s\n",
                        s.m_view.showGrid ? "yes" : "no");
    out += FormatString("- Show sockets: %s\n",
                        s.m_view.showSockets ? "yes" : "no");
    out += FormatString("- Show socket labels: %s\n",
                        s.m_view.showSocketLabels ? "yes" : "no");
    out += FormatString("- Show parent lines: %s\n",
                        s.m_view.showParentLines ? "yes" : "no");
    out += FormatString("- Wireframe: %s\n",
                        s.m_view.wireframe ? "yes" : "no");
    addLine("");

    // ---- Hex Dump (first 256 bytes) -------------------------------------
    // Layout: "AAAA: BB BB BB ... BB" — 16 bytes per row, uppercase hex.
    // Matches `xxd`-style output so the result is readable both pasted
    // into Claude and diffed against other tools.
    addLine("## Hex Dump (first 256 bytes)");
    addLine("```");
    const std::size_t n = std::min<std::size_t>(256, s.m_rawBytes.size());
    for (std::size_t i = 0; i < n; i += 16) {
        std::string row = FormatString("%04zX:", i);
        for (std::size_t j = 0; j < 16 && (i + j) < n; ++j) {
            row += FormatString(" %02X", s.m_rawBytes[i + j]);
        }
        out += row;
        out += '\n';
    }
    if (n == 0) {
        addLine("(empty)");
    }
    addLine("```");
}

// ---------------------------------------------------------------------------
// Attachments — per-child transforms, skinning remaps, attachment list.
// The bulk diagnostic data when inspecting cross-.aoc composition.
// ---------------------------------------------------------------------------
void AocViewer::BuildDiagnosticMarkdown_Attachments(std::string& out) const {
    const auto& s = m_state;

    out.clear();
    out.reserve(4 * 1024);

    // ----- Phase 4 — Attachments -----
    const int nc = s.m_sceneGraph.ChildCount();
    out += "## Attachments (" + std::to_string(nc) + ")\n";
    if (nc == 0) {
        out += "- (no attachments)\n";
    } else {
        for (const auto& child : s.m_sceneGraph.Children()) {
            const bool isAoc =
                (child.sourcePath.size() >= 4 &&
                 (child.sourcePath.compare(child.sourcePath.size()-4, 4, ".aoc") == 0 ||
                  child.sourcePath.compare(child.sourcePath.size()-4, 4, ".AOC") == 0));
            out += "- " + child.socketName + " -> " + child.sourcePath;
            out += isAoc ? " (via .aoc main mesh extraction)" : " (direct .sm)";
            if (!child.warnings.empty()) {
                out += " [" + std::to_string(child.warnings.size()) + " warning(s)]";
            }
            out += "\n";
        }
    }

    // Phase 5.2 — ## Skinning section. Per-attachment dump for diagnostic visibility.
    constexpr std::size_t kRemapDumpLimit = 30;
    out += "\n## Skinning\n\n";
    const auto& children = s.m_sceneGraph.Children();
    if (children.empty()) {
        out += "(no attachments)\n";
    } else {
        for (const auto& child : children) {
            out += FormatString("Attached at \"%s\": %s\n",
                                child.socketName.c_str(),
                                child.sourcePath.c_str());

            out += FormatString("  Skin mode    : %s\n",
                                child.skinMode == Aoc::ChildSkinMode::Skinned ? "Skinned" : "Rigid");

            out += FormatString("  Reason       : %s\n",
                                child.skinModeReason.empty() ? "(none)" : child.skinModeReason.c_str());

            out += FormatString("  Parent bones : %zu\n",
                                s.m_resolvedSkel.bones.size());

            out += FormatString("  Child bones  : raw AST %zu, resolved %zu\n",
                                child.childAst.bones.size(),
                                child.childResolvedSkel.bones.size());

            if (!child.mesh.boneNames.empty()) {
                out += FormatString("  Mesh format  : V01/V02 (local bone shuffle applied, %zu local bones)\n",
                                    child.mesh.boneNames.size());
            } else {
                out += "  Mesh format  : DOLm/V3+ (direct child-skel indexing)\n";
            }

            // Remap table — first 30 entries.
            if (child.skinMode == Aoc::ChildSkinMode::Skinned &&
                !child.boneRemap.empty()) {
                out += "\n  Bone remap (first " + std::to_string(kRemapDumpLimit) + "):\n";
                const std::size_t shown =
                    std::min<std::size_t>(kRemapDumpLimit, child.boneRemap.size());
                for (std::size_t i = 0; i < shown; ++i) {
                    const int pIdx = child.boneRemap[i];
                    const std::string& cname =
                        i < child.childResolvedSkel.bones.size()
                            ? child.childResolvedSkel.bones[i].name
                            : std::string("(?)");
                    if (pIdx >= 0 &&
                        static_cast<std::size_t>(pIdx) < s.m_resolvedSkel.bones.size()) {
                        out += FormatString("    [%3zu] %-24s -> parent[%3d] %s\n",
                                            i,
                                            cname.c_str(),
                                            pIdx,
                                            s.m_resolvedSkel.bones[static_cast<std::size_t>(pIdx)]
                                                .name.c_str());
                    } else {
                        out += FormatString("    [%3zu] %-24s -> UNMATCHED (socket fallback)\n",
                                            i,
                                            cname.c_str());
                    }
                }
                if (child.boneRemap.size() > shown) {
                    out += FormatString("    ... (%zu more)\n",
                                        child.boneRemap.size() - shown);
                }
            }

            // Warnings specific to this child.
            if (!child.warnings.empty()) {
                out += "\n  Warnings:\n";
                for (const auto& w : child.warnings) {
                    out += FormatString("    - %s\n", w.c_str());
                }
            }

            out += "\n";
        }
    }

    // Phase 5.2.1 — ## Transforms section. Per-attachment override dump.
    out += "\n## Transforms\n\n";
    if (children.empty()) {
        out += "(no attachments)\n";
    } else {
        for (const auto& child : children) {
            out += FormatString("### %s  (%s)\n",
                                child.socketName.c_str(),
                                child.sourcePath.c_str());

            if (Aoc::IsTransformIdentity(child)) {
                out += "(identity -- no effect)\n\n";
                continue;
            }

            out += FormatString("Translation:  X=%.3f  Y=%.3f  Z=%.3f  (meters)\n",
                                child.overrideT[0], child.overrideT[1], child.overrideT[2]);

            out += FormatString("Rotation:     X=%.1f  Y=%.1f  Z=%.1f  (Euler XYZ, intrinsic, degrees)\n",
                                child.overrideR[0], child.overrideR[1], child.overrideR[2]);

            out += FormatString("Scale:        X=%.3f  Y=%.3f  Z=%.3f\n",
                                child.overrideS[0], child.overrideS[1], child.overrideS[2]);

            // Full 4x4 row-major matrix dump.
            float m[16];
            Aoc::ComposeOverrideMatrix(child, m);
            out += "\nOverride matrix (row-major):\n";
            for (int row = 0; row < 4; ++row) {
                out += FormatString("  [%.3f, %.3f, %.3f, %.3f]\n",
                                    m[row * 4 + 0], m[row * 4 + 1],
                                    m[row * 4 + 2], m[row * 4 + 3]);
            }
            out += "\n";
        }
    }
}

// ---------------------------------------------------------------------------
// Full — kitchen-sink dump that concatenates Summary + Render + any
// remaining Phase 2/3/4/5 sections (Lights/Audio, Particles, Transitionable,
// Persistence) + Attachments. Produced output is equivalent to the pre-T12
// single-Copy-button dump, preserving the "paste to Claude" workflow.
// ---------------------------------------------------------------------------
void AocViewer::BuildDiagnosticMarkdown_Full(std::string& out) const {
    const auto& s = m_state;

    out.clear();
    out.reserve(8 * 1024);

    // Pull Summary + Render (file/parser/warnings/refs/render/hex).
    {
        std::string summary;
        BuildDiagnosticMarkdown_Summary(summary);
        out += summary;
    }
    {
        std::string render;
        BuildDiagnosticMarkdown_Render(render);
        out += render;
    }

    // ---- Phase 2 — Lights / Audio --------------------------------------
    out += "\n## Phase 2 — Lights / Audio\n\n";

    // Find typed blocks
    const Parsers::LightsBlock*      lightsB = nullptr;
    const Parsers::SoundEventsBlock* soundsB = nullptr;
    for (const auto& b : s.m_parsedDoc.merged.blocks) {
        if (b.lights      && !lightsB) lightsB = b.lights.get();
        if (b.soundEvents && !soundsB) soundsB = b.soundEvents.get();
    }

    // --- Lights block summary --------------------------------------------
    if (lightsB) {
        out += "### Lights block (";
        out += std::to_string(lightsB->lights.size());
        out += " parsed";
        if (lightsB->skippedDirectional > 0) {
            out += ", ";
            out += std::to_string(lightsB->skippedDirectional);
            out += " directional skipped";
        }
        out += ")\n\n";
        for (const auto& ld : lightsB->lights) {
            out += "- **" + ld.name + "** (";
            out += (ld.type == Parsers::LightType::Spot ? "spot" : "point");
            out += ") bone=`" + ld.boneName + "` shadow=";
            out += (ld.castsShadow ? "yes" : "no");
            out += ", states: ";
            for (std::size_t i = 0; i < ld.states.size(); ++i) {
                if (i) out += ", ";
                out += ld.states[i].name;
                if (static_cast<int>(i) == ld.defaultStateIndex) out += "*";
            }
            out += "\n";
        }
        out += "\n";
    } else {
        out += "_No Lights block._\n\n";
    }

    // --- Dominant shadow caster -----------------------------------------
    out += "### Dominant shadow caster\n\n";
    {
        const int idx = s.m_lights.DominantShadowCasterIndex();
        if (idx >= 0) {
            out += "slot " + std::to_string(idx) + " — `"
                +  s.m_lights.Block().lights[static_cast<std::size_t>(idx)].name
                +  "`\n\n";
        } else {
            out += "(none — Phase-1 sun fallback)\n\n";
        }
    }

    // --- SoundEvents summary --------------------------------------------
    out += "### SoundEvents\n\n";
    if (soundsB) {
        out += std::to_string(soundsB->clips.size()) + " clips\n\n";
    } else {
        out += "_No SoundEvents block._\n\n";
    }

    // --- Audio resolution ------------------------------------------------
    out += "### Audio resolution\n\n";
    if (s.m_audio) {
        const auto& miss = s.m_audio->MissingLog();
        out += "operational: ";
        out += (s.m_audio->IsOperational() ? "yes" : "no");
        out += "\n\nmissing references: " + std::to_string(miss.size()) + "\n";
        for (const auto& name : miss) {
            out += "- `" + name + "`\n";
        }
    } else {
        out += "_Audio system not initialised._\n";
    }

    // ---- Phase 3 — Particles --------------------------------------------
    {
        const auto& ps = s.m_particles;
        out += "\n## Particles\n";

        if (ps.EmitterCount() == 0) {
            out += "- (none)\n";
        } else {
            const std::size_t missing = ps.MissingCount();
            const std::size_t uniq    = ps.UniqueFilenames();

            // Group emitters by animationName for readable output.
            std::unordered_map<std::string, std::vector<const Aoc::AocParticleEmitter*>> byAnim;
            for (const auto& e : ps.Emitters()) byAnim[e.animationName].push_back(&e);

            out += "- Animations: " + std::to_string(byAnim.size()) + "\n";
            out += "- Emitters: " + std::to_string(ps.EmitterCount())
                 + " total, " + std::to_string(uniq) + " unique .pet, "
                 + std::to_string(missing) + " MISSING\n";
            if (ps.SkippedUnknownTypes() > 0) {
                out += "- Skipped unknown event types: "
                     + std::to_string(ps.SkippedUnknownTypes()) + "\n";
            }

            // Build a set of valid clip names so we can flag [ORPHAN] for
            // emitters whose animationName matches NO clip in the loaded .ast.
            std::unordered_set<std::string> validClips(
                s.m_animPlayer.AnimNames().begin(), s.m_animPlayer.AnimNames().end());

            for (const auto& kv : byAnim) {
                out += "\n### Animation: \"" + kv.first + "\" (" +
                       std::to_string(kv.second.size()) + " event" +
                       (kv.second.size() == 1 ? "" : "s") + ")";

                if (validClips.find(kv.first) == validClips.end() && !kv.first.empty()) {
                    out += "  [ORPHAN]";
                }
                out += "\n";

                for (const Aoc::AocParticleEmitter* e : kv.second) {
                    out += "- ";
                    out += FormatString("@%.2fs  ", e->time);
                    out += "`" + e->filename + "`";

                    if (e->petExists) out += "  [FOUND]";
                    else              out += "  [MISSING]";
                    if (e->resolvedBoneIndex < 0) out += "  [UNRESOLVED BONE]";

                    if (!e->boneGroup.empty())     out += "  bone_group=" + e->boneGroup;
                    else if (!e->boneName.empty()) out += "  bone_name="  + e->boneName;
                    out += "\n";
                }
            }

            if (!ps.Warnings().empty()) {
                out += "\n### Parse Warnings\n";
                for (const auto& w : ps.Warnings()) out += "- " + w + "\n";
            }
        }
    }

    // ----- Phase 4 — Transitionable -----
    out += "\n## Transitionable\n";
    const auto& tr = s.m_transitions;
    out += std::string("- Source .ot: ") +
           (tr.DiscoveredOtPath().empty() ? "(none)" : tr.DiscoveredOtPath()) + "\n";
    out += std::string("- Source .otc: ") +
           (tr.DiscoveredOtcPath().empty() ? "(none)" : tr.DiscoveredOtcPath()) + "\n";
    out += std::string("- Override: ") +
           (tr.OverrideOtPath().empty() ? "(none)" : tr.OverrideOtPath()) + "\n";
    if (!tr.IsAvailable()) {
        out += "- (no Transitionable block resolved)\n";
    } else {
        const auto* tb = tr.OtFile().transitionable.get();
        out += "- num_states (declared): " + std::to_string(tb->declaredNumStates) + "\n";
        out += "- num_states (computed): " + std::to_string(tb->numStates) + "\n";
        out += std::string("- Binary form: ") + (tb->isBinary ? "yes" : "no") + "\n";
        out += "- Current state: " + std::to_string(tr.CurrentState()) + "\n";
        out += "- Handlers (" + std::to_string(tb->handlers.size()) + "):\n";
        for (const auto& h : tb->handlers) {
            out += "  - " + h.key + " -> ";
            if (!h.extractedClip.empty()) out += "PlayAnimation(" + h.extractedClip + ")";
            else out += "(no PlayAnimation found)";
            out += "\n";
        }
    }

    // Append Attachments + Skinning + Transforms.
    out += "\n";
    {
        std::string attachments;
        BuildDiagnosticMarkdown_Attachments(attachments);
        out += attachments;
    }

    // ----- Phase 5.1 — Persistence -----
    out += "\n## Persistence\n";
    out += "- JSON path: " + s.m_persisted.JsonPath() + "\n";
    if (!s.m_persisted.JsonExistsOnDisk()) {
        out += "- Load status: no JSON file on disk\n";
    } else if (!s.m_persisted.IsLoaded()) {
        out += "- Load status: found but parse failed\n";
    } else {
        out += "- Version: 1\n";
        out += "- Load status: loaded (" +
               std::to_string(s.m_persisted.AttachmentCount()) + " entries)\n";
        if (!s.m_persisted.Entries().empty()) {
            out += "- Attachments (" +
                   std::to_string(s.m_persisted.Entries().size()) + "):\n";
            for (const auto& e : s.m_persisted.Entries()) {
                const char* statusLabel = "[OK]";
                switch (e.status) {
                    case Aoc::AocPersistedAttachments::EntryStatus::MissingSource:
                        statusLabel = "[MISSING source]"; break;
                    case Aoc::AocPersistedAttachments::EntryStatus::MissingSocket:
                        statusLabel = "[MISSING socket]"; break;
                    case Aoc::AocPersistedAttachments::EntryStatus::ParseFailed:
                        statusLabel = "[PARSE FAILED]"; break;
                    default: break;
                }
                out += "  - " + e.socket + " -> " + e.sourcePath + " " + statusLabel;
                if (!e.failureReason.empty()) out += " - " + e.failureReason;
                out += "\n";
            }
        }
        if (s.m_persisted.HasTransitionState()) {
            out += "- Transition state:\n";
            out += "  - Current state: " +
                   std::to_string(s.m_transitions.CurrentState()) + "\n";
            out += "  - Override .ot: " +
                   (s.m_transitions.OverrideOtPath().empty()
                        ? std::string("(none)")
                        : s.m_transitions.OverrideOtPath()) + "\n";
        }
        if (!s.m_persisted.Warnings().empty()) {
            out += "- Warnings:\n";
            for (const auto& w : s.m_persisted.Warnings()) {
                out += "  - " + w + "\n";
            }
        }
    }
}

void AocViewer::RenderDiagnosticTab() {
    // T12 — ImGui tab bar with 4 sub-tabs. Each sub-tab owns its own Copy
    // button that copies only that sub-tab's markdown. Full is the
    // catch-all for "paste everything to Claude".
    auto renderSubTab = [&](const char* label, auto&& builder) {
        if (ImGui::BeginTabItem(label)) {
            std::string md;
            builder(md);

            if (ImGui::Button("Copy")) {
                ImGui::SetClipboardText(md.c_str());
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(copies this sub-tab only)");
            ImGui::Separator();

            // Read-only multiline: matches the pre-T12 presentation
            // (selectable text, proper scrolling with large dumps,
            // monospace-friendly). Size = full remaining region.
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            ImGui::InputTextMultiline("##DiagPreview",
                                      md.data(),
                                      md.size() + 1,
                                      avail,
                                      ImGuiInputTextFlags_ReadOnly);
            ImGui::EndTabItem();
        }
    };

    if (ImGui::BeginTabBar("##DiagSub", ImGuiTabBarFlags_None)) {
        renderSubTab("Summary",     [this](std::string& md) { BuildDiagnosticMarkdown_Summary(md); });
        renderSubTab("Render",      [this](std::string& md) { BuildDiagnosticMarkdown_Render(md); });
        renderSubTab("Attachments", [this](std::string& md) { BuildDiagnosticMarkdown_Attachments(md); });
        renderSubTab("Full",        [this](std::string& md) { BuildDiagnosticMarkdown_Full(md); });
        ImGui::EndTabBar();
    }
}

} // namespace GGPKExp
