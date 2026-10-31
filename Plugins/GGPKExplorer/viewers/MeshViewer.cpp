#include "MeshViewer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../core/GGPKManager.h"
#include "../core/parsers/AocTokenizer.h"
#include "../core/parsers/AocParser.h"
#include "../core/parsers/AocResolver.h"
#include "animation/MatrixMath.h"
#include "animation/SkinningPalette.h"
#include "animation/AnimationSidePanel.h"
#include "MeshShaders.h"  // for kMaxSkinBones
#include "../core/GGPKExplorerSettings.h"

namespace GGPKExp {

namespace {
// Fallback used when the plugin hasn't wired a shared MeshViewSettings yet.
// Shared across MeshViewer instances (same process) — still survives model
// switches, but not plugin restart. Task 7 wires the real persistent state.
GGPKExp::MeshViewSettings& FallbackViewSettings() {
    static GGPKExp::MeshViewSettings s_fallback;
    return s_fallback;
}
} // anonymous namespace

// -----------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------

void MeshViewer::Initialize(ID3D11Device* device) {
    m_renderer.Initialize(device);
    m_material.Initialize(device);
    m_material.CreateFallbackTexture();
}

void MeshViewer::Clear() {
    m_mesh = {};
    m_renderer.ClearMeshBuffers();
    m_skeleton = {};
    m_resolvedSkeleton = {};
    m_staticLocalBbox = {};
    m_smData = {};
    m_isSmFile = false;
    m_path.clear();
    m_filename.clear();
    m_isAoc = false;
    m_aocNoMesh = false;
    m_aocSkinPath.clear();
    m_aocSkeletonPath.clear();
    m_aocWarnings.clear();
    m_isStaticPropMesh = false;
    m_rigSourceName.clear();
    m_skeletonError.clear();
    m_boneViz.clear();

    // Skinning caches are keyed to the previously loaded skeleton. If not
    // cleared here, a subsequent load of an unskinned mesh (or one whose
    // skeleton fails to load) will leave m_skinningMatrices populated with
    // the old palette; the render loop's `if (!m_skinningMatrices.empty())`
    // guard then feeds a stale palette into the shader, warping the new mesh.
    m_skinningMatrices.clear();
    m_bindInverseWorld.clear();
    m_skinningTruncationWarned = false;
    m_boneRemap.clear();

    // Phase 4.2: reset animation playback state.
    m_animPlayer.Reset();

    m_loaded = false;
    m_error.clear();

    // Reset display state
    m_camera.Reset();
    m_renderMode = 0;
    // NOTE: view toggles (Grid/Bones) live in m_viewSettings and survive
    // Clear() on purpose — user setting is remembered across model switches
    // and, once wired by the plugin, across sessions.

    m_material.Clear();
    m_material.CreateFallbackTexture();

    // Reset async loading state
    m_loadState = MeshLoadState::Idle;
    m_prepareFrameDrawn = false;
    m_pending = {};
    m_keptView = {};   // LoadData re-sets it after Clear() for a reload
    m_smdBytes.clear();
    m_rigAstBytes.clear();
}

// -----------------------------------------------------------------------
// IViewer::LoadData — starts the async state machine
// -----------------------------------------------------------------------

void MeshViewer::LoadData(std::vector<uint8_t> data, const std::string& path) {
    // Loading the open file again (its .aoc saved in the Text tab) keeps the
    // view the user set up: camera, render mode and clip by name. While no
    // model is shown (the reload is not drawn yet, or an edit broke it) the
    // view kept last time carries on to the next reload. Another file
    // (ContentPanel clears the viewer on a file switch) starts fitted.
    KeptView kept;
    if (!path.empty() && path == m_path) {
        if (m_loadState == MeshLoadState::Ready && m_renderer.GetVertexCount() > 0) {
            kept.valid = true;
            kept.camera = m_camera;
            kept.renderMode = m_renderMode;
            const std::size_t clip = m_animPlayer.SelectedAnimIndex();
            if (clip < m_animPlayer.AnimNames().size()) {
                kept.clip = m_animPlayer.AnimNames()[clip];
                kept.time = m_animPlayer.CurrentTime();
                kept.playing = m_animPlayer.IsPlaying();
            }
        } else {
            kept = std::move(m_keptView);
        }
    }
    Clear();
    m_keptView = std::move(kept);
    // Re-apply the persisted playback speed. Clear() -> Reset() already
    // preserves m_defaultSpeed inside the player, but the very first model
    // load after plugin start needs this to propagate the JSON-loaded value.
    if (m_playbackSpeed) {
        m_animPlayer.SetSpeed(*m_playbackSpeed);
    }
    m_pending.inputData = std::move(data);
    m_pending.path = path;
    m_path = path;
    const auto slash = path.find_last_of("/\\");
    m_filename = (slash != std::string::npos) ? path.substr(slash + 1) : path;

    std::string ext;
    auto dot = path.rfind('.');
    if (dot != std::string::npos) ext = path.substr(dot);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    m_pending.ext = ext;

    m_prepareFrameDrawn = false;
    m_loadState = MeshLoadState::Preparing;
}

// -----------------------------------------------------------------------
// ResolveAocModel — the .aoc's main skin mesh and skeleton. Reads the .sm into
// m_pending (as if it had been opened) and records the skeleton path.
// -----------------------------------------------------------------------

bool MeshViewer::ResolveAocModel() {
    const auto& bytes = m_pending.inputData;
    auto tok = Parsers::TokenizeAoc(bytes.data(), bytes.size());
    if (!tok) {
        m_error = "AOC: " + tok.error;
        return false;
    }
    auto leaf = Parsers::ParseAoc(tok.value);
    if (!leaf) {
        m_error = "AOC: " + leaf.error;
        return false;
    }
    leaf.value.sourcePath = m_pending.path;
    auto doc = Parsers::ResolveExtends(leaf.value, m_mgr);
    if (!doc) {
        m_error = "AOC: " + doc.error;
        return false;
    }

    // A parent that is missing or does not parse ends the chain: what it
    // would have named (the skin, the skeleton) is then absent. Kept for the
    // view and the Info text.
    m_aocWarnings = std::move(doc.value.warnings);

    const Parsers::AocModelRefs refs = Parsers::FindModelRefs(doc.value.merged);
    m_aocSkeletonPath = refs.skeletonPath;
    if (refs.skinPath.empty()) {
        m_aocNoMesh = true;
        return true;
    }

    // The skin may be named with or without its extension: always <base>.sm.
    std::string base = refs.skinPath;
    const size_t dot = base.find_last_of('.');
    const size_t slash = base.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) base.resize(dot);
    m_aocSkinPath = base + ".sm";

