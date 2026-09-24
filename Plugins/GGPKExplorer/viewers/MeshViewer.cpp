#include "MeshViewer.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../core/GGPKManager.h"
#include "../core/VirtualTree.h"
#include "../core/parsers/AocTokenizer.h"
#include "../core/parsers/AocParser.h"
#include "../core/parsers/AocSkeletonAugmenter.h"
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
    m_material.CreateFallbackTextures();
}

void MeshViewer::SetContext(GGPKManager* mgr, VirtualTree* tree) {
    m_mgr = mgr;
    m_tree = tree;
}

void MeshViewer::Clear() {
    m_mesh = {};
    m_renderer.ClearMeshBuffers();
    m_skeleton = {};
    m_resolvedSkeleton = {};
    m_staticLocalBbox = {};
    m_smData = {};
    m_isSmFile = false;
    m_hasSkinning = false;
    m_filename.clear();
    m_isStaticPropMesh = false;
    m_rigSourceName.clear();
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
    m_forceBindPose = false;

    // Phase 4.2: reset animation playback state.
    m_animPlayer.Reset();

    m_loaded = false;
    m_error.clear();

    // Reset display state
    m_camera.Reset();
    m_renderMode = 0;
    m_activeTab = 0;
    // NOTE: view toggles (Grid/Bones) live in m_viewSettings and survive
    // Clear() on purpose — user setting is remembered across model switches
    // and, once wired by the plugin, across sessions.

    m_material.Clear();
    m_material.CreateFallbackTextures();

    // Reset async loading state
    m_loadState = MeshLoadState::Idle;
    m_prepareFrameDrawn = false;
    m_pending = {};
    m_smdBytes.clear();
    m_smdHeaderBytes.clear();
    m_rigAstBytes.clear();

    // --- Sprint 3: reset .aoc overlay state ---
    m_aocDocument = {};
    m_aocWarnings.clear();
    m_pendingAocBytes.clear();
    m_pendingAocPath.clear();
    m_manualAocPathInput.clear();
    m_showSockets = false;
    m_showSocketNames = false;
    m_showSocketParentLines = false;
    m_manualAocDialogOpen = false;
}

// -----------------------------------------------------------------------
// IViewer::LoadData — starts the async state machine
// -----------------------------------------------------------------------

void MeshViewer::LoadData(std::vector<uint8_t> data, const std::string& path) {
    Clear();
    // Re-apply the persisted playback speed. Clear() -> Reset() already
    // preserves m_defaultSpeed inside the player, but the very first model
    // load after plugin start needs this to propagate the JSON-loaded value.
    if (m_playbackSpeed) {
        m_animPlayer.SetSpeed(*m_playbackSpeed);
    }
    m_pending.inputData = std::move(data);
    m_pending.path = path;
    m_filename = path;

    std::string ext;
    auto dot = path.rfind('.');
    if (dot != std::string::npos) ext = path.substr(dot);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    m_pending.ext = ext;

    m_prepareFrameDrawn = false;
    m_loadState = MeshLoadState::Preparing;
}

// -----------------------------------------------------------------------
// LoadMesh — backward-compat wrapper (called by ContentPanel). Delegates to
// LoadData which drives the async state machine.
// -----------------------------------------------------------------------

bool MeshViewer::LoadMesh(const std::vector<uint8_t>& data, const std::string& filename,
                           const std::string& /*fullPath*/, GGPKManager* mgr,
                           VirtualTree* tree) {
    if (mgr) m_mgr = mgr;
    if (tree) m_tree = tree;
    LoadData(std::vector<uint8_t>(data), filename);
    return true;
}

// -----------------------------------------------------------------------
// DoMainThreadRead — runs in the ReadingFiles phase. Reads .smd / .smd.header
// / rig.ast from GGPK, then transitions to UploadingGpu.
// -----------------------------------------------------------------------

