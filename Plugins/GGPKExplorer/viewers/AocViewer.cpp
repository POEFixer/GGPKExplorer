#include "AocViewer.h"
#include "aoc/AocAudioSystem.h"
#include "aoc/AocUiHelpers.h"
#include "mesh/MeshRenderer.h"
#include "mesh/MeshMaterial.h"
#include "animation/MatrixMath.h"
#include "../core/GGPKManager.h"
#include "../core/parsers/AocTokenizer.h"
#include "../core/parsers/AocParser.h"
#include "../core/parsers/AocResolver.h"
#include "../core/parsers/AocSkeletonAugmenter.h"
#include "../core/parsers/AstParser.h"
#include "../core/parsers/AstSkeletonBuilder.h"
#include "../ui/AttachBrowseModal.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <windows.h>
#include <bcrypt.h>

namespace GGPKExp {

namespace {

// Extracts the directory portion of a GGPK path (everything up to and
// including the final slash). Returns an empty string for paths with no
// separator. Used to seed AocAudioSystem's base-directory hint for relative
// sound file resolution.
std::string ExtractDirectoryPath(const std::string& p) {
    const auto i = p.find_last_of("/\\");
    return (i == std::string::npos) ? std::string{} : p.substr(0, i + 1);
}

// Computes a hex-encoded SHA256 digest of the given buffer using the
// Windows BCrypt API (bcrypt.lib is already linked via GGPKExplorer.vcxproj
// for the GGPK directory hashing chain in lib/ggpk/GGPK.cpp). Returns an
// empty string on failure; callers should treat that as "SHA256 not
// available" rather than a hard error, since the Info tab displays this as
// an advisory field.
std::string ComputeSha256Hex(const std::vector<uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    BCRYPT_HASH_HANDLE hHash = nullptr;
    uint8_t digest[32] = {};
    bool ok = false;
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) == 0) {
        if (BCryptHashData(hHash,
                           const_cast<PUCHAR>(bytes.data()),
                           static_cast<ULONG>(bytes.size()),
                           0) == 0) {
            if (BCryptFinishHash(hHash, digest, 32, 0) == 0) {
                ok = true;
            }
        }
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (!ok) return {};

    char hex[65] = {};
    for (int i = 0; i < 32; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return std::string(hex);
}

} // namespace

// --------------------------------------------------------------------------
// Lifecycle
// --------------------------------------------------------------------------

AocViewer::AocViewer() {
    m_browseModal = std::make_unique<AttachBrowseModal>();
}
AocViewer::~AocViewer() = default;

void AocViewer::Initialize(ID3D11Device* device) {
    m_state.m_device = device;
    if (m_browseModal) m_browseModal->SetDevice(device);   // Phase 5.1.5c
    // MeshRenderer + MeshMaterial created lazily on first LoadData (they need GGPKManager).

    // Phase 2 — construct the audio system once and reuse across file loads.
    // Configure(mgr, aocDir) is deferred to DoParsing because we don't know
    // the file path yet. Start() initialises the miniaudio device; it fails
    // silently on systems without audio hardware, in which case
    // IsOperational() returns false and Trigger() becomes a no-op.
    m_state.m_audio = std::make_unique<Aoc::AocAudioSystem>();
    m_state.m_audio->Start();
}

void AocViewer::SetContext(GGPKManager* mgr, VirtualTree* tree) {
    m_state.m_mgr = mgr;
    m_state.m_tree = tree;
}

void AocViewer::SetDatSchema(DatSchema* schema) {
    m_datSchemaCached = schema;
    if (m_browseModal) {
        m_browseModal->SetSchema(schema);
    }
}

void AocViewer::SetOnSave(std::function<void(const std::vector<uint8_t>&)> cb) {
    m_state.m_onSave = std::move(cb);
}

void AocViewer::SetSelectionProvider(std::function<AocContentSelection()> cb) {
    m_selectionProvider = std::move(cb);
}

void AocViewer::Clear() {
    m_loadState = LoadState::Idle;
    m_state.m_filePath.clear();
    m_state.m_rawBytes.clear();
    m_state.m_rawBytes.shrink_to_fit();
    m_state.m_sha256Hex.clear();
    m_state.m_parsedDoc = {};
    m_state.m_resolveCache.clear();
    m_state.m_astSkel = {};
    m_state.m_resolvedSkel = {};
    m_state.m_aocBindInverseWorld.clear();
    m_state.m_boneRemap.clear();
    m_state.m_staticLocalBbox = {};
    m_state.m_parseWarnings.clear();
    m_state.m_parseErrors.clear();
    m_state.m_sceneGraph.Clear();
    m_state.m_animPlayer.Reset();
    m_state.m_paletteScratch.clear();
    m_state.m_lastSaveStatus.clear();
    m_error.clear();
    m_loaded = false;
    m_activeTab = AocTab::Preview;

    // Phase 2 — reset light system + ephemeral audio bookkeeping.
    // m_audio itself is preserved (reconfigured in DoParsing) so it survives
    // across file loads without re-initialising the audio device every time.
    // m_activeLightStateName is NOT reset here: it persists so the user's
    // dropdown selection survives Raw-Text save/re-parse cycles.
    m_state.m_lights.Clear();
    m_state.m_audioLastTriggered.clear();
    m_state.m_uiToasts.clear();
    m_state.m_lastAnimTimeForAudio = 0.0f;
    m_state.m_particles.Clear();
    m_state.m_particlesLastClip.clear();

    // Phase 4 — reset transitions + override + detach all socket attachments.
    // `DetachAllChildren` is called explicitly here (cross-file load); note that
    // `AocSceneGraph::Clear()` above (line 122) does NOT touch m_children — that
    // is intentional so Raw Text re-parse preserves children.
    m_state.m_transitions.Clear();
    m_state.m_persisted.Clear();  // Phase 5.1 — in-memory only; JSON on disk preserved.
    m_state.m_transitionOverrideOt.clear();
    m_state.m_transitionStateDirty = false;
    m_state.m_sceneGraph.DetachAllChildren();
    m_childMaterials.clear();
    m_mainBuffersDirtyFromChildRender = false;

    // Drop stale GPU buffers so the next LoadData re-uploads; avoids the
    // MeshRenderer trying to draw a freed mesh between Clear() and the
    // next frame's lazy upload.
    if (m_renderer) m_renderer->ClearMeshBuffers();
    m_uploadedMeshAddr = nullptr;
    m_uploadFailed = false;
}

void AocViewer::LoadData(std::vector<uint8_t> data, const std::string& path) {
    Clear();

    if (m_playbackSpeed) {
        m_state.m_animPlayer.SetSpeed(*m_playbackSpeed);
    }
    m_state.m_rawBytes = std::move(data);
    m_state.m_filePath = path;

    // SHA256 for Overview + Diagnostic tabs. Computed once here so the worker
    // thread doesn't repeat it every frame; returns empty on BCrypt failure
    // (harmless -- Overview tab simply omits the SHA256 line).
    m_state.m_sha256Hex = ComputeSha256Hex(m_state.m_rawBytes);

    m_loadState = LoadState::Preparing;
}

// --------------------------------------------------------------------------
// State machine
// --------------------------------------------------------------------------

void AocViewer::TickLoadState() {
    switch (m_loadState) {
    case LoadState::Preparing:
        m_loadState = LoadState::ReadingChain;
        break;
    case LoadState::ReadingChain:
        DoReadingChain();
        break;
    case LoadState::Parsing:
        DoParsing();
        break;
    default:
        break;
    }
}

void AocViewer::DoReadingChain() {
    // Tokenize the raw .aoc bytes. The resolver (in DoParsing) walks the extends
    // chain via GGPKManager to merge parent .aoc files.
    auto tok = Parsers::TokenizeAoc(m_state.m_rawBytes.data(),
                                     m_state.m_rawBytes.size());
    if (!tok) {
        m_error = tok.error;
        m_state.m_parseErrors.push_back(tok.error);
        m_loadState = LoadState::Error;
        return;
    }

    auto leaf = Parsers::ParseAoc(tok.value);
    if (!leaf) {
        m_error = leaf.error;
        m_state.m_parseErrors.push_back(leaf.error);
        m_loadState = LoadState::Error;
        return;
    }
    leaf.value.sourcePath = m_state.m_filePath;

    // Stash the leaf AocFile warnings, then resolve in the next tick.
    for (const auto& w : leaf.value.warnings) {
        m_state.m_parseWarnings.push_back(w);
    }

    // Temporarily stash the leaf into merged so Parsing tick can pick it up.
    // (The subsequent ResolveExtends call will overwrite m_parsedDoc.)
    m_state.m_parsedDoc.merged = std::move(leaf.value);
    m_loadState = LoadState::Parsing;
}

void AocViewer::DoParsing() {
    // Pull the leaf AocFile out of the staged document — ResolveExtends will
    // produce a fully merged AocDocument.
    Parsers::AocFile leaf = std::move(m_state.m_parsedDoc.merged);
    m_state.m_parsedDoc = {};

    auto docResult = Parsers::ResolveExtends(leaf, m_state.m_mgr,
                                              &m_state.m_resolveCache);
    if (!docResult) {
        m_error = docResult.error;
        m_state.m_parseErrors.push_back(docResult.error);
        m_loadState = LoadState::Error;
        return;
    }
    m_state.m_parsedDoc = std::move(docResult.value);

    for (const auto& w : m_state.m_parsedDoc.warnings) {
        m_state.m_parseWarnings.push_back(w);
    }
    for (const auto& w : m_state.m_parsedDoc.merged.warnings) {
        m_state.m_parseWarnings.push_back(w);
    }

    // Find the skeleton path in the merged ClientAnimationController block.
    std::string skeletonPath;
    for (const auto& b : m_state.m_parsedDoc.merged.blocks) {
        if (b.clientAnim && !b.clientAnim->skeletonPath.empty()) {
            skeletonPath = b.clientAnim->skeletonPath;
            break;
        }
    }

    // Find the skin mesh path in the merged SkinMesh block.
    std::string skinPath;
    for (const auto& b : m_state.m_parsedDoc.merged.blocks) {
        if (b.skinMesh && !b.skinMesh->skinPath.empty()) {
            skinPath = b.skinMesh->skinPath;
            break;
        }
    }

    // Build skeleton from ClientAnimationController.skeletonPath reference.
    if (!skeletonPath.empty() && m_state.m_mgr) {
        void* node = m_state.m_mgr->FindNodeByPath(skeletonPath);
        if (node) {
            auto astBytes = m_state.m_mgr->ReadFile(node);
            if (astBytes && !astBytes.value.empty()) {
                auto astParse = Parsers::ParseAst(astBytes.value.data(),
                                                    astBytes.value.size());
                if (astParse) {
                    m_state.m_astSkel = std::move(astParse.value);
                    auto resolved = Parsers::BuildResolvedSkeleton(m_state.m_astSkel);
                    if (resolved) {
                        m_state.m_resolvedSkel = std::move(resolved.value);
                        std::vector<std::string> augWarns;
                        Parsers::AugmentSkeleton(m_state.m_parsedDoc,
                                                  m_state.m_resolvedSkel,
                                                  &augWarns);
                        for (const auto& w : augWarns) {
                            m_state.m_parseWarnings.push_back(w);
                        }

                        // Seed staticLocalBbox from bone world transforms so
                        // ComputeAnimatedBounds() has a non-degenerate static
                        // reference (used by camera follow + grid anchoring).
                        m_state.m_staticLocalBbox = {};
                        for (const auto& bb : m_state.m_resolvedSkel.bones) {
                            const float x = bb.worldTransform[12];
                            const float y = bb.worldTransform[13];
                            const float z = bb.worldTransform[14];
                            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
                                continue;
                            }
                            m_state.m_staticLocalBbox.Include(x, y, z);
                        }
                    } else {
                        m_state.m_parseWarnings.push_back(
                            "Skeleton resolve failed: " + resolved.error);
                    }
                } else {
                    m_state.m_parseWarnings.push_back(
                        "Skeleton parse failed: " + astParse.error);
                }
            } else {
                m_state.m_parseWarnings.push_back(
                    "Skeleton read failed: " + skeletonPath);
            }
        } else {
            m_state.m_parseWarnings.push_back(
                "Skeleton not found in GGPK: " + skeletonPath);
        }
    }

