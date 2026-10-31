#pragma once
#include <array>
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <d3d11.h>
#include <imgui.h>
#include "IViewer.h"
#include "../core/parsers/SmdParser.h"
#include "../core/parsers/AstParser.h"
#include "../core/parsers/AstSkeletonBuilder.h"
#include "../core/parsers/SmParser.h"
#include "mesh/MeshCamera.h"
#include "mesh/MeshRenderer.h"
#include "mesh/MeshMaterial.h"
#include "animation/AnimationPlayer.h"
#include "animation/SkeletonBounds.h"
#include "../core/GGPKExplorerSettings.h"

namespace GGPKExp {
class GGPKManager;

// The model view: a .sm (with its .smd, textures and skeleton), a bare .smd,
// a skeleton-only .ast, or the main skin mesh and skeleton an .aoc names.
class MeshViewer : public IViewer {
public:
    ~MeshViewer() override = default;

    void Initialize(ID3D11Device* device);
    void SetManager(GGPKManager* mgr) { m_mgr = mgr; }

    // IViewer interface. `path` is the file's full GGPK path (its companion
    // files are looked up next to it). Loading the open file's path again (an
    // .aoc saved in the Text tab) keeps the camera, render mode and clip.
    void LoadData(std::vector<uint8_t> data, const std::string& path) override;
    void Clear() override;
    void Render() override;

    // Plugin owns the shared view settings (Task 7); MeshViewer falls back
    // to a local static if not wired. Survives Clear() / model switches.
    void SetViewSettings(GGPKExp::MeshViewSettings* state) {
        m_viewSettings = state;
    }

    // Task 6: animation side panel wiring.
    // Plugin owns the shared panel state (Task 8); MeshViewer falls back
    // to a local static if not wired.
    void SetAnimPanelState(GGPKExp::AnimationPanelState* state) {
        m_animPanelState = state;
    }

    // Pointer to GGPKExplorerUiSettings::playbackSpeed (owned by plugin).
    // When non-null, viewer applies *ptr on every model load so the user's
    // chosen speed survives model switches (bug 2026-04-15 fix #1).
    void SetPlaybackSpeedPtr(float* ptr) { m_playbackSpeed = ptr; }

    // Apply a specific speed value to the underlying AnimationPlayer.
    // Also updates the default-speed used across future Reset() calls.
    void SetPlaybackSpeed(float s) { m_animPlayer.SetSpeed(s); }

    bool ConsumeUiSettingsDirty() {
        const bool was = m_animPanelSettingsDirty;
        m_animPanelSettingsDirty = false;
        return was;
    }

private:
    // Async loading state machine (driven once per Render, on the GUI thread)
    enum class MeshLoadState { Idle, Preparing, ReadingFiles, UploadingGpu, Ready, Error };

    // Sub-components
    MeshRenderer m_renderer;
    MeshCamera m_camera;
    MeshMaterial m_material;

    // Parsed data
    Parsers::SmdMesh m_mesh;
    Parsers::AstSkeleton m_skeleton;
    Parsers::ResolvedSkeleton m_resolvedSkeleton;
    GGPKExp::Animation::SkeletonBounds m_staticLocalBbox;
    Parsers::SmData m_smData;

    // Phase 4.3 Sprint 1 — mesh skinning palette.
    std::vector<std::array<float, 16>> m_bindInverseWorld;     // cached inverse of bind worldTransform
    std::vector<std::array<float, 16>> m_skinningMatrices;     // per-frame palette (bindInverse * currentWorld)
    bool m_skinningTruncationWarned = false;                    // one-time gate for >128-bone warning

    // V01/V02 local-bone -> global-skeleton-bone remap (Fix #1).
    // Indexed by local BLENDINDICES value (0..m_mesh.boneCount-1).
    // -1 = no global match (local name not found in skeleton) -> slot gets identity.
    // Empty for DOLm/V3+ meshes (their indices are already global).
    std::vector<int> m_boneRemap;

    // .aoc: the skin mesh (.sm) and skeleton (.ast) it names. The model shown
    // is that .sm with the skeleton of the .aoc's ClientAnimationController.
    bool        m_isAoc = false;
    bool        m_aocNoMesh = false;   // the .aoc names no skin mesh
    std::string m_aocSkinPath;
    std::string m_aocSkeletonPath;
    // The resolver's warnings: a parent that is missing or does not parse
    // ends the chain, and what it named (the skin, the skeleton) is absent.
    std::vector<std::string> m_aocWarnings;