void MeshViewer::DoMainThreadRead() {
    if (m_pending.inputData.empty() || !m_mgr || !m_tree) {
        m_error = "No data to load";
        m_loadState = MeshLoadState::Error;
        return;
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

        // Phase 4.2: attach player + auto-play first animation (idempotent w/ AttachSkeleton's auto-play).
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

        // --- Sprint 3: .aoc overlay for standalone .ast ---
        TryAutoDiscoverAoc(m_pending.path);
        ApplyPendingAoc();
        if (!m_resolvedSkeleton.virtualBones.empty()) m_showSockets = true;

        m_loaded = true;
        m_loadState = MeshLoadState::Ready;
        return;
    }

    m_isSmFile = (ext == ".sm");

    if (m_isSmFile) {
        auto smResult = Parsers::ParseSm(data.data(), data.size());
        if (smResult) m_smData = std::move(smResult.value);

        if (!m_smData.smdPath.empty()) {
            // Read .smd
            if (void* smdNode = m_mgr->FindNodeByPath(m_smData.smdPath)) {
                auto r = m_mgr->ReadFile(smdNode);
                if (r) m_smdBytes = std::move(r.value);
            }
            // Read companion .smd.header (may or may not exist)
            std::string headerPath = m_smData.smdPath + ".header";
            if (void* hNode = m_mgr->FindNodeByPath(headerPath)) {
                auto r = m_mgr->ReadFile(hNode);
                if (r) m_smdHeaderBytes = std::move(r.value);
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
            if (lastSlash != std::string::npos) {
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
        }
    } else if (ext == ".smd") {
        // Raw .smd file: input data IS the smd bytes
        m_smdBytes = data;
    } else {
        m_error = "Unsupported extension: " + ext;
        m_loadState = MeshLoadState::Error;
        return;
    }

    // --- Sprint 3: auto-discover companion .aoc ---
    {
        std::string basePath;
        if (!m_smData.smdPath.empty())       basePath = m_smData.smdPath;
        else if (!m_pending.path.empty())    basePath = m_pending.path;
        if (!basePath.empty()) {
            TryAutoDiscoverAoc(basePath);
        }
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
    }

    // Parse mesh bytes FIRST so we know submesh count before loading materials
    // (LoadAllMaterials expands run-length counts based on submesh count).
    if (!m_smdBytes.empty()) {
        auto r = Parsers::ParseSmd(m_smdBytes.data(), m_smdBytes.size());
        if (r) {
            m_mesh = std::move(r.value);
        } else {
            m_error = r.error.empty() ? "Failed to parse SMD mesh" : r.error;
            m_loadState = MeshLoadState::Error;
            return;
        }
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
            m_material.LoadTextureFromSm(m_pending.inputData, m_mgr, m_tree);
        }
    }

    if (m_mesh.vertices.empty()) {
        m_loaded = true;
        m_loadState = MeshLoadState::Ready;
        m_pending = {};
        m_smdBytes.clear(); m_smdBytes.shrink_to_fit();
        m_smdHeaderBytes.clear(); m_smdHeaderBytes.shrink_to_fit();
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

    // Phase 4.2: attach player (.aoc apply path — skeleton may have gained virtual bones).
    m_animPlayer.AttachSkeleton(&m_skeleton);

    // --- Sprint 3: apply pending .aoc overlay, if any ---
    ApplyPendingAoc();
    if (!m_resolvedSkeleton.virtualBones.empty()) {
        m_showSockets = true;  // helpful default: auto-enable when sockets discovered
    }

    m_camera.FitToBounds(m_mesh.bboxMin, m_mesh.bboxMax);
    m_staticLocalBbox = {};
    m_staticLocalBbox.Include(m_mesh.bboxMin[0], m_mesh.bboxMin[1], m_mesh.bboxMin[2]);
    m_staticLocalBbox.Include(m_mesh.bboxMax[0], m_mesh.bboxMax[1], m_mesh.bboxMax[2]);

    m_loaded = true;
    m_loadState = MeshLoadState::Ready;

    // Free temporary byte buffers — GPU has them now
    m_pending = {};
    m_smdBytes.clear(); m_smdBytes.shrink_to_fit();
    m_smdHeaderBytes.clear(); m_smdHeaderBytes.shrink_to_fit();
    m_rigAstBytes.clear(); m_rigAstBytes.shrink_to_fit();
    m_pendingAocBytes.clear(); m_pendingAocBytes.shrink_to_fit();
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
        return;
    }
    if (!m_loaded) {
        ImGui::TextDisabled("No mesh data loaded");
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
            m_activeTab = 0;
            Render3DView();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Info")) {
            m_activeTab = 1;
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
        (m_animPlayer.IsAnyLayerPlaying() || m_animPlayer.BaseLayerTime() > 0.0f);
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
    {
        bool anyNormal = false;
        for (auto& mt : m_material.GetMaterials()) if (mt.normalLoaded) { anyNormal = true; break; }
        if (anyNormal) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "[normal mapped]");
        }
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
    if (!m_resolvedSkeleton.virtualBones.empty()) {
        ImGui::Checkbox("Sockets", &m_showSockets);
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Reset View")) {
        m_camera.FitToBounds(m_mesh.bboxMin, m_mesh.bboxMax);
    }

    // --- Sprint 3: .aoc overlay status + controls ---
    if (!m_resolvedSkeleton.virtualBones.empty() || !m_aocWarnings.empty() || !m_pendingAocPath.empty()) {
        if (m_showSockets) {
            ImGui::Checkbox("Socket Names", &m_showSocketNames);
            ImGui::SameLine();
            ImGui::Checkbox("Socket->Parent Lines", &m_showSocketParentLines);
            ImGui::SameLine();
        }
        ImGui::Text("Bones: %zu  Sockets: %zu",
                    m_resolvedSkeleton.bones.size(),
                    m_resolvedSkeleton.virtualBones.size());
        if (!m_aocWarnings.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "  %zu AOC warnings", m_aocWarnings.size());
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                int maxShow = 15;
                for (const auto& w : m_aocWarnings) {
                    if (--maxShow < 0) { ImGui::Text("... (more)"); break; }
                    ImGui::TextUnformatted(w.c_str());
                }
                ImGui::EndTooltip();
            }
        }
        ImGui::Text("Overlay: %s", m_pendingAocPath.empty() ? "(none)" : m_pendingAocPath.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("...##aoc")) {
            m_manualAocPathInput = m_pendingAocPath;
            if (m_manualAocPathInput.size() >= 256) m_manualAocPathInput.resize(255);
            m_manualAocDialogOpen = true;
        }
    }
    RenderManualAocDialog();

    // Task 6: reserve a right-side strip for the animation panel when the
    // skeleton ships any animations. Panel state is shared with AocViewer
    // via the plugin; fall back to a local default if the plugin hasn't
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

    bool hovered = ImGui::IsItemHovered();
    ImVec2 mousePos = ImGui::GetIO().MousePos;

    // Vertex hover info
    if (hovered && m_loaded && !m_mesh.vertices.empty()) {
        float bestDist = 100.0f;
        int bestIdx = -1;
        float hvVP[16];
        float hvAspect = canvasSize.x / canvasSize.y;
        m_camera.BuildVPMatrix(hvVP, hvAspect);

        size_t step = m_mesh.vertices.size() > 5000 ? m_mesh.vertices.size() / 5000 : 1;
        for (size_t i = 0; i < m_mesh.vertices.size(); i += step) {
            auto& v = m_mesh.vertices[i];
            if (!std::isfinite(v.x) || std::fabs(v.x) > 1e6f) continue;
            float x = v.x*hvVP[0] + v.y*hvVP[4] + v.z*hvVP[8] + hvVP[12];
            float y = v.x*hvVP[1] + v.y*hvVP[5] + v.z*hvVP[9] + hvVP[13];
            float w = v.x*hvVP[3] + v.y*hvVP[7] + v.z*hvVP[11] + hvVP[15];
            if (w < 0.001f) continue;
            float sx = canvasPos.x + (x/w * 0.5f + 0.5f) * canvasSize.x;
            float sy = canvasPos.y + (-y/w * 0.5f + 0.5f) * canvasSize.y;
            float dx = sx - mousePos.x, dy = sy - mousePos.y;
            float d = dx*dx + dy*dy;
            if (d < bestDist * bestDist) {
                bestDist = std::sqrt(d);
                bestIdx = static_cast<int>(i);
            }
        }

        if (bestIdx >= 0 && bestDist < 15.0f) {
            auto& v = m_mesh.vertices[bestIdx];
            ImGui::BeginTooltip();
            ImGui::Text("Vertex %d", bestIdx);
            ImGui::Text("Pos: (%.2f, %.2f, %.2f)", v.x, v.y, v.z);
            float u = HalfToFloat(v.uvRaw[0]), uv = HalfToFloat(v.uvRaw[1]);
            ImGui::Text("UV: (%.4f, %.4f)", u, uv);
            ImGui::Text("Bones: [%d %d %d %d] w[%d %d %d %d]",
                v.boneData[0], v.boneData[1], v.boneData[2], v.boneData[3],
                v.boneData[4], v.boneData[5], v.boneData[6], v.boneData[7]);
            ImGui::EndTooltip();
        }
    }

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

    // Dev override: force palette to identity for all slots.
    if (m_forceBindPose) {
        static constexpr std::array<float, 16> kIdentityAll = {
            1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1
        };
        for (auto& m : m_skinningMatrices) m = kIdentityAll;
    }

    const float* palettePtr = nullptr;
    std::size_t  paletteCount = 0;
    if (!m_skinningMatrices.empty()) {
        palettePtr   = m_skinningMatrices[0].data();
        paletteCount = m_skinningMatrices.size();
    }
    m_renderer.RenderScene(cw, ch, m_camera, m_material, m_mesh, m_renderMode,
                           m_hasSkinning, palettePtr, paletteCount);

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

        // --- Sprint 3: .aoc sockets overlay ---
        if (m_showSockets && !m_resolvedSkeleton.virtualBones.empty()) {
            const ImU32 colSocket = IM_COL32(0, 220, 220, 255);   // cyan
            const ImU32 colLine   = IM_COL32(0, 220, 220, 160);

            for (const auto& vb : m_resolvedSkeleton.virtualBones) {
                const float wx = vb.worldTransform[12];
                const float wy = vb.worldTransform[13];
                const float wz = vb.worldTransform[14];
                if (!std::isfinite(wx) || !std::isfinite(wy) || !std::isfinite(wz)) continue;

                ImVec2 sp = project(wx, wy, wz);
                const float r = 4.0f;
                dl->AddLine(ImVec2(sp.x - r, sp.y),     ImVec2(sp.x,     sp.y - r), colSocket, 1.5f);
                dl->AddLine(ImVec2(sp.x,     sp.y - r), ImVec2(sp.x + r, sp.y),     colSocket, 1.5f);
                dl->AddLine(ImVec2(sp.x + r, sp.y),     ImVec2(sp.x,     sp.y + r), colSocket, 1.5f);
                dl->AddLine(ImVec2(sp.x,     sp.y + r), ImVec2(sp.x - r, sp.y),     colSocket, 1.5f);

                if (m_showSocketNames) {
                    dl->AddText(ImVec2(sp.x + 6, sp.y - 6), colSocket, vb.name.c_str());
                }
                if (m_showSocketParentLines && vb.parentBoneIndex < m_boneViz.size()) {
                    const auto& pb = m_boneViz[vb.parentBoneIndex];
                    if (std::isfinite(pb.worldPos[0])) {
                        ImVec2 pp = project(pb.worldPos[0], pb.worldPos[1], pb.worldPos[2]);
                        dl->AddLine(pp, sp, colLine, 1.0f);
                    }
                }
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
// Info tab
// -----------------------------------------------------------------------

void MeshViewer::RenderInfoTab() {
    if (ImGui::SmallButton("Copy All")) {
        std::string text = BuildInfoText();
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(copies info as text to clipboard)");

    // Static-prop banner (Option C, 2026-04-15): surface the per-mesh-rig
    // detection so the user understands why Play may behave differently on
    // this mesh (single-pose "once"/"open" clip vs. full idle loop on
    // shared-rig characters). Amber matches existing "warning" tone.
    if (m_isStaticPropMesh && !m_rigSourceName.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.25f, 1.0f));
        ImGui::TextWrapped("Static prop mesh: uses local skeleton %s",
                           m_rigSourceName.c_str());
        ImGui::PopStyleColor();
        ImGui::TextDisabled(
            "(not the shared rig.ast; animation is single-pose / event-triggered,"
            " not an idle loop)");
        ImGui::Separator();
    }

    ImGui::BeginChild("##InfoScroll", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

    // --- Mesh Data (.smd) ---
    if (ImGui::CollapsingHeader("Mesh Data (.smd)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(8.0f);
        ImGui::Text("Version: v%02u%s", m_mesh.version, m_mesh.version >= 3 ? " (DOLm)" : "");
        ImGui::Text("Vertices: %u", m_mesh.vertexCount);
        ImGui::Text("Triangles: %u", m_mesh.triangleCount);
        ImGui::Text("Indices: %zu", m_mesh.indices.size());
        if (m_mesh.detectedStride > 0)
            ImGui::Text("Vertex stride: %u bytes", m_mesh.detectedStride);
        if (m_mesh.boneCount > 0)
            ImGui::Text("Bones: %u", m_mesh.boneCount);
        if (m_mesh.blendCount > 0)
            ImGui::Text("Blend shapes: %u", m_mesh.blendCount);
        if (!m_mesh.submeshName.empty())
            ImGui::Text("Submesh: %s", m_mesh.submeshName.c_str());
        if (m_mesh.version >= 3) {
            ImGui::TextDisabled("DOLm offset: %u, DOLm version: %u, bone entries: %u, submeshes: %u",
                m_mesh.dolmOffset, m_mesh.dolmVersion, m_mesh.boneDataEntryCount, m_mesh.submeshCount);
        }
        ImGui::Separator();
        ImGui::Text("Bounding Box:");
        ImGui::Text("  Min: (%.4f, %.4f, %.4f)", m_mesh.bboxMin[0], m_mesh.bboxMin[1], m_mesh.bboxMin[2]);
        ImGui::Text("  Max: (%.4f, %.4f, %.4f)", m_mesh.bboxMax[0], m_mesh.bboxMax[1], m_mesh.bboxMax[2]);
        float dx = m_mesh.bboxMax[0] - m_mesh.bboxMin[0];
        float dy = m_mesh.bboxMax[1] - m_mesh.bboxMin[1];
        float dz = m_mesh.bboxMax[2] - m_mesh.bboxMin[2];
        ImGui::Text("  Size: %.2f x %.2f x %.2f", dx, dy, dz);
        ImGui::Unindent(8.0f);
    }

    // --- SM Descriptor ---
    if (m_isSmFile && !m_smData.smdPath.empty()) {
        if (ImGui::CollapsingHeader("SM Descriptor (.sm)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            ImGui::Text("SM version: %u", m_smData.version);
            ImGui::Text("Mesh path: %s", m_smData.smdPath.c_str());
            if (!m_rigSourceName.empty()) {
                if (m_isStaticPropMesh) {
                    ImGui::Text("Rig source: ");
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                                       "%s (local)", m_rigSourceName.c_str());
                } else {
                    ImGui::Text("Rig source: %s (shared)", m_rigSourceName.c_str());
                }
            }
            if (m_smData.boneGroupCount > 0)
                ImGui::Text("Bone groups: %u", m_smData.boneGroupCount);

            ImGui::Separator();
            auto& mats = m_material.GetMaterials();
            ImGui::Text("Materials (%zu):", m_smData.materialPaths.size());
            // Note: `mats` is the per-submesh expansion (size == submeshCount),
            // not 1:1 with the raw material entries. Find loaded/ddsPath/normal
            // info by matching matPath against the current entry's .path.
            for (size_t i = 0; i < m_smData.materialPaths.size(); ++i) {
                const char* pathStr = m_smData.materialPaths[i].path.c_str();
                uint32_t runLen = m_smData.materialPaths[i].submeshCount;

                bool loaded = false;
                bool hasNormal = false;
                std::string ddsPath;
                std::string normalDdsPath;
                for (const auto& mt : mats) {
                    if (mt.matPath == m_smData.materialPaths[i].path) {
                        if (mt.loaded) {
                            loaded = true;
                            if (ddsPath.empty()) ddsPath = mt.ddsPath;
                        }
                        if (mt.normalLoaded) {
                            hasNormal = true;
                            if (normalDdsPath.empty()) normalDdsPath = mt.normalDdsPath;
                        }
                        if (loaded && hasNormal) break;
                    }
                }

                if (loaded)
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                        "  [%zu] %s  x%u submesh%s", i, pathStr, runLen, runLen == 1 ? "" : "es");
                else
                    ImGui::Text("  [%zu] %s  x%u submesh%s", i, pathStr, runLen, runLen == 1 ? "" : "es");

                if (!ddsPath.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("-> %s", ddsPath.c_str());
                }
                if (hasNormal) {
                    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "       normal: %s",
                        normalDdsPath.c_str());
                }
            }

            if (!m_mesh.submeshRanges.empty() && m_mesh.submeshRanges.size() > 1) {
                ImGui::Separator();
                ImGui::Text("Submesh ranges (%zu):", m_mesh.submeshRanges.size());
                for (size_t i = 0; i < m_mesh.submeshRanges.size(); ++i) {
                    auto& r = m_mesh.submeshRanges[i];
                    ImGui::Text("  [%zu] %u indices (offset %u), %u vertices (offset %u)",
                        i, r.indexCount, r.indexOffset, r.vertexCount, r.vertexOffset);
                }
            }

            if (m_smData.bboxMin[0] != 0 || m_smData.bboxMax[0] != 0) {
                ImGui::Separator();
                ImGui::Text("SM Bounding Box:");
                ImGui::Text("  Min: (%.4f, %.4f, %.4f)", m_smData.bboxMin[0], m_smData.bboxMin[1], m_smData.bboxMin[2]);
                ImGui::Text("  Max: (%.4f, %.4f, %.4f)", m_smData.bboxMax[0], m_smData.bboxMax[1], m_smData.bboxMax[2]);
            }
            ImGui::Unindent(8.0f);
        }
    }

    // --- Texture Pipeline ---
    if (m_isSmFile) {
        auto& texInfo = m_material.GetTexInfo();
        if (ImGui::CollapsingHeader("Texture Pipeline", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);

            if (!texInfo.matPath.empty()) {
                ImGui::Text("Material: %s", texInfo.matPath.c_str());
                if (texInfo.matFound) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "(%zu bytes)", texInfo.matSize);
                } else {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "NOT FOUND");
                }
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "No .mat path found in .sm");
            }

            if (!texInfo.matJsonError.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "JSON error: %s", texInfo.matJsonError.c_str());
            }

            if (!texInfo.allDdsPaths.empty()) {
                ImGui::Separator();
                ImGui::Text("DDS textures found (%zu):", texInfo.allDdsPaths.size());
                for (size_t i = 0; i < texInfo.allDdsPaths.size(); ++i) {
                    bool isSelected = (texInfo.allDdsPaths[i] == texInfo.selectedDdsPath);
                    if (isSelected)
                        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "  > %s", texInfo.allDdsPaths[i].c_str());
                    else
                        ImGui::Text("    %s", texInfo.allDdsPaths[i].c_str());
                }
            }

            if (!texInfo.selectedDdsPath.empty()) {
                ImGui::Separator();
                ImGui::Text("Active texture: %s", texInfo.selectedDdsPath.c_str());
                if (texInfo.ddsFound) {
                    ImGui::Text("  File: %zu bytes", texInfo.ddsSize);
                    if (texInfo.gpuLoaded) {
                        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "  GPU: loaded OK");
                    } else if (!texInfo.gpuError.empty()) {
                        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "  GPU: %s", texInfo.gpuError.c_str());
                    }
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "  NOT FOUND in GGPK");
                }
            }

            auto& texLog = m_material.GetTexLog();
            if (!texLog.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("Raw log: %s", texLog.c_str());
            }

            ImGui::Unindent(8.0f);
        }
    }

    // --- Skeleton ---
    if (!m_skeleton.bones.empty()) {
        if (ImGui::CollapsingHeader("Skeleton")) {
            ImGui::Indent(8.0f);
            ImGui::Text("Bones: %zu (version: 0x%02X, header count: %u)",
                        m_skeleton.bones.size(),
                        static_cast<unsigned>(m_skeleton.version),
                        static_cast<unsigned>(m_skeleton.boneCountHeader));

            if (ImGui::BeginTable("##InfoBones", 4,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                    ImVec2(0, 250.0f))) {
                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("First Child", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Next Sibling", ImGuiTableColumnFlags_WidthFixed, 90);
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < m_skeleton.bones.size(); ++i) {
                    auto& b = m_skeleton.bones[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%zu", i);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(b.name.c_str());
                    ImGui::TableNextColumn();
                    if (b.firstChildIndex == 0xFF) ImGui::TextDisabled("leaf");
                    else ImGui::Text("%u", b.firstChildIndex);
                    ImGui::TableNextColumn();
                    if (b.nextSiblingIndex == 0xFF) ImGui::TextDisabled("last");
                    else ImGui::Text("%u", b.nextSiblingIndex);
                }
                ImGui::EndTable();
            }
            ImGui::Unindent(8.0f);
        }
    }

    // --- Animations ---
    if (!m_skeleton.animations.empty()) {
        if (ImGui::CollapsingHeader("Animations", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            ImGui::Text("Clips: %zu", m_skeleton.animations.size());
            if (ImGui::BeginTable("##InfoAnims", 3,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                    ImVec2(0, 200.0f))) {
                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Bones", ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < m_skeleton.animations.size(); ++i) {
                    auto& a = m_skeleton.animations[i];
                    const char* pb = (a.playbackType == 'l') ? "loop"
                                   : (a.playbackType == 'n') ? "none"
                                   : (a.playbackType == 'o') ? "once"
                                   : "?";
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%zu", i);
                    ImGui::TableNextColumn(); ImGui::Text("%s [%s]", a.name.c_str(), pb);
                    ImGui::TableNextColumn(); ImGui::Text("%u", static_cast<unsigned>(a.animBoneCount));
                }
                ImGui::EndTable();
            }
            ImGui::Unindent(8.0f);
        }
    }

    // --- Render Stats ---
    if (ImGui::CollapsingHeader("Render Stats")) {
        ImGui::Indent(8.0f);
        ImGui::Text("GPU vertex count: %u", m_renderer.GetVertexCount());
        ImGui::Text("GPU index count: %u", m_renderer.GetIndexCount());
        ImGui::Text("Render target: %d x %d", m_renderer.GetRenderTargetWidth(), m_renderer.GetRenderTargetHeight());
        bool hasTexture2 = m_material.HasTexture() ||
            (!m_material.GetMaterials().empty() && m_material.GetMaterials()[0].loaded);
        ImGui::Text("Texture bound: %s", hasTexture2 ? "yes (DDS)" : "no (white fallback)");
        ImGui::Text("Render mode: %s", m_renderMode == 0 ? "Solid" : m_renderMode == 1 ? "Wireframe" : "Solid+Wire");
        ImGui::Unindent(8.0f);
    }

    // Fix #1 diagnostics: local bone names + resolved remap.
    if (!m_mesh.boneNames.empty()) {
        if (ImGui::CollapsingHeader("V01/V02 Bone Remap (Fix #1)",
                                     ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Local bones: %zu", m_mesh.boneNames.size());
            ImGui::Text("Remap entries: %zu", m_boneRemap.size());
            int unresolved = 0;
            for (int gi : m_boneRemap) if (gi < 0) ++unresolved;
            if (unresolved > 0)
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                   "Unresolved: %d (will use identity)", unresolved);
            else
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                                   "All bones resolved");

            if (ImGui::BeginTable("##boneremap", 3,
                                   ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Local idx");
                ImGui::TableSetupColumn("Local name");
                ImGui::TableSetupColumn("Global idx");
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < m_mesh.boneNames.size(); ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%zu", i);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(m_mesh.boneNames[i].c_str());
                    ImGui::TableNextColumn();
                    int gi = (i < m_boneRemap.size()) ? m_boneRemap[i] : -1;
                    if (gi < 0) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "(none)");
                    else        ImGui::Text("%d", gi);
                }
                ImGui::EndTable();
            }
        }
    }

    // Dev tool: force identity palette (isolates skinning bugs vs vertex-decode bugs).
    if (ImGui::CollapsingHeader("Dev tools")) {
        ImGui::Checkbox("Force Bind Pose (identity palette)", &m_forceBindPose);
        ImGui::TextDisabled("When ON, all skinning matrices forced to identity.");
        ImGui::TextDisabled("If shards disappear with this ON, the bug is in palette/remap,");
        ImGui::TextDisabled("not vertex decode.");
    }

    ImGui::EndChild();
}