    // Load main skin mesh (Phase 1). AocSceneGraph internally resolves
    // .sm/.smd/.ast via GGPKManager.
    if (!skinPath.empty() && m_state.m_mgr) {
        m_state.m_sceneGraph.LoadMain(m_state.m_mgr, skinPath);
    }

    // ----------------------------------------------------------------------
    // Phase 2 — construct subsystems from parsed blocks
    // ----------------------------------------------------------------------
    //
    // Walk the merged blocks looking for typed Lights / SoundEvents views
    // populated by AocResolver::RebuildTypedViews. Each subsystem is seeded
    // with a deep copy of the parsed block so later Raw-Text edits + re-parse
    // replace the data cleanly. m_activeLightStateName persists across
    // reparses so a user's dropdown selection survives a save; we reassert
    // it into the light system here.
    {
        auto& s = m_state;
        const Parsers::LightsBlock*          lightsBlock    = nullptr;
        const Parsers::SoundEventsBlock*     soundBlock     = nullptr;
        const Parsers::ParticleEffectsBlock* particleBlock  = nullptr;   // NEW Phase 3
        for (const auto& b : s.m_parsedDoc.merged.blocks) {
            if (!lightsBlock    && b.lights)          lightsBlock    = b.lights.get();
            if (!soundBlock     && b.soundEvents)     soundBlock     = b.soundEvents.get();
            if (!particleBlock  && b.particleEffects) particleBlock  = b.particleEffects.get();  // NEW
            if (lightsBlock && soundBlock && particleBlock) break;
        }

        if (lightsBlock) {
            s.m_lights.SetLights(*lightsBlock);
            s.m_lights.SetActiveStateName(s.m_activeLightStateName);
        } else {
            s.m_lights.Clear();
        }
        (void)soundBlock; // SoundEvents block is consumed by m_audio via its
                          // own direct access to s.m_parsedDoc in Task 12.

        if (s.m_audio) {
            s.m_audio->Configure(s.m_mgr, ExtractDirectoryPath(s.m_filePath));
            s.m_audio->SetOnMissing([this](const std::string& path) {
                m_state.m_uiToasts.push_back({
                    "SoundEvent MISSING: " + path,
                    static_cast<float>(ImGui::GetTime())
                });
            });
        }

        // Find the ClientAnimationController block — needed for bone-group
        // resolution (bone_group membership lives there, not on the skeleton).
        const Parsers::ClientAnimCtrl* cacBlock = nullptr;
        for (const auto& b : s.m_parsedDoc.merged.blocks) {
            if (b.clientAnim) { cacBlock = b.clientAnim.get(); break; }
        }

        if (particleBlock) {
            s.m_particles.Populate(*particleBlock, s.m_resolvedSkel, cacBlock, s.m_mgr);
        } else {
            s.m_particles.Clear();
        }
    }