    void* node = m_mgr->FindNodeByPath(m_aocSkinPath);
    if (!node) {
        FailMesh("Missing .sm at: " + m_aocSkinPath);
        return false;
    }
    auto r = m_mgr->ReadFile(node);
    if (!r || r.value.empty()) {
        FailMesh("Failed to read .sm: " + m_aocSkinPath + (r.error.empty() ? "" : " -- " + r.error));
        return false;
    }
    m_pending.inputData = std::move(r.value);
    m_pending.path = m_aocSkinPath;
    m_pending.ext = ".sm";
    return true;
}

// -----------------------------------------------------------------------
// FailMesh — the model cannot be shown: one error line names the file and the
// cause instead of an empty canvas. For an .aoc it says that the skin mesh the
// .aoc names is what is missing.
// -----------------------------------------------------------------------

void MeshViewer::FailMesh(const std::string& why) {
    m_error = m_isAoc ? "No renderable skin mesh: " + why : why;
    m_loadState = MeshLoadState::Error;
}

// -----------------------------------------------------------------------
// DoMainThreadRead — runs in the ReadingFiles phase. Reads .smd and the
// skeleton (.ast) from GGPK, then transitions to UploadingGpu.
// -----------------------------------------------------------------------

void MeshViewer::DoMainThreadRead() {
    if (m_pending.inputData.empty() || !m_mgr) {
        m_error = "No data to load";
        m_loadState = MeshLoadState::Error;
        return;
    }

    // .aoc: swap the pending input for the .sm it names, then load that .sm
    // (with the .aoc's own skeleton instead of the .sm's companion rig).
    if (m_pending.ext == ".aoc") {
        m_isAoc = true;
        if (!ResolveAocModel()) {
            m_loadState = MeshLoadState::Error;
            return;
        }
        if (m_aocNoMesh) {
            m_loaded = true;
            m_loadState = MeshLoadState::Ready;
            return;
        }
    }

    const std::string& ext = m_pending.ext;
    const auto& data = m_pending.inputData;

    // .ast is skeleton-only — parse synchronously and finish
    if (ext == ".ast") {
        auto astResult = Parsers::ParseAst(data.data(), data.size());
        if (!astResult) {
            m_error = astResult.error.empty() ? "Failed to parse skeleton" : astResult.error;
            m_loadState = MeshLoadState::Error;
            return;
        }
        m_skeleton = std::move(astResult.value);

        // Derive bone viz + camera bounds from transform translations so standalone
        // .ast effect files still render their skeleton in the 3D view.
        InitResolvedSkeleton();

        // Attach the player: it selects the default clip without playing it.
        m_animPlayer.AttachSkeleton(&m_skeleton);

        constexpr float kMaxCoordBone = 1e6f;
        bool bboxInitialized = false;
        for (const auto& bv : m_boneViz) {
            if (!std::isfinite(bv.worldPos[0]) || !std::isfinite(bv.worldPos[1]) || !std::isfinite(bv.worldPos[2]))
                continue;
            if (std::fabs(bv.worldPos[0]) > kMaxCoordBone ||
                std::fabs(bv.worldPos[1]) > kMaxCoordBone ||
                std::fabs(bv.worldPos[2]) > kMaxCoordBone) continue;
            if (!bboxInitialized) {
                m_mesh.bboxMin[0] = m_mesh.bboxMax[0] = bv.worldPos[0];
                m_mesh.bboxMin[1] = m_mesh.bboxMax[1] = bv.worldPos[1];
                m_mesh.bboxMin[2] = m_mesh.bboxMax[2] = bv.worldPos[2];
                bboxInitialized = true;
                continue;
            }
            if (bv.worldPos[0] < m_mesh.bboxMin[0]) m_mesh.bboxMin[0] = bv.worldPos[0];
            if (bv.worldPos[0] > m_mesh.bboxMax[0]) m_mesh.bboxMax[0] = bv.worldPos[0];
            if (bv.worldPos[1] < m_mesh.bboxMin[1]) m_mesh.bboxMin[1] = bv.worldPos[1];
            if (bv.worldPos[1] > m_mesh.bboxMax[1]) m_mesh.bboxMax[1] = bv.worldPos[1];
            if (bv.worldPos[2] < m_mesh.bboxMin[2]) m_mesh.bboxMin[2] = bv.worldPos[2];
            if (bv.worldPos[2] > m_mesh.bboxMax[2]) m_mesh.bboxMax[2] = bv.worldPos[2];
        }
        if (!bboxInitialized) {
            // Fallback: unit cube centered at origin
            m_mesh.bboxMin[0] = m_mesh.bboxMin[1] = m_mesh.bboxMin[2] = -1.0f;
            m_mesh.bboxMax[0] = m_mesh.bboxMax[1] = m_mesh.bboxMax[2] =  1.0f;
        } else {
            // Pad any zero-size axis so the camera doesn't degenerate
            for (int i = 0; i < 3; ++i) {
                if (m_mesh.bboxMax[i] - m_mesh.bboxMin[i] < 0.01f) {
                    m_mesh.bboxMin[i] -= 0.5f;
                    m_mesh.bboxMax[i] += 0.5f;
                }
            }
        }
        m_camera.FitToBounds(m_mesh.bboxMin, m_mesh.bboxMax);
        m_staticLocalBbox = {};
        m_staticLocalBbox.Include(m_mesh.bboxMin[0], m_mesh.bboxMin[1], m_mesh.bboxMin[2]);
        m_staticLocalBbox.Include(m_mesh.bboxMax[0], m_mesh.bboxMax[1], m_mesh.bboxMax[2]);

        m_loaded = true;
        m_loadState = MeshLoadState::Ready;
        return;
    }

    m_isSmFile = (ext == ".sm");

    if (m_isSmFile) {
        // The .sm names its mesh data (.smd); without it there is nothing to show.
        auto smResult = Parsers::ParseSm(data.data(), data.size());
        if (!smResult) {
            FailMesh("Failed to parse .sm: " + m_pending.path +
                     (smResult.error.empty() ? "" : " -- " + smResult.error));
            return;
        }
        m_smData = std::move(smResult.value);

        void* smdNode = m_mgr->FindNodeByPath(m_smData.smdPath);
        if (!smdNode) {
            FailMesh("Missing .smd at: " + m_smData.smdPath);
            return;
        }
        {
            auto r = m_mgr->ReadFile(smdNode);
            if (!r || r.value.empty()) {
                FailMesh("Failed to read .smd: " + m_smData.smdPath +
                         (r.error.empty() ? "" : " -- " + r.error));
                return;
            }
            m_smdBytes = std::move(r.value);
        }

        // Per-mesh rig discovery (2026-04-15):
        //   1. Try <sm-basename>.ast in the .smd's directory — static props,
        //      chests, attachments, effects have their OWN local skeleton.
        //   2. Fall back to rig.ast — animated characters and monster bodies
        //      share the directory's rig.
        // The .sm basename (e.g. "bittersoncorpse01") differs from the .smd
        // basename (which is hashed, e.g. "BitterSon01_StaticCorpse_cb7cfce7"),
        // so we derive it from m_pending.path, not m_smData.smdPath.
        // Signal verified on 27,566 .sm files: 13.6% have per-mesh .ast,
        // 99.4% of those are single-clip static/trigger meshes — never
        // /characters/.
        auto lastSlash = m_smData.smdPath.rfind('/');
        if (m_isAoc) {
            // The .aoc's ClientAnimationController names the skeleton
            // (none: the mesh shows in bind pose).
            if (!m_aocSkeletonPath.empty()) {
                if (void* astNode = m_mgr->FindNodeByPath(m_aocSkeletonPath)) {
                    auto r = m_mgr->ReadFile(astNode);
                    if (r) {
                        m_rigAstBytes = std::move(r.value);
                        const auto s = m_aocSkeletonPath.find_last_of("/\\");
                        m_rigSourceName = (s != std::string::npos)
                            ? m_aocSkeletonPath.substr(s + 1) : m_aocSkeletonPath;
                    } else {
                        m_skeletonError = "read failed" + (r.error.empty() ? "" : ": " + r.error);
                    }
                }
            }
        } else if (lastSlash != std::string::npos) {
            const std::string dir = m_smData.smdPath.substr(0, lastSlash + 1);

            std::string smBase;
            {
                const auto smSlash = m_pending.path.rfind('/');
                std::string smName = (smSlash != std::string::npos)
                                     ? m_pending.path.substr(smSlash + 1)
                                     : m_pending.path;
                const auto smDot = smName.rfind('.');
                smBase = (smDot != std::string::npos) ? smName.substr(0, smDot) : smName;
            }

            bool loadedLocal = false;
            if (!smBase.empty()) {
                const std::string localAstPath = dir + smBase + ".ast";
                if (void* astNode = m_mgr->FindNodeByPath(localAstPath)) {
                    auto r = m_mgr->ReadFile(astNode);
                    if (r) {
                        m_rigAstBytes = std::move(r.value);
                        m_isStaticPropMesh = true;
                        m_rigSourceName = smBase + ".ast";
                        loadedLocal = true;
                    }
                }
            }

            if (!loadedLocal) {
                const std::string astPath = dir + "rig.ast";
                if (void* astNode = m_mgr->FindNodeByPath(astPath)) {
                    auto r = m_mgr->ReadFile(astNode);
                    if (r) {
                        m_rigAstBytes = std::move(r.value);
                        m_rigSourceName = "rig.ast";
                    }
                }
            }
        }
    } else if (ext == ".smd") {
        // Raw .smd file: input data IS the smd bytes
        m_smdBytes = data;
    } else {
        m_error = "Unsupported extension: " + ext;
        m_loadState = MeshLoadState::Error;
        return;
    }

    m_loadState = MeshLoadState::UploadingGpu;
}

