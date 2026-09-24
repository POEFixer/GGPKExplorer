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
#include "../core/parsers/SmdHeaderParser.h"
#include "../core/parsers/AstParser.h"
#include "../core/parsers/AstSkeletonBuilder.h"
#include "../core/parsers/SmParser.h"
#include "../core/parsers/AocTypes.h"
#include "../core/parsers/AocResolver.h"
#include "mesh/MeshCamera.h"
#include "mesh/MeshRenderer.h"
#include "mesh/MeshMaterial.h"
#include "animation/AnimationPlayer.h"
#include "animation/SkeletonBounds.h"
#include "animation/TimelineUi.h"
#include "../core/GGPKExplorerSettings.h"

namespace GGPKExp {
class GGPKManager;
class VirtualTree;

class MeshViewer : public IViewer {
public:
    ~MeshViewer() override = default;

    void Initialize(ID3D11Device* device);
    void SetContext(GGPKManager* mgr, VirtualTree* tree);

    // IViewer interface
    void LoadData(std::vector<uint8_t> data, const std::string& path) override;
    void Clear() override;
    void Render() override;

    // Extended load (with full path and GGPK context) — called by ContentPanel.
    // Now a thin backward-compat wrapper around LoadData's async state machine.
    bool LoadMesh(const std::vector<uint8_t>& data, const std::string& filename,
                  const std::string& fullPath, GGPKManager* mgr, VirtualTree* tree);

    // Async loading state machine
    enum class MeshLoadState { Idle, Preparing, ReadingFiles, UploadingGpu, Ready, Error };

    void TickLoadState();   // Called each Render — drives state machine

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
    float GetPlaybackSpeed() const { return m_animPlayer.Speed(); }

private:
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

    // Dev tool: force skinning palette to identity regardless of animation/remap.
    // Useful for isolating skinning bugs vs vertex-decode bugs in 3D preview.
    bool m_forceBindPose = false;

    // --- .aoc overlay state (Sprint 3) ---
    Parsers::AocDocument        m_aocDocument;
    Parsers::AocResolverCache   m_aocResolverCache;
    std::vector<std::string>    m_aocWarnings;
    std::vector<uint8_t>        m_pendingAocBytes;
    std::string                 m_pendingAocPath;      // auto-loaded path (or "" if none)
    std::string                 m_manualAocPathInput;  // buffer for manual override dialog
    bool                        m_showSockets = false;
    bool                        m_showSocketNames = false;
    bool                        m_showSocketParentLines = false;
    bool                        m_manualAocDialogOpen = false;

    // Mesh state
    bool m_isSmFile = false;
    bool m_hasSkinning = false;
    std::string m_filename;

    // Per-mesh rig detection (2026-04-15). When a <basename>.ast companion file
    // exists next to the .sm (3,739 / 27,566 files — chests, corpses, props,
    // attachments, effects), the mesh has its OWN local skeleton rather than
    // sharing rig.ast with animated characters. Driven by the per-mesh rig
    // load in DoMainThreadRead; used by RenderInfoTab to surface a banner and
    // (potentially) by the animation gate if Play on static props misbehaves.
    bool m_isStaticPropMesh = false;
    std::string m_rigSourceName;   // e.g. "bittersoncorpse01.ast" or "rig.ast" (basename only)

    // Display toggles
    int m_renderMode = 0;  // 0=Solid, 1=Wire, 2=Solid+Wire
    int m_activeTab = 0;   // 0=3D View, 1=Info

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
    VirtualTree* m_tree = nullptr;

    // Async loading state
    MeshLoadState m_loadState = MeshLoadState::Idle;
    bool m_prepareFrameDrawn = false;

    struct MeshPendingInput {
        std::vector<uint8_t> inputData;      // original .sm/.smd/.ast bytes from LoadData
        std::string path;
        std::string ext;                     // lowercased extension
    };
    MeshPendingInput m_pending;

    // Byte buffers read in ReadingFiles phase, parsed+uploaded in UploadingGpu
    std::vector<uint8_t> m_smdBytes;
    std::vector<uint8_t> m_smdHeaderBytes;
    std::vector<uint8_t> m_rigAstBytes;

    // Spinner rendering helper
    void DrawMeshLoadingSpinner(const char* status);

    // State transitions (main thread only)
    void DoMainThreadRead();
    void DoMainThreadUpload();

    void Render3DView();
    void RenderInfoTab();
    std::string BuildInfoText() const;
    void InitResolvedSkeleton();              // one-time — BuildResolvedSkeleton + initial bone-viz
    void RefreshBoneVizFromWorldTransforms(); // per-frame — reads bones[].worldTransform
    static float HalfToFloat(uint16_t h);

    // Sprint 3: .aoc overlay helpers
    void TryAutoDiscoverAoc(const std::string& meshOrAstPath);
    void ApplyPendingAoc();
    void RenderManualAocDialog();
    static std::string StripExtensionForAoc(const std::string& path);
};

} // namespace GGPKExp