// -----------------------------------------------------------------------
// Build plain-text report
// -----------------------------------------------------------------------

std::string MeshViewer::BuildInfoText() const {
    std::string s;
    s.reserve(4096);

    auto line = [&](const char* fmt, ...) {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        s += buf;
        s += '\n';
    };

    line("=== Mesh Info: %s ===", m_filename.c_str());
    line("");

    line("[Mesh Data (.smd)]");
    line("Version: v%02u%s", m_mesh.version, m_mesh.version >= 3 ? " (DOLm)" : "");
    line("Vertices: %u", m_mesh.vertexCount);
    line("Triangles: %u", m_mesh.triangleCount);
    line("Indices: %zu", m_mesh.indices.size());
    if (m_mesh.detectedStride > 0)
        line("Vertex stride: %u bytes", m_mesh.detectedStride);
    if (m_mesh.boneCount > 0)
        line("Bones: %u", m_mesh.boneCount);
    if (m_mesh.blendCount > 0)
        line("Blend shapes: %u", m_mesh.blendCount);
    if (!m_mesh.submeshName.empty())
        line("Submesh: %s", m_mesh.submeshName.c_str());
    if (m_mesh.version >= 3)
        line("DOLm: offset=%u, version=%u, bone_entries=%u, submeshes=%u",
             m_mesh.dolmOffset, m_mesh.dolmVersion, m_mesh.boneDataEntryCount, m_mesh.submeshCount);
    line("Index format: %s", m_renderer.Use32BitIndices() ? "uint32" : "uint16");
    line("BBox Min: (%.4f, %.4f, %.4f)", m_mesh.bboxMin[0], m_mesh.bboxMin[1], m_mesh.bboxMin[2]);
    line("BBox Max: (%.4f, %.4f, %.4f)", m_mesh.bboxMax[0], m_mesh.bboxMax[1], m_mesh.bboxMax[2]);
    float dx = m_mesh.bboxMax[0] - m_mesh.bboxMin[0];
    float dy = m_mesh.bboxMax[1] - m_mesh.bboxMin[1];
    float dz = m_mesh.bboxMax[2] - m_mesh.bboxMin[2];
    line("BBox Size: %.2f x %.2f x %.2f", dx, dy, dz);

    auto& mats = m_material.GetMaterials();
    if (m_isSmFile && !m_smData.smdPath.empty()) {
        line("");
        line("[SM Descriptor (.sm)]");
        line("SM version: %u", m_smData.version);
        line("Mesh path: %s", m_smData.smdPath.c_str());
        if (!m_rigSourceName.empty()) {
            line("Rig source: %s (%s)", m_rigSourceName.c_str(),
                 m_isStaticPropMesh ? "local, static prop" : "shared");
        }
        if (m_smData.boneGroupCount > 0)
            line("Bone groups: %u", m_smData.boneGroupCount);
        line("Materials (%zu):", m_smData.materialPaths.size());
        // Note: `mats` is the per-submesh expansion (size == submeshCount),
        // not 1:1 with the raw material entries. Find loaded/ddsPath/normal
        // info by matching matPath against the current entry's .path.
        for (size_t i = 0; i < m_smData.materialPaths.size(); ++i) {
            const char* pathStr = m_smData.materialPaths[i].path.c_str();
            uint32_t runLen = m_smData.materialPaths[i].submeshCount;

            bool loaded = false;
            bool hasNormal = false;
            std::string ddsPath;
            std::string normalDdsPath;
            for (const auto& mt : mats) {
                if (mt.matPath == m_smData.materialPaths[i].path) {
                    if (mt.loaded) {
                        loaded = true;
                        if (ddsPath.empty()) ddsPath = mt.ddsPath;
                    }
                    if (mt.normalLoaded) {
                        hasNormal = true;
                        if (normalDdsPath.empty()) normalDdsPath = mt.normalDdsPath;
                    }
                    if (loaded && hasNormal) break;
                }
            }

            std::string ddsInfo = ddsPath.empty() ? std::string() : (" -> " + ddsPath);
            char runBuf[32];
            snprintf(runBuf, sizeof(runBuf), " x%u submesh%s", runLen, runLen == 1 ? "" : "es");
            line("  [%zu] %s%s%s%s", i, pathStr, runBuf,
                 loaded ? " [TEXTURED]" : "", ddsInfo.c_str());
            if (hasNormal)
                line("       normal: %s", normalDdsPath.c_str());
        }
        if (!m_mesh.submeshRanges.empty() && m_mesh.submeshRanges.size() > 1) {
            line("Submesh ranges (%zu):", m_mesh.submeshRanges.size());
            for (size_t i = 0; i < m_mesh.submeshRanges.size(); ++i) {
                auto& r = m_mesh.submeshRanges[i];
                line("  [%zu] %u idx (off %u), %u vtx (off %u)",
                     i, r.indexCount, r.indexOffset, r.vertexCount, r.vertexOffset);
            }
        }
        if (m_smData.bboxMin[0] != 0 || m_smData.bboxMax[0] != 0) {
            line("SM BBox Min: (%.4f, %.4f, %.4f)", m_smData.bboxMin[0], m_smData.bboxMin[1], m_smData.bboxMin[2]);
            line("SM BBox Max: (%.4f, %.4f, %.4f)", m_smData.bboxMax[0], m_smData.bboxMax[1], m_smData.bboxMax[2]);
        }
    }

    auto& texInfo = m_material.GetTexInfo();
    if (m_isSmFile) {
        line("");
        line("[Texture Pipeline]");
        if (!texInfo.matPath.empty()) {
            line("Material: %s (%s, %zu bytes)",
                 texInfo.matPath.c_str(),
                 texInfo.matFound ? "found" : "NOT FOUND",
                 texInfo.matSize);
        }
        if (!texInfo.matJsonError.empty())
            line("JSON error: %s", texInfo.matJsonError.c_str());
        if (!texInfo.allDdsPaths.empty()) {
            line("DDS textures found (%zu):", texInfo.allDdsPaths.size());
            for (auto& p : texInfo.allDdsPaths)
                line("  %s%s", p.c_str(), p == texInfo.selectedDdsPath ? " [selected]" : "");
        }
        if (!texInfo.selectedDdsPath.empty()) {
            line("Active texture: %s", texInfo.selectedDdsPath.c_str());
            if (texInfo.ddsFound) {
                line("  File: %zu bytes", texInfo.ddsSize);
                line("  GPU: %s", texInfo.gpuLoaded ? "loaded OK" : texInfo.gpuError.c_str());
            } else {
                line("  NOT FOUND in GGPK");
            }
        }
    }

    if (!m_skeleton.bones.empty()) {
        line("");
        line("[Skeleton]");
        line("Bones: %zu", m_skeleton.bones.size());
        line("Version: 0x%02X", static_cast<unsigned>(m_skeleton.version));
        line("Header bone count: %u", static_cast<unsigned>(m_skeleton.boneCountHeader));
        for (size_t i = 0; i < m_skeleton.bones.size(); ++i) {
            auto& b = m_skeleton.bones[i];
            line("  [%zu] %s (first_child: %s, next_sibling: %s)", i,
                 b.name.c_str(),
                 b.firstChildIndex == 0xFF ? "leaf" : std::to_string(b.firstChildIndex).c_str(),
                 b.nextSiblingIndex == 0xFF ? "last" : std::to_string(b.nextSiblingIndex).c_str());
        }
    }

    if (!m_skeleton.animations.empty()) {
        line("");
        line("[Animations] (%zu clips)", m_skeleton.animations.size());
        for (size_t i = 0; i < m_skeleton.animations.size(); ++i) {
            auto& a = m_skeleton.animations[i];
            line("  [%zu] %s (bones: %u, fps: %u, playback: %c)",
                 i, a.name.c_str(),
                 static_cast<unsigned>(a.animBoneCount),
                 static_cast<unsigned>(a.fps),
                 a.playbackType);
        }
    }

    line("");
    line("[Render Stats]");
    line("GPU vertices: %u", m_renderer.GetVertexCount());
    line("GPU indices: %u", m_renderer.GetIndexCount());
    line("Render target: %d x %d", m_renderer.GetRenderTargetWidth(), m_renderer.GetRenderTargetHeight());
    bool hasTexture2 = m_material.HasTexture() ||
        (!mats.empty() && mats[0].loaded);
    line("Texture: %s", hasTexture2 ? "DDS" : "white fallback");
    line("Camera: dist=%.2f, yaw=%.3f, pitch=%.3f, target=(%.2f, %.2f, %.2f)",
         m_camera.GetDistance(), m_camera.GetYaw(), m_camera.GetPitch(),
         0.0f, 0.0f, 0.0f); // target position available via GetTargetPosition but we don't expose individual floats here

    return s;
}