// -----------------------------------------------------------------------
// DoMainThreadUpload — runs in the UploadingGpu phase. Parses skeleton and
// mesh bytes, loads textures, creates GPU buffers, then transitions to Ready.
// -----------------------------------------------------------------------

void MeshViewer::DoMainThreadUpload() {
    // Parse skeleton if we have rig bytes
    if (!m_rigAstBytes.empty()) {
        auto r = Parsers::ParseAst(m_rigAstBytes.data(), m_rigAstBytes.size());
        if (r) m_skeleton = std::move(r.value);
        else m_skeletonError = r.error.empty() ? std::string("does not parse") : r.error;
    }

    // Parse mesh bytes FIRST so we know submesh count before loading materials
    // (LoadAllMaterials expands run-length counts based on submesh count).
    const std::string& smdPath = m_isSmFile ? m_smData.smdPath : m_pending.path;
    if (!m_smdBytes.empty()) {
        auto r = Parsers::ParseSmd(m_smdBytes.data(), m_smdBytes.size());
        if (r) {
            m_mesh = std::move(r.value);
        } else {
            FailMesh("Failed to parse .smd: " + smdPath + (r.error.empty() ? "" : " -- " + r.error));
            return;
        }
    }
    // No vertices and none declared: nothing to draw. (An .smd whose vertex
    // format is not supported yet declares its count and gets the info page.)
    if (m_mesh.vertices.empty() && m_mesh.vertexCount == 0) {
        FailMesh("No geometry in .smd: " + smdPath);
        return;
    }

    // Upload material textures — material loader reads GGPK directly (main thread only)
    if (m_isSmFile) {
        // Pass submesh count so MeshMaterial can expand run-length entries to per-submesh array.
        const size_t submeshCount = m_mesh.submeshRanges.empty()
            ? 1u
            : m_mesh.submeshRanges.size();
        m_material.LoadAllMaterials(m_smData, submeshCount, m_mgr);
        auto& mats = m_material.GetMaterials();
        if (mats.empty() || !mats[0].loaded) {
            m_material.LoadTextureFromSm(m_pending.inputData, m_mgr);
        }
    }

    if (m_mesh.vertices.empty()) {
        // Declared vertices in a format not decoded yet: Render shows the info page.
        m_loaded = true;
        m_loadState = MeshLoadState::Ready;
        m_pending = {};
        m_smdBytes.clear(); m_smdBytes.shrink_to_fit();
        m_rigAstBytes.clear(); m_rigAstBytes.shrink_to_fit();
        return;
    }

    // Recompute bbox from real vertices (same as old code)
    {
        constexpr float kMaxCoord = 1e6f;
        bool bboxInitialized = false;
        for (const auto& v : m_mesh.vertices) {
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) continue;
            if (std::fabs(v.x) > kMaxCoord || std::fabs(v.y) > kMaxCoord || std::fabs(v.z) > kMaxCoord) continue;
            if (!bboxInitialized) {
                m_mesh.bboxMin[0] = m_mesh.bboxMax[0] = v.x;
                m_mesh.bboxMin[1] = m_mesh.bboxMax[1] = v.y;
                m_mesh.bboxMin[2] = m_mesh.bboxMax[2] = v.z;
                bboxInitialized = true;
                continue;
            }
            if (v.x < m_mesh.bboxMin[0]) m_mesh.bboxMin[0] = v.x;
            if (v.x > m_mesh.bboxMax[0]) m_mesh.bboxMax[0] = v.x;
            if (v.y < m_mesh.bboxMin[1]) m_mesh.bboxMin[1] = v.y;
            if (v.y > m_mesh.bboxMax[1]) m_mesh.bboxMax[1] = v.y;
            if (v.z < m_mesh.bboxMin[2]) m_mesh.bboxMin[2] = v.z;
            if (v.z > m_mesh.bboxMax[2]) m_mesh.bboxMax[2] = v.z;
        }
    }

    if (!m_renderer.CreateMeshBuffers(m_mesh, m_error)) {
        m_loadState = MeshLoadState::Error;
        return;
    }

    InitResolvedSkeleton();

    // Phase 4.2: attach player.
    m_animPlayer.AttachSkeleton(&m_skeleton);

    m_camera.FitToBounds(m_mesh.bboxMin, m_mesh.bboxMax);
    m_staticLocalBbox = {};
    m_staticLocalBbox.Include(m_mesh.bboxMin[0], m_mesh.bboxMin[1], m_mesh.bboxMin[2]);
    m_staticLocalBbox.Include(m_mesh.bboxMax[0], m_mesh.bboxMax[1], m_mesh.bboxMax[2]);

    m_loaded = true;
    m_loadState = MeshLoadState::Ready;
    ApplyKeptView();

    // Free temporary byte buffers — GPU has them now
    m_pending = {};
    m_smdBytes.clear(); m_smdBytes.shrink_to_fit();
    m_rigAstBytes.clear(); m_rigAstBytes.shrink_to_fit();
}