    // Phase 4 — populate Transitionable from sibling .ot / .otc.
    // Uses m_transitionOverrideOt if set (persists across Raw Text re-parse);
    // otherwise filename convention.
    m_state.m_transitions.Populate(m_state.m_mgr, m_state.m_filePath,
                                    m_state.m_transitionOverrideOt);

    // Phase 5.1 — Restore persistent attachments + state.
    Aoc::AocPersistedAttachments::LoadResult persistRes;
    m_state.m_persisted.Populate(m_state.m_pluginDir,
                                 m_state.m_filePath,
                                 m_state.m_mgr,
                                 m_state.m_sceneGraph,
                                 m_state.m_parsedDoc,
                                 m_state.m_transitions,
                                 persistRes);

    // Phase 5.1 — sync UI field with restored override state so Source/Clear Override UI reflects persistence.
    m_state.m_transitionOverrideOt = m_state.m_transitions.OverrideOtPath();

    // Toasts — green for success (isError=false), red for skipped (isError=true).
    const float nowT = static_cast<float>(ImGui::GetTime());
    if (persistRes.restoredAttachments > 0 && persistRes.transitionStateRestored) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Restored %d attachments + state %d",
                      persistRes.restoredAttachments, m_state.m_transitions.CurrentState());
        m_state.m_uiToasts.push_back({buf, nowT, false});
    } else if (persistRes.restoredAttachments > 0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Restored %d attachments", persistRes.restoredAttachments);
        m_state.m_uiToasts.push_back({buf, nowT, false});
    } else if (persistRes.transitionStateRestored) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Restored state %d", m_state.m_transitions.CurrentState());
        m_state.m_uiToasts.push_back({buf, nowT, false});
    }
    if (persistRes.skippedAttachments > 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Persistence: %d attachments skipped (missing source/socket)",
                      persistRes.skippedAttachments);
        m_state.m_uiToasts.push_back({buf, nowT, true});
    }
    // T7 (Bug B5) — surface override-.ot parse failures. The override path is
    // preserved in memory (m_transitionOverrideOt synced above) and in JSON on
    // the next Save(); the toast tells the user why the Transitionable UI may
    // be dimmer than expected without silently dropping their selection.
    if (persistRes.overrideOtFailed) {
        for (const auto& w : persistRes.overrideOtWarnings) {
            Aoc::Ui::PushToast(m_state, w, /*isError=*/true);
        }
    }

    // State changed as side-effect of restore -> re-resolve clip next frame.
    if (persistRes.transitionStateRestored) m_state.m_transitionStateDirty = true;

    if (m_state.m_transitions.IsAvailable()) {
        const auto& names = m_state.m_animPlayer.AnimNames();
        const std::string clip = m_state.m_transitions.ResolveClipForState(
                m_state.m_transitions.CurrentState(), names);
        if (!clip.empty()) {
            // AnimationPlayer has no SetClipByName(); look up index in AnimNames()
            // and call SelectAnimation(idx). If not found, leave the player on
            // its default (anim 0) so the timeline keeps working.
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (names[i] == clip) {
                    m_state.m_animPlayer.SelectAnimation(i);
                    break;
                }
            }
        }
    }

    // Bind-pose inverse (for mesh skinning palette). Must be computed BEFORE any
    // ApplyFrame mutates m_resolvedSkel.bones[i].worldTransform. At this point
    // BuildResolvedSkeleton has finalized the bind pose and AttachSkeleton has
    // not yet run, so bone world transforms still reflect the bind pose.
    m_state.m_aocBindInverseWorld.clear();
    m_state.m_aocBindInverseWorld.reserve(m_state.m_resolvedSkel.bones.size());
    for (const auto& bone : m_state.m_resolvedSkel.bones) {
        std::array<float, 16> inv{};
        if (!Animation::MatrixMath::Inverse4x4(bone.worldTransform, inv.data())) {
            // Singular — identity fallback (shouldn't happen for valid bind poses)
            inv = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        }
        m_state.m_aocBindInverseWorld.push_back(inv);
    }

    // V01/V02 local→global bone-index remap (mirrors MeshViewer.cpp:527-545).
    // V01/V02 meshes encode BLENDINDICES as local indices into mesh.boneNames;
    // we must remap to global skeleton indices so the palette shuffle (later
    // in RenderPreviewTab) can put per-local-slot matrices where the shader
    // expects them. DOLm/V3+ meshes leave this empty and skip the shuffle.
    m_state.m_boneRemap.clear();
    if (m_state.m_sceneGraph.HasMesh() &&
        !m_state.m_sceneGraph.Mesh().boneNames.empty() &&
        !m_state.m_resolvedSkel.bones.empty()) {
        const auto& localNames = m_state.m_sceneGraph.Mesh().boneNames;
        m_state.m_boneRemap.reserve(localNames.size());
        for (const auto& localName : localNames) {
            int match = -1;
            for (size_t gi = 0; gi < m_state.m_resolvedSkel.bones.size(); ++gi) {
                if (m_state.m_resolvedSkel.bones[gi].name == localName) {
                    match = static_cast<int>(gi);
                    break;
                }
            }
            m_state.m_boneRemap.push_back(match);
        }
    }

    // Pre-size the palette scratch so BuildSkinningPalette has room to write
    // (it caps at min(outPalette.size(), bindInv.size(), kMaxSkinBones); empty
    // scratch silently produces zero output). Mirrors MeshViewer::InitResolvedSkeleton
    // which resizes m_skinningMatrices once per load.
    m_state.m_paletteScratch.resize(m_state.m_resolvedSkel.bones.size());

    // Attach animation player to the AST skeleton (auto-plays anim 0 if present).
    m_state.m_animPlayer.AttachSkeleton(&m_state.m_astSkel);

    // Fit camera to mesh bbox so the first frame shows a useful view instead
    // of the default pose pointing at world origin. RebuildScene() saves and
    // restores m_camera around DoParsing, so a user's manual pose survives
    // a reparse triggered by Raw Text save.
    if (m_state.m_sceneGraph.HasMesh()) {
        float mn[3], mx[3];
        m_state.m_sceneGraph.ComputeMeshLocalBounds(mn, mx);
        m_state.m_camera.FitToBounds(mn, mx);
    } else if (!m_state.m_resolvedSkel.bones.empty()) {
        // No mesh loaded — fall back to skeleton world-transform bbox.
        float mn[3] = { 1e30f, 1e30f, 1e30f };
        float mx[3] = { -1e30f, -1e30f, -1e30f };
        bool any = false;
        for (const auto& b : m_state.m_resolvedSkel.bones) {
            const float x = b.worldTransform[12];
            const float y = b.worldTransform[13];
            const float z = b.worldTransform[14];
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
            mn[0] = std::min(mn[0], x); mx[0] = std::max(mx[0], x);
            mn[1] = std::min(mn[1], y); mx[1] = std::max(mx[1], y);
            mn[2] = std::min(mn[2], z); mx[2] = std::max(mx[2], z);
            any = true;
        }
        if (any) {
            m_state.m_camera.FitToBounds(mn, mx);
        }
    }

    m_loaded = true;
    m_loadState = LoadState::Ready;
}