// -----------------------------------------------------------------------
// Half-float to float conversion
// -----------------------------------------------------------------------

float MeshViewer::HalfToFloat(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;

    if (exp == 0) {
        if (mant == 0) return sign ? -0.0f : 0.0f;
        float f = static_cast<float>(mant) / 1024.0f;
        f *= (1.0f / 16384.0f);
        return sign ? -f : f;
    }
    if (exp == 31) {
        if (mant == 0) return sign ? -INFINITY : INFINITY;
        return NAN;
    }

    uint32_t f32 = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    float result;
    std::memcpy(&result, &f32, 4);
    return result;
}

// -----------------------------------------------------------------------
// Sprint 3: .aoc overlay helpers
// -----------------------------------------------------------------------

std::string MeshViewer::StripExtensionForAoc(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return path;
    // Only strip if the extension is short (e.g., .sm, .smd, .ast)
    if (path.size() - dot > 6) return path;
    return path.substr(0, dot);
}

void MeshViewer::TryAutoDiscoverAoc(const std::string& meshOrAstPath) {
    m_pendingAocBytes.clear();
    m_pendingAocPath.clear();
    if (!m_mgr) return;
    std::string aocPath = StripExtensionForAoc(meshOrAstPath) + ".aoc";
    void* node = m_mgr->FindNodeByPath(aocPath);
    if (!node) return;
    auto r = m_mgr->ReadFile(node);
    if (!r) return;
    m_pendingAocBytes = std::move(r.value);
    m_pendingAocPath = aocPath;
}