// -----------------------------------------------------------------------
// TickLoadState — drives the state machine (called once per Render frame)
// -----------------------------------------------------------------------

void MeshViewer::TickLoadState() {
    if (m_loadState == MeshLoadState::Preparing) {
        if (!m_prepareFrameDrawn) {
            m_prepareFrameDrawn = true;
            return;
        }
        m_loadState = MeshLoadState::ReadingFiles;
        return;
    }
    if (m_loadState == MeshLoadState::ReadingFiles) {
        DoMainThreadRead();
        return;
    }
    if (m_loadState == MeshLoadState::UploadingGpu) {
        DoMainThreadUpload();
        return;
    }
    // Idle / Ready / Error — no transition work
}

// -----------------------------------------------------------------------
// ApplyKeptView — after a reload of the same file: the camera and render mode
// as they were, and the clip of the same name (if the skeleton still has it)
// at the same playhead, playing if it was.
// -----------------------------------------------------------------------

void MeshViewer::ApplyKeptView() {
    if (!m_keptView.valid) return;
    m_camera = m_keptView.camera;
    m_renderMode = m_keptView.renderMode;
    if (!m_keptView.clip.empty()) {
        const auto& names = m_animPlayer.AnimNames();
        const auto it = std::find(names.begin(), names.end(), m_keptView.clip);
        if (it != names.end()) {
            const auto clip = static_cast<std::size_t>(it - names.begin());
            m_animPlayer.SelectAnimation(clip);
            if (m_animPlayer.SelectedAnimIndex() == clip) {
                if (m_keptView.time > 0.0f) m_animPlayer.SeekSeconds(m_keptView.time);
                if (m_keptView.playing) m_animPlayer.Play();
            }
        }
    }
    m_keptView = {};
}

// -----------------------------------------------------------------------
// DrawMeshLoadingSpinner — mirrors PsgViewer::DrawLoadingSpinner pattern
// -----------------------------------------------------------------------

void MeshViewer::DrawMeshLoadingSpinner(const char* status) {
    ImVec2 regionSize = ImGui::GetContentRegionAvail();
    if (regionSize.x < 10.0f || regionSize.y < 10.0f) return;

    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImVec2 center(cursor.x + regionSize.x * 0.5f, cursor.y + regionSize.y * 0.5f);
    float radius = 22.0f;
    float t = static_cast<float>(ImGui::GetTime());

    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(cursor, ImVec2(cursor.x + regionSize.x, cursor.y + regionSize.y),
                      IM_COL32(0, 0, 0, 180));

    int segments = 12;
    for (int i = 0; i < segments; ++i) {
        float angle = t * 5.0f + static_cast<float>(i) * 3.14159265f * 2.0f / static_cast<float>(segments);
        float alpha = static_cast<float>(segments - i) / static_cast<float>(segments);
        ImVec2 p(center.x + cosf(angle) * radius, center.y + sinf(angle) * radius);
        dl->AddCircleFilled(p, 3.5f, IM_COL32(255, 255, 255, static_cast<int>(alpha * 255)));
    }

    if (status && status[0]) {
        ImVec2 tsz = ImGui::CalcTextSize(status);
        dl->AddText(ImVec2(center.x - tsz.x * 0.5f, center.y + radius + 10.0f),
                    IM_COL32(255, 255, 255, 255), status);
    }

    ImGui::Dummy(regionSize);
}