void AocViewer::RebuildScene() {
    // Called when Raw Text is saved and re-parsed.
    // Re-run DoReadingChain + DoParsing without resetting user toggles/camera.
    auto savedView = m_state.m_view;
    auto savedCamera = m_state.m_camera;

    m_state.m_parsedDoc = {};
    m_state.m_resolveCache.clear();
    m_state.m_astSkel = {};
    m_state.m_resolvedSkel = {};
    m_state.m_aocBindInverseWorld.clear();
    m_state.m_boneRemap.clear();
    m_state.m_sceneGraph.Clear();
    m_state.m_parseWarnings.clear();
    m_state.m_parseErrors.clear();

    // Re-parse builds a fresh mesh pointer — drop stale GPU buffers so the
    // next frame's upload path runs CreateMeshBuffers/LoadAllMaterials
    // against the new mesh instead of silently reusing the old VB/IB.
    if (m_renderer) m_renderer->ClearMeshBuffers();
    m_uploadedMeshAddr = nullptr;
    m_uploadFailed = false;
    m_state.m_paletteScratch.clear();

    DoReadingChain();
    if (m_loadState == LoadState::Parsing) {
        DoParsing();
    }

    m_state.m_view = savedView;
    m_state.m_camera = savedCamera;

    // Phase 4 M9 — force clip re-resolution after reparse. Populate() above may
    // have clamped the current state index, and the .amd clip list may have
    // changed if the user edited the CAC block. The next frame's ResolveClip
    // pass consumes this flag.
    m_state.m_transitionStateDirty = true;
}