void MeshViewer::ApplyPendingAoc() {
    m_aocDocument = {};
    m_aocWarnings.clear();
    m_resolvedSkeleton.virtualBones.clear();

    if (m_pendingAocBytes.empty() || !m_resolvedSkeleton.walkSucceeded) return;

    auto tok = Parsers::TokenizeAoc(m_pendingAocBytes.data(), m_pendingAocBytes.size());
    if (!tok) {
        m_aocWarnings.push_back("AOC tokenize error: " + tok.error);
        return;
    }
    auto file = Parsers::ParseAoc(tok.value);
    if (!file) {
        m_aocWarnings.push_back("AOC parse error: " + file.error);
        return;
    }
    file.value.sourcePath = m_pendingAocPath;

    auto doc = Parsers::ResolveExtends(file.value, m_mgr, &m_aocResolverCache);
    if (!doc) {
        m_aocWarnings.push_back("AOC resolve error: " + doc.error);
        return;
    }
    m_aocDocument = std::move(doc.value);
    for (const auto& w : m_aocDocument.warnings) m_aocWarnings.push_back(w);

    std::vector<std::string> augWarns;
    auto aug = Parsers::AugmentSkeleton(m_aocDocument, m_resolvedSkeleton, &augWarns);
    if (!aug) {
        m_aocWarnings.push_back("AOC augment error: " + aug.error);
        return;
    }
    for (const auto& w : augWarns) m_aocWarnings.push_back(w);
}