// -----------------------------------------------------------------------
// Bone visualization computation
// -----------------------------------------------------------------------

// Builds ResolvedSkeleton via FCNS walk (AstSkeletonBuilder) and seeds m_boneViz
// from the resulting bind-pose worldTransforms. Called once per load — expensive.
// The walker composes each bone's worldTransform using the same row-major
// row-vector convention as the old inline MulMat4; translation lives in [12..14].
void MeshViewer::InitResolvedSkeleton() {
    // Full rebuild — one-time, expensive.
    m_boneViz.clear();
    if (m_skeleton.bones.empty()) return;

    auto built = Parsers::BuildResolvedSkeleton(m_skeleton);
    if (!built.success) return;
    m_resolvedSkeleton = std::move(built.value);

    // Cache inverse-bind matrices once per skeleton load. Bind pose is
    // `m_resolvedSkeleton.bones[i].worldTransform` AT THIS POINT (before
    // AnimationPlayer::ApplyFrame ever runs). This must NOT be moved later
    // in the lifecycle.
    {
        const auto& bones = m_resolvedSkeleton.bones;
        m_bindInverseWorld.resize(bones.size());
        m_skinningMatrices.resize(bones.size());
        for (std::size_t i = 0; i < bones.size(); ++i) {
            if (!Animation::MatrixMath::Inverse4x4(bones[i].worldTransform,
                                                    m_bindInverseWorld[i].data())) {
                // Singular bind matrix — substitute identity. Mesh will skin
                // incorrectly through this bone, but won't NaN out.
                std::memset(m_bindInverseWorld[i].data(), 0, 64);
                m_bindInverseWorld[i][0]  = 1.0f;
                m_bindInverseWorld[i][5]  = 1.0f;
                m_bindInverseWorld[i][10] = 1.0f;
                m_bindInverseWorld[i][15] = 1.0f;
            }
        }
    }
    m_skinningTruncationWarned = false;

    // Initial bone-viz from bind pose.
    RefreshBoneVizFromWorldTransforms();

    // Fix #1: V01/V02 local->global bone remap. Build only if the mesh supplied
    // local bone names (DOLm meshes have boneNames empty and skip this). Names
    // are matched case-sensitive against m_resolvedSkeleton.bones[].name; any
    // local bone whose name is not in the skeleton maps to -1 (identity at
    // render time).
    m_boneRemap.clear();
    if (!m_mesh.boneNames.empty() && !m_resolvedSkeleton.bones.empty()) {
        m_boneRemap.reserve(m_mesh.boneNames.size());
        for (const auto& localName : m_mesh.boneNames) {
            int match = -1;
            for (size_t gi = 0; gi < m_resolvedSkeleton.bones.size(); ++gi) {
                if (m_resolvedSkeleton.bones[gi].name == localName) {
                    match = static_cast<int>(gi);
                    break;
                }
            }
            m_boneRemap.push_back(match);
        }
    }
}

// Refreshes m_boneViz directly from the current m_resolvedSkeleton.bones[].worldTransform.
// Called per-frame by the animation playback path — no ResolvedSkeleton rebuild.
void MeshViewer::RefreshBoneVizFromWorldTransforms() {
    m_boneViz.clear();
    if (m_resolvedSkeleton.bones.empty()) return;

    const auto& bones = m_resolvedSkeleton.bones;
    const std::size_t n = bones.size();
    m_boneViz.assign(n, {});

    constexpr float kMaxCoord = 1e6f;
    for (std::size_t i = 0; i < n; ++i) {
        m_boneViz[i].name = bones[i].name;

        const float* wm = bones[i].worldTransform;
        const float tx = wm[12], ty = wm[13], tz = wm[14];
        const bool ok =
            std::isfinite(tx) && std::isfinite(ty) && std::isfinite(tz) &&
            std::fabs(tx) < kMaxCoord && std::fabs(ty) < kMaxCoord &&
            std::fabs(tz) < kMaxCoord;

        m_boneViz[i].worldPos[0] = ok ? tx : std::numeric_limits<float>::quiet_NaN();
        m_boneViz[i].worldPos[1] = ok ? ty : std::numeric_limits<float>::quiet_NaN();
        m_boneViz[i].worldPos[2] = ok ? tz : std::numeric_limits<float>::quiet_NaN();

        const std::size_t p = bones[i].arrayParent;
        m_boneViz[i].parentIndex =
            (m_resolvedSkeleton.walkSucceeded && p != static_cast<std::size_t>(-1) && p < 256)
                ? static_cast<std::uint8_t>(p)
                : 0xFF;
    }
}

// -----------------------------------------------------------------------
// ImGui Render
// -----------------------------------------------------------------------