float AocViewer::ComputeGridZ() const {
    // POE2 meshes use a Z-down convention (see mesh/MeshCamera.cpp).
    // The grid sits on the floor plane, which is bboxMax Z (largest Z),
    // matching MeshViewer's `gridZ = m_mesh.bboxMax[2]` convention.
    switch (static_cast<AocGridAnchor>(m_state.m_view.gridAnchor)) {
        case AocGridAnchor::MeshBounds: {
            float mn[3], mx[3];
            m_state.m_sceneGraph.ComputeMeshLocalBounds(mn, mx);
            return mx[2];
        }
        case AocGridAnchor::Skeleton: {
            // Skeleton bone bbox (legacy behavior); floor = max Z.
            float mn[3] = { 1e30f, 1e30f, 1e30f };
            float mx[3] = { -1e30f, -1e30f, -1e30f };
            bool any = false;
            for (const auto& b : m_state.m_resolvedSkel.bones) {
                const float x = b.worldTransform[12];
                const float y = b.worldTransform[13];
                const float z = b.worldTransform[14];
                if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
                if (x < mn[0]) mn[0] = x; if (x > mx[0]) mx[0] = x;
                if (y < mn[1]) mn[1] = y; if (y > mx[1]) mx[1] = y;
                if (z < mn[2]) mn[2] = z; if (z > mx[2]) mx[2] = z;
                any = true;
            }
            return any ? mx[2] : 0.0f;
        }
        case AocGridAnchor::WorldOrigin:
        default:
            return 0.0f;
    }
}