void MeshViewer::RenderManualAocDialog() {
    if (!m_manualAocDialogOpen) return;
    ImGui::OpenPopup("Load .aoc Overlay##aocPicker");
    if (ImGui::BeginPopupModal("Load .aoc Overlay##aocPicker", &m_manualAocDialogOpen,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("GGPK path to .aoc file:");
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", m_manualAocPathInput.c_str());
        if (ImGui::InputText("##aocPath", buf, sizeof(buf))) {
            m_manualAocPathInput = buf;
        }
        if (ImGui::Button("Load", ImVec2(100, 0))) {
            if (m_mgr) {
                if (void* node = m_mgr->FindNodeByPath(m_manualAocPathInput)) {
                    auto r = m_mgr->ReadFile(node);
                    if (r) {
                        m_pendingAocBytes = std::move(r.value);
                        m_pendingAocPath  = m_manualAocPathInput;
                        ApplyPendingAoc();
                        m_showSockets = !m_resolvedSkeleton.virtualBones.empty();
                        m_manualAocDialogOpen = false;
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear", ImVec2(100, 0))) {
            m_pendingAocBytes.clear();
            m_pendingAocPath.clear();
            m_aocDocument = {};
            m_aocWarnings.clear();
            m_resolvedSkeleton.virtualBones.clear();
            m_showSockets = false;
            m_manualAocDialogOpen = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            m_manualAocDialogOpen = false;
        }
        ImGui::EndPopup();
    }
}

} // namespace GGPKExp