void MeshViewer::Render() {
    TickLoadState();

    // Async loading states draw a spinner until the transition completes
    if (m_loadState == MeshLoadState::Preparing
     || m_loadState == MeshLoadState::ReadingFiles) {
        DrawMeshLoadingSpinner("Reading files...");
        return;
    }
    if (m_loadState == MeshLoadState::UploadingGpu) {
        DrawMeshLoadingSpinner("Uploading to GPU...");
        return;
    }

    if (!m_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error: %s", m_error.c_str());
        RenderAocWarnings(kAllWarnings);   // no Info tab here: all of them
        return;
    }
    if (!m_loaded) {
        ImGui::TextDisabled("No mesh data loaded");
        return;
    }
    if (m_aocNoMesh) {
        // A broken parent can be what lost the skin: then say so instead of
        // claiming the .aoc names none.
        if (m_aocWarnings.empty())
            ImGui::TextDisabled("This .aoc file names no skin mesh.");
        else
            ImGui::TextDisabled("No skin mesh found: the extends chain could not be fully resolved.");
        RenderAocWarnings(kAllWarnings);
        return;
    }

    // Info-only mode: mesh parsed but no renderable vertices
    if (m_mesh.vertexCount > 0 && m_mesh.vertices.empty()) {
        ImGui::Text("Mesh Info: %s", m_filename.c_str());
        ImGui::Separator();
        ImGui::Text("Format: v%02u%s", m_mesh.version, m_mesh.version >= 9 ? " (DOLm)" : "");
        ImGui::Text("Declared vertices: %u", m_mesh.vertexCount);
        if (m_mesh.boneCount > 0) ImGui::Text("Bones: %u", m_mesh.boneCount);
        ImGui::Text("Triangles: %u", m_mesh.triangleCount);
        ImGui::Text("Indices: %zu", m_mesh.indices.size());
        if (!m_mesh.submeshName.empty())
            ImGui::Text("Submesh: %s", m_mesh.submeshName.c_str());
        ImGui::Separator();
        ImGui::Text("Bounding Box:");
        ImGui::Text("  Min: (%.2f, %.2f, %.2f)", m_mesh.bboxMin[0], m_mesh.bboxMin[1], m_mesh.bboxMin[2]);
        ImGui::Text("  Max: (%.2f, %.2f, %.2f)", m_mesh.bboxMax[0], m_mesh.bboxMax[1], m_mesh.bboxMax[2]);
        ImGui::Separator();
        ImGui::TextDisabled("3D preview not available -- vertex data format not yet supported for this file.");
        return;
    }

    if (!m_renderer.AreShadersReady()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Shaders not compiled");
        return;
    }

    // Tab bar: 3D View | Info
    // 3D View is shown for any loaded asset — mesh+skeleton, skeleton-only (.ast),
    // or mesh-only (.smd). Skeleton-only files display the bone hierarchy there.
    if (ImGui::BeginTabBar("##MeshViewerTabs")) {
        if (ImGui::BeginTabItem("3D View")) {
            Render3DView();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Info")) {
            RenderInfoTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// -----------------------------------------------------------------------
// 3D View tab
// -----------------------------------------------------------------------

void MeshViewer::Render3DView() {
    // Phase 4.2: per-frame animation update. Runs before any UI/canvas so the
    // refreshed bone viz is visible on this very frame.
    const float animDt = ImGui::GetIO().DeltaTime;
    m_animPlayer.Tick(animDt);
    // Fix #1a (2026-04-15): only sample/apply keyframe 0 when the user has
    // actually pressed Play or scrubbed off bind-pose. AttachSkeleton
    // auto-selects anim index 0 for UI convenience, but without this gate
    // ApplyFrame runs every frame even in "paused at t=0" state -- writing
    // the first keyframe's pose into the skeleton. For static-corpse V01
    // meshes whose vertices are baked in corpse-pose world space, any
    // non-identity palette displaces them -> visible shards.
    const bool wantApplyFrame =
        m_animPlayer.SelectedAnimIndex() != static_cast<std::size_t>(-1) &&
        (m_animPlayer.IsPlaying() || m_animPlayer.CurrentTime() > 0.0f);
    if (wantApplyFrame) {
        m_animPlayer.ApplyFrame(m_resolvedSkeleton);
        RefreshBoneVizFromWorldTransforms();
    }

    // Camera follow: keep the model's visible-geometry bbox centered in the
    // viewport. ComputeAnimatedBounds inflates the bone-joint bbox by the
    // static local mesh extent so pivot-offset rigs still stay centered.
    // Zoom is unchanged — user still controls orbit distance.
    if (m_animPanelState && m_animPanelState->followModel) {
        const auto b = GGPKExp::Animation::ComputeAnimatedBounds(
                           m_resolvedSkeleton, m_staticLocalBbox);
        if (b.Valid()) {
            float cx, cy, cz;
            b.Center(cx, cy, cz);
            m_camera.SetTarget(cx, cy, cz);
        }
    }

    uint32_t triCount = m_renderer.GetIndexCount() / 3;
    ImGui::Text("%u vertices | %u triangles | %s", m_renderer.GetVertexCount(), triCount, m_filename.c_str());
    if (m_mesh.detectedStride > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(stride %u)", m_mesh.detectedStride);
    }
    if (m_mesh.boneCount > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("[v%02u, %u bones]", m_mesh.version, m_mesh.boneCount);
    }

    bool hasTexture = m_material.HasTexture() ||
        (!m_material.GetMaterials().empty() && m_material.GetMaterials()[0].loaded);
    if (hasTexture) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[textured]");
    }

    GGPKExp::MeshViewSettings& vs =
        m_viewSettings ? *m_viewSettings : FallbackViewSettings();

    // Controls row
    ImGui::SetNextItemWidth(100.0f);
    ImGui::Combo("##Mode", &m_renderMode, "Solid\0Wireframe\0Solid+Wire\0");
    ImGui::SameLine();
    if (ImGui::Checkbox("Grid", &vs.showGrid)) m_animPanelSettingsDirty = true;
    ImGui::SameLine();
    if (!m_boneViz.empty()) {
        if (ImGui::Checkbox("Bones", &vs.showBones)) m_animPanelSettingsDirty = true;
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Reset View")) {
        m_camera.FitToBounds(m_mesh.bboxMin, m_mesh.bboxMax);
    }

    // An .aoc that names no skeleton shows its mesh in bind pose; when the
    // extends chain broke, the skeleton may have been in the lost parent.
    if (m_isAoc && m_aocSkeletonPath.empty())
        RenderAocWarnings(1);

    // Task 6: reserve a right-side strip for the animation panel when the
    // skeleton ships any animations. Panel state is owned (and persisted) by
    // the plugin; fall back to a local default if the plugin hasn't
    // wired it up yet (defensive).
    static GGPKExp::AnimationPanelState s_fallbackPanelState;
    GGPKExp::AnimationPanelState& panelState =
        m_animPanelState ? *m_animPanelState : s_fallbackPanelState;

    const bool  hasAnim = !m_skeleton.animations.empty();
    const float panelW  = hasAnim
        ? GGPKExp::Animation::Width(panelState) + 4.0f   // +4 px gap
        : 0.0f;

    // Canvas region: full height, reduced width to leave space for the panel.
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    const float fullPanelH = (std::max)(64.0f, canvasSize.y);
    canvasSize.x = (std::max)(64.0f, canvasSize.x - panelW);
    canvasSize.y = fullPanelH;

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##MeshCanvas", canvasSize,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);

    // Camera input (uses IsItemHovered internally)
    m_camera.ProcessInput(canvasPos, canvasSize);

    // Render 3D scene to offscreen target
    int cw = static_cast<int>(canvasSize.x);
    int ch = static_cast<int>(canvasSize.y);

    // Build the skinning palette from current pose + cached bind inverses.
    bool truncated = false;
    if (!m_bindInverseWorld.empty() && m_resolvedSkeleton.bones.size() == m_bindInverseWorld.size()) {
        Animation::BuildSkinningPalette(
            m_resolvedSkeleton,
            m_bindInverseWorld,
            /*maxBones*/ kMaxSkinBones,
            m_skinningMatrices,
            &truncated);
        if (truncated && !m_skinningTruncationWarned) {
            m_animPlayer.PushWarning(
                "skinning truncated: skel has " +
                std::to_string(m_resolvedSkeleton.bones.size()) +
                " bones, shader supports " +
                std::to_string(kMaxSkinBones));
            m_skinningTruncationWarned = true;
        }
    }

    // Fix #1: V01/V02 local->global palette shuffle. For DOLm meshes m_boneRemap
    // is empty -> skip this block, palette stays as-is.
    if (!m_boneRemap.empty() && !m_skinningMatrices.empty()) {
        // Identity matrix (row-major 4x4, MeshRenderer will transpose before upload).
        static constexpr std::array<float, 16> kIdentity = {
            1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1
        };
        // Snapshot of original palette so shuffle doesn't clobber slots it still needs.
        std::vector<std::array<float, 16>> original = m_skinningMatrices;
        const size_t boneCount = m_boneRemap.size();
        for (size_t i = 0; i < boneCount && i < m_skinningMatrices.size(); ++i) {
            int gi = m_boneRemap[i];
            if (gi >= 0 && static_cast<size_t>(gi) < original.size()) {
                m_skinningMatrices[i] = original[gi];
            } else {
                m_skinningMatrices[i] = kIdentity;
            }
        }
        // Slots beyond boneCount are not touched -- shader only samples 0..boneCount-1
        // for V01/V02 vertices.
    }

    const float* palettePtr = nullptr;
    std::size_t  paletteCount = 0;
    if (!m_skinningMatrices.empty()) {
        palettePtr   = m_skinningMatrices[0].data();
        paletteCount = m_skinningMatrices.size();
    }
    m_renderer.RenderScene(cw, ch, m_camera, m_material, m_mesh, m_renderMode,
                           palettePtr, paletteCount);

    // Display the rendered texture
    if (m_renderer.GetRenderTargetSRV()) {
        ImGui::SetCursorScreenPos(canvasPos);
        ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(m_renderer.GetRenderTargetSRV())), canvasSize);
    }

    // Overlay: grid, bones
    if ((vs.showGrid || vs.showBones) && m_loaded) {
        float vp[16];
        float aspect = canvasSize.x / canvasSize.y;
        m_camera.BuildVPMatrix(vp, aspect);

        auto project = [&](float px, float py, float pz) -> ImVec2 {
            float x = px * vp[0] + py * vp[4] + pz * vp[8]  + vp[12];
            float y = px * vp[1] + py * vp[5] + pz * vp[9]  + vp[13];
            float w = px * vp[3] + py * vp[7] + pz * vp[11] + vp[15];
            if (std::fabs(w) < 0.0001f) w = 0.0001f;
            x /= w; y /= w;
            float sx = canvasPos.x + (x * 0.5f + 0.5f) * canvasSize.x;
            float sy = canvasPos.y + (-y * 0.5f + 0.5f) * canvasSize.y;
            return ImVec2(sx, sy);
        };

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Grid + axes
        if (vs.showGrid) {
            float dx = m_mesh.bboxMax[0] - m_mesh.bboxMin[0];
            float dy = m_mesh.bboxMax[1] - m_mesh.bboxMin[1];
            float maxSpan = (std::max)(dx, dy);
            if (maxSpan < 1.0f) maxSpan = 1.0f;

            float gridStep = 1.0f;
            float target = maxSpan / 10.0f;
            if (target >= 100.0f) gridStep = 100.0f;
            else if (target >= 50.0f) gridStep = 50.0f;
            else if (target >= 20.0f) gridStep = 20.0f;
            else if (target >= 10.0f) gridStep = 10.0f;
            else if (target >= 5.0f) gridStep = 5.0f;
            else if (target >= 2.0f) gridStep = 2.0f;
            else if (target >= 1.0f) gridStep = 1.0f;
            else if (target >= 0.5f) gridStep = 0.5f;
            else gridStep = 0.1f;

            float gridExtent = gridStep * 10.0f;
            float gridZ = m_mesh.bboxMax[2];
            ImU32 gridCol = IM_COL32(255, 255, 255, 35);

            for (float x = -gridExtent; x <= gridExtent + 0.01f; x += gridStep) {
                ImVec2 a = project(x, -gridExtent, gridZ);
                ImVec2 b = project(x,  gridExtent, gridZ);
                dl->AddLine(a, b, gridCol, 1.0f);
            }
            for (float y = -gridExtent; y <= gridExtent + 0.01f; y += gridStep) {
                ImVec2 a = project(-gridExtent, y, gridZ);
                ImVec2 b = project( gridExtent, y, gridZ);
                dl->AddLine(a, b, gridCol, 1.0f);
            }

            float axLen = gridExtent * 0.6f;
            ImVec2 origin = project(0, 0, gridZ);
            ImVec2 xEnd = project(axLen, 0, gridZ);
            ImVec2 yEnd = project(0, axLen, gridZ);
            ImVec2 zEnd = project(0, 0, gridZ - axLen);
            dl->AddLine(origin, xEnd, IM_COL32(220, 60, 60, 200), 2.0f);
            dl->AddLine(origin, yEnd, IM_COL32(60, 200, 60, 200), 2.0f);
            dl->AddLine(origin, zEnd, IM_COL32(60, 100, 220, 200), 2.0f);
        }

        // Bone skeleton
        if (vs.showBones && !m_boneViz.empty()) {
            ImU32 boneCol = IM_COL32(0, 255, 200, 220);
            ImU32 jointCol = IM_COL32(255, 255, 0, 255);
            for (size_t i = 0; i < m_boneViz.size(); ++i) {
                auto& b = m_boneViz[i];
                if (!std::isfinite(b.worldPos[0])) continue;

                ImVec2 bp = project(b.worldPos[0], b.worldPos[1], b.worldPos[2]);

                if (b.parentIndex != 0xFF && b.parentIndex < m_boneViz.size()) {
                    auto& p = m_boneViz[b.parentIndex];
                    if (std::isfinite(p.worldPos[0])) {
                        ImVec2 pp = project(p.worldPos[0], p.worldPos[1], p.worldPos[2]);
                        dl->AddLine(pp, bp, boneCol, 2.0f);
                    }
                }

                dl->AddCircleFilled(bp, 3.0f, jointCol);
            }
        }
    }

    // Task 6: right-anchored animation panel. Uses the horizontal space
    // reserved earlier via canvasSize.x -= panelW. The 3D canvas's last
    // advancing widget is ImGui::Image() at canvasPos, so SameLine places
    // the panel immediately right of the canvas at the same vertical origin.
    if (hasAnim) {
        ImGui::SameLine(0.0f, 4.0f);
        const bool changed =
            GGPKExp::Animation::RenderAnimationSidePanel(m_animPlayer,
                                                         panelState,
                                                         m_playbackSpeed,
                                                         fullPanelH);
        if (changed) {
            m_animPanelSettingsDirty = true;
        }
    }
}