// --------------------------------------------------------------------------
// Render dispatch
// --------------------------------------------------------------------------

void AocViewer::Render() {
    TickLoadState();

    if (m_loadState == LoadState::Error) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: %s", m_error.c_str());
        return;
    }
    if (m_loadState != LoadState::Ready) {
        ImGui::TextDisabled("Loading %s...", m_state.m_filePath.c_str());
        return;
    }

    if (ImGui::BeginTabBar("##AocTabs", ImGuiTabBarFlags_Reorderable)) {
        if (ImGui::BeginTabItem("Preview")) {
            m_activeTab = AocTab::Preview;
            RenderPreviewTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Overview")) {
            m_activeTab = AocTab::Overview;
            RenderOverviewTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sockets")) {
            m_activeTab = AocTab::Sockets;
            RenderSocketsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Animations")) {
            m_activeTab = AocTab::Animations;
            RenderAnimationsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Diagnostic")) {
            m_activeTab = AocTab::Diagnostic;
            RenderDiagnosticTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // Phase 5.1.5a — Browse-and-Attach modal. No-op when closed.
    if (m_browseModal) {
        m_browseModal->SetManager(m_state.m_mgr);
        m_browseModal->SetTree(m_state.m_tree);
        m_browseModal->Render();
    }
}

// --------------------------------------------------------------------------
// Tab stubs -- implementations in split .cpp files (Tasks 5-10)
// --------------------------------------------------------------------------

// RenderPreviewTab     -> AocViewer_3DPreview.cpp
// RenderOverviewTab    -> AocViewer_Info.cpp
// RenderSocketsTab     -> AocViewer_Sockets.cpp
// RenderAnimationsTab  -> AocViewer_Animations.cpp
// RenderDiagnosticTab  -> AocViewer_Diagnostic.cpp

} // namespace GGPKExp