    // Mesh state
    bool m_isSmFile = false;
    std::string m_path;       // full GGPK path of the file opened
    std::string m_filename;   // its file name (shown above the view)

    // Per-mesh rig detection (2026-04-15). When a <basename>.ast companion file
    // exists next to the .sm (3,739 / 27,566 files — chests, corpses, props,
    // attachments, effects), the mesh has its OWN local skeleton rather than
    // sharing rig.ast with animated characters. Driven by the per-mesh rig
    // load in DoMainThreadRead; shown in the Info tab.
    bool m_isStaticPropMesh = false;
    std::string m_rigSourceName;   // e.g. "bittersoncorpse01.ast" or "rig.ast" (basename only)
    std::string m_skeletonError;   // why the skeleton found did not load (read / parse), for Info

    // Display toggles
    int m_renderMode = 0;  // 0=Solid, 1=Wire, 2=Solid+Wire

    // Computed world-space bone positions for visualization
    struct BoneVizData {
        float worldPos[3] = {};
        uint8_t parentIndex = 0xFF;
        std::string name;
    };
    std::vector<BoneVizData> m_boneViz;

    // Phase 4.2: animation playback.
    GGPKExp::Animation::AnimationPlayer m_animPlayer;

    // Task 6: shared animation panel state (owned by plugin, nullable).
    GGPKExp::AnimationPanelState* m_animPanelState = nullptr;

    // Pointer to plugin-owned playback speed (nullable); when set, applied on
    // every model load so JSON-persisted speed survives Clear()/model switches.
    float* m_playbackSpeed = nullptr;

    // View settings shared by all MeshViewer sessions (plugin-owned, nullable).
    GGPKExp::MeshViewSettings*    m_viewSettings = nullptr;

    // Dirty flag covers BOTH animPanelState and viewSettings changes —
    // ContentPanel::ConsumeUiSettingsDirty debounces the save.
    bool                          m_animPanelSettingsDirty = false;

    // Context (non-owning, lifetime guaranteed by GGPKExplorer)
    GGPKManager* m_mgr = nullptr;

    // Async loading state
    MeshLoadState m_loadState = MeshLoadState::Idle;
    bool m_prepareFrameDrawn = false;

    struct MeshPendingInput {
        std::vector<uint8_t> inputData;      // original .sm/.smd/.ast bytes from LoadData
        std::string path;
        std::string ext;                     // lowercased extension
    };
    MeshPendingInput m_pending;

    // The view a reload of the same file keeps: LoadData captures it from the
    // model shown, the next successful upload applies it, Clear() drops it.
    struct KeptView {
        bool        valid = false;
        MeshCamera  camera;
        int         renderMode = 0;
        std::string clip;            // the selected clip's name
        float       time = 0.0f;
        bool        playing = false;
    };
    KeptView m_keptView;
    void ApplyKeptView();

    // Byte buffers read in ReadingFiles phase, parsed+uploaded in UploadingGpu
    std::vector<uint8_t> m_smdBytes;
    std::vector<uint8_t> m_rigAstBytes;

    // Spinner rendering helper
    void DrawMeshLoadingSpinner(const char* status);

    // State transitions (main thread only)
    void DoMainThreadRead();
    void DoMainThreadUpload();

    void Render3DView();
    void RenderInfoTab();
    // Up to `maxShown` of m_aocWarnings as warning lines (kAllWarnings: all).
    static constexpr std::size_t kAllWarnings = static_cast<std::size_t>(-1);
    void RenderAocWarnings(std::size_t maxShown) const;
    std::string BuildInfoText() const;   // the Info tab's lines (also what Copy copies)
    void InitResolvedSkeleton();              // one-time — BuildResolvedSkeleton + initial bone-viz
    void RefreshBoneVizFromWorldTransforms(); // per-frame — reads bones[].worldTransform

    // .aoc: resolves the extends chain and replaces m_pending with the skin
    // mesh's .sm. False with m_error set when the .aoc cannot be read.
    bool ResolveAocModel();
    // Ends the load with one error line naming the file and the cause (an
    // .aoc's line says it is its skin mesh that cannot be shown).
    void FailMesh(const std::string& why);
    void TickLoadState();
};

} // namespace GGPKExp