// -----------------------------------------------------------------------
// RenderAocWarnings — the .aoc resolver's warnings (a parent of the extends
// chain that is missing, does not parse, a cycle, the depth limit), at most
// `maxShown` of them; the Info tab lists them all.
// -----------------------------------------------------------------------

void MeshViewer::RenderAocWarnings(std::size_t maxShown) const {
    if (m_aocWarnings.empty() || maxShown == 0) return;
    const ImVec4 kWarnColor(1.0f, 0.75f, 0.3f, 1.0f);
    const std::size_t shown = (std::min)(maxShown, m_aocWarnings.size());
    ImGui::PushTextWrapPos(0.0f);
    for (std::size_t i = 0; i < shown; ++i)
        ImGui::TextColored(kWarnColor, "Warning: %s", m_aocWarnings[i].c_str());
    if (shown < m_aocWarnings.size())
        ImGui::TextDisabled("%zu more in the Info tab.", m_aocWarnings.size() - shown);
    ImGui::PopTextWrapPos();
}

// -----------------------------------------------------------------------
// Info tab — the file, its counts, skeleton and textures; Copy copies them.
// -----------------------------------------------------------------------

void MeshViewer::RenderInfoTab() {
    const std::string text = BuildInfoText();
    if (ImGui::SmallButton("Copy")) {
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::Separator();
    ImGui::BeginChild("##InfoScroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
    ImGui::EndChild();
}

std::string MeshViewer::BuildInfoText() const {
    std::string s;
    auto line = [&s](const std::string& text) {
        s += text;
        s += '\n';
    };

    line("File: " + m_path);
    if (m_isAoc) {
        line("Skin mesh: " + (m_aocSkinPath.empty() ? std::string("(none)") : m_aocSkinPath));
        if (!m_aocWarnings.empty()) {
            line("Extends chain warnings (" + std::to_string(m_aocWarnings.size()) + "):");
            for (const auto& w : m_aocWarnings) line("  " + w);
        }
    }
    if (m_isSmFile && !m_smData.smdPath.empty())
        line("Mesh data: " + m_smData.smdPath);

    const bool hasGeometry = m_mesh.vertexCount > 0 || !m_mesh.vertices.empty();
    if (hasGeometry) {
        const size_t submeshes = m_mesh.submeshRanges.empty() ? 1 : m_mesh.submeshRanges.size();
        line("Vertices: " + std::to_string(m_mesh.vertexCount));
        line("Triangles: " + std::to_string(m_mesh.triangleCount));
        line("Submeshes: " + std::to_string(submeshes));
    }

    // The skeleton looked up, and whether it loaded: m_rigSourceName is set
    // once its bytes were read, m_skeletonError when reading or parsing failed.
    const std::string notLoaded = m_skeletonError.empty()
        ? std::string(m_rigSourceName.empty() ? " (not found)" : "")
        : " (not loaded: " + m_skeletonError + ")";
    if (m_isAoc) {
        line("Skeleton: " + (m_aocSkeletonPath.empty() ? std::string("(none named)")
                                                       : m_aocSkeletonPath + notLoaded));
    } else if (m_isSmFile) {
        if (m_rigSourceName.empty()) {
            const auto dot = m_filename.rfind('.');
            const std::string smBase = (dot != std::string::npos) ? m_filename.substr(0, dot) : m_filename;
            line("Skeleton: (not found: no " + smBase + ".ast or rig.ast next to the mesh data)");
        } else {
            line("Skeleton: " + m_rigSourceName + (m_isStaticPropMesh ? " (local)" : " (shared)") + notLoaded);
        }
    }
    // Where the bone count comes from: the loaded skeleton, else the mesh data.
    if (!m_skeleton.bones.empty())
        line("Bones: " + std::to_string(m_skeleton.bones.size()) + " (from the skeleton)");
    else if (m_mesh.boneCount > 0)
        line("Bones: " + std::to_string(m_mesh.boneCount) + " (from the mesh data; no skeleton loaded)");
    line("Animation clips: " + std::to_string(m_skeleton.animations.size()));

    if (m_isSmFile) {
        const auto& mats = m_material.GetMaterials();
        line("Textures (" + std::to_string(m_smData.materialPaths.size()) + "):");
        // `mats` is the per-submesh expansion, not 1:1 with the .sm's material
        // entries: match by material path.
        for (const auto& entry : m_smData.materialPaths) {
            std::string dds;
            for (const auto& mt : mats) {
                if (mt.matPath == entry.path && mt.loaded) {
                    dds = mt.ddsPath;
                    break;
                }
            }
            line("  " + entry.path + (dds.empty() ? std::string("  (not loaded)") : "  -> " + dds));
        }
        if (m_material.HasTexture())
            line("  fallback -> " + m_material.FallbackDdsPath());
    }
    return s;
}

} // namespace GGPKExp
