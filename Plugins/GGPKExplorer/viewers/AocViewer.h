#pragma once

#include "IViewer.h"
#include "../core/parsers/AocTypes.h"
#include "../core/parsers/AocResolver.h"
#include "../core/parsers/AstSkeletonBuilder.h"
#include "../core/parsers/AstParser.h"
#include "../core/GGPKExplorerSettings.h"
#include "mesh/MeshCamera.h"
#include "animation/AnimationPlayer.h"
#include "animation/SkeletonBounds.h"
#include "animation/TimelineUi.h"
#include "aoc/AocSceneGraph.h"
#include "aoc/AocLightSystem.h"
#include "aoc/AocAudioSystem.h"
#include "aoc/AocParticleSystem.h"
#include "aoc/AocTransitionState.h"
#include "aoc/AocPersistedAttachments.h"
#include "aoc/AocRecentAttachments.h"

#include <array>
#include <cstdint>
#include <d3d11.h>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GGPKExp {

class GGPKManager;
class VirtualTree;
class MeshRenderer;
class MeshMaterial;
class AttachBrowseModal;
class DatSchema;

// --------------------------------------------------------------------------
// Phase 1 additions (2026-04-15) — AOC Composition Viewer redesign
// --------------------------------------------------------------------------

enum class AocGridAnchor : int {
    MeshBounds   = 0,   // Use main mesh vertex AABB (default, fixes float/sink bug)
    Skeleton     = 1,   // Legacy behavior: skeleton bone AABB
    WorldOrigin  = 2,   // Always Z=0 (Z-down world convention)
};

// Phase 4 — callback return type for ContentPanel selection forwarding.
struct AocContentSelection {
    std::string          path;    // empty = no valid previous selection
    std::vector<uint8_t> bytes;
};

struct AocViewToggles {
    bool showSockets        = true;
    bool showSocketLabels   = false;   // Off by default for 60+ socket files
    bool showLights         = true;    // Phase 2 (no-op in Phase 1)
    bool showLightLabels    = false;   // Off by default; enabled for debugging
    bool showParticles      = true;    // Phase 3
    bool showParticleLabels = false;   // Off by default; noisy on emitter-heavy .aoc files
    bool showAudioMarkers   = true;    // Phase 2
    bool showGrid           = true;
    bool showParentLines    = true;    // Socket→parent bone lines (existing behavior)
    int  gridAnchor         = static_cast<int>(AocGridAnchor::MeshBounds);
    bool wireframe          = false;
    bool showAttachments   = true;     // Phase 4: render child meshes at sockets
};

enum class AocTab : int {
    Preview    = 0,
    Overview   = 1,
    Sockets    = 2,
    Animations = 3,
    Diagnostic = 4,
};

// --------------------------------------------------------------------------
// AocViewerState — shared state across split tab files
// --------------------------------------------------------------------------

struct AocViewerState {
    // File metadata (set by LoadData)
    std::string           m_filePath;
    std::vector<uint8_t>  m_rawBytes;       // Original UTF-16LE + BOM
    std::string           m_sha256Hex;      // Computed in LoadData

    // Parsed data (rebuilt on Raw Text save)
    Parsers::AocDocument         m_parsedDoc;
    Parsers::AocResolverCache    m_resolveCache;
    Parsers::AstSkeleton         m_astSkel;
    Parsers::ResolvedSkeleton    m_resolvedSkel;
    Animation::SkeletonBounds    m_staticLocalBbox;
    std::vector<std::string>     m_parseWarnings;
    std::vector<std::string>     m_parseErrors;

    // Bind-pose inverse of s.m_resolvedSkel bones — computed once in DoParsing
    // (before any ApplyFrame mutates worldTransform). Used for GPU skinning palette.
    std::vector<std::array<float, 16>> m_aocBindInverseWorld;

    // V01/V02 local-bone → global-skeleton-bone remap. Built in DoParsing.
    // Indexed by local BLENDINDICES value (0..m_sceneGraph.Mesh().boneCount-1).
    // -1 = no global match (local name not found in skeleton) → slot gets identity.
    // Empty for DOLm/V3+ meshes (their indices are already global).
    std::vector<int> m_boneRemap;

    // Rendering subsystems (Phase 1)
    Aoc::AocSceneGraph           m_sceneGraph;
    Aoc::AocLightSystem          m_lights;
    MeshCamera                   m_camera;
    Animation::AnimationPlayer   m_animPlayer;

    // Phase 2 — Audio + active state name + UI ephemera
    std::unique_ptr<Aoc::AocAudioSystem>       m_audio;

    // Timeline debounce map: filename → last-trigger time (seconds).
    // Used by the audio hook (Task 12) to suppress <50 ms re-triggers.
    std::unordered_map<std::string, float>     m_audioLastTriggered;

    // Phase 3 — ParticleEffects
    Aoc::AocParticleSystem       m_particles;
    std::string                  m_particlesLastClip;   // last clip name seen by trigger loop

    // Phase 4 — Transitionable + attachments
    Aoc::AocTransitionState      m_transitions;
    std::string                  m_transitionOverrideOt;  // manual "Browse .ot..." selection
    bool                         m_transitionStateDirty = false; // hotkey/dropdown → resolve clip next frame

    // T8 / B1 debounce: tracks which socket's transform popup is currently open.
    // Empty = no popup open. On open->close transition, if that row's
    // pendingTransformSave is true, fire one Save and clear the flag.
    std::string                  m_transformPopupOpenRow;

    // Phase 5.1 — Persistent attachments + state
    Aoc::AocPersistedAttachments m_persisted;
    std::string                  m_pluginDir;            // set via SetPluginDir for JSON path

    // Phase 5.1.5a — Global LRU of recently-attached source paths.
    // Loaded on SetPluginDir, persists across files + sessions.
    Aoc::AocRecentAttachments    m_recent;

    // Toast queue for UI notifications (attach/detach results, browse errors,
    // missing audio, Recent-entry errors). Each entry has text + fade start
    // time. Rendered by 3D Preview tab as a bottom-right overlay.
    // isError controls render colour (true = red, false = green).
    struct UiToast {
        std::string text;
        float       startTime;
        bool        isError = true;   // false → green render
    };
    std::vector<UiToast>                       m_uiToasts;

    float                                      m_lastAnimTimeForAudio = 0.0f;

    // Persisted across Raw-Text save/re-parse so user's dropdown selection
    // survives model reload.
    std::string                                m_activeLightStateName;

    // Per-frame scratch for Animation::BuildSkinningPalette. Held here so
    // the underlying allocation survives frame boundaries (amortises
    // resize cost to a single alloc per skeleton size). Unused until
    // Task 6 wires real bind-inverse matrices.
    std::vector<std::array<float, 16>> m_paletteScratch;

    // Phase 5.2 — per-child skinning palette scratch, reused across all
    // skinned children each frame to avoid per-frame allocation.
    std::vector<std::array<float, 16>> m_childPaletteScratch;

    // User toggles (persisted via GGPKExplorerUiSettings in Phase 1.5)
    AocViewToggles        m_view;

    std::string           m_lastSaveStatus; // "Saved at HH:MM:SS" or error

    // Animation panel shared state (owned by plugin, nullable)
    AnimationPanelState*  m_animPanelState  = nullptr;
    bool                  m_animPanelSettingsDirty = false;

    // Context (non-owning)
    GGPKManager*          m_mgr     = nullptr;
    VirtualTree*          m_tree    = nullptr;
    ID3D11Device*         m_device  = nullptr;

    // Save callback — plugin wires this to GGPKManager::WriteFile
    std::function<void(const std::vector<uint8_t>&)> m_onSave;
};

// --------------------------------------------------------------------------
// AocViewer — thin dispatcher
// --------------------------------------------------------------------------

class AocViewer : public IViewer {
public:
    AocViewer();
    ~AocViewer() override;

    void Initialize(ID3D11Device* device);
    void SetContext(GGPKManager* mgr, VirtualTree* tree);

    // Phase 5.1.5b — DAT schema pointer forwarded to AttachBrowseModal.
    // Called by ContentPanel when schema becomes available (may be before or
    // after the modal is constructed). Safe to call multiple times.
    void SetDatSchema(DatSchema* schema);

    // IViewer interface
    void LoadData(std::vector<uint8_t> data, const std::string& path) override;
    void Clear() override;
    void Render() override;

    // Animation wiring (unchanged API)
    void SetAnimPanelState(AnimationPanelState* state) { m_state.m_animPanelState = state; }
    void SetPlaybackSpeedPtr(float* ptr) { m_playbackSpeed = ptr; }
    void SetPlaybackSpeed(float s) { m_state.m_animPlayer.SetSpeed(s); }
    float GetPlaybackSpeed() const { return m_state.m_animPlayer.Speed(); }
    bool ConsumeUiSettingsDirty() {
        const bool was = m_state.m_animPanelSettingsDirty;
        m_state.m_animPanelSettingsDirty = false;
        return was;
    }

    // Raw Text save callback (called by ContentPanel to wire GGPKManager::WriteFile)
    void SetOnSave(std::function<void(const std::vector<uint8_t>&)> cb);

    // Phase 4 — Selection provider for "Attach Selected Mesh" context menu.
    void SetSelectionProvider(std::function<AocContentSelection()> cb);

    // Phase 5.1 — Plugin directory (for JSON sidecar location)
    void SetPluginDir(const std::string& dir) {
        m_state.m_pluginDir = dir;
        m_state.m_recent.Load(dir);
    }

private:
    enum class LoadState { Idle, Preparing, ReadingChain, Parsing, Ready, Error };

    AocViewerState m_state;
    LoadState      m_loadState = LoadState::Idle;
    AocTab         m_activeTab = AocTab::Preview;
    float*         m_playbackSpeed = nullptr;

    // Render subsystem (heavy — owned via unique_ptr to avoid full include in header)
    std::unique_ptr<MeshRenderer> m_renderer;
    std::unique_ptr<MeshMaterial> m_material;

    // Tracks which scene-graph mesh has been uploaded into MeshRenderer's
    // GPU buffers. Opaque address is enough: whenever the scene graph
    // reloads, its `Mesh()` pointer changes, so we re-upload.
    const void* m_uploadedMeshAddr = nullptr;

    // Set to true when the most recent GPU upload (CreateMeshBuffers /
    // material load) failed. While true, the 3D preview skips RenderScene
    // so the pipeline doesn't draw with stale or missing buffers. Reset
    // on successful upload, Clear(), and RebuildScene().
    bool m_uploadFailed = false;

    // Phase 4 — set by child render pass when it clobbered MeshRenderer's
    // VB/IB with a child mesh. Next frame, main mesh re-uploads VB/IB only
    // (materials stay cached).
    bool m_mainBuffersDirtyFromChildRender = false;

    // Phase 4 — per-child material cache keyed by "sourcePath|socketName".
    // Cleared on Clear() (cross-file load) to release D3D11 resources.
    std::unordered_map<std::string, std::unique_ptr<MeshMaterial>> m_childMaterials;

    // Phase 4 — injected by plugin at Initialize
    std::function<AocContentSelection()> m_selectionProvider;

    // Phase 5.1.5a — modal for browsing + attaching .sm/.aoc from GGPK
    std::unique_ptr<AttachBrowseModal> m_browseModal;

    // Cached DatSchema pointer until m_browseModal exists.
    DatSchema* m_datSchemaCached = nullptr;

    // Lifecycle helpers
    void TickLoadState();
    void DoReadingChain();
    void DoParsing();
    void RebuildScene();                  // Called after LoadData + after Raw Text save

    // Tab renderers — impl in split .cpp files
    void RenderPreviewTab();              // AocViewer_3DPreview.cpp
    void RenderOverviewTab();             // AocViewer_Info.cpp
    void RenderSocketsTab();              // AocViewer_Sockets.cpp
    void RenderAnimationsTab();           // AocViewer_Animations.cpp
    void RenderDiagnosticTab();           // AocViewer_Diagnostic.cpp

    // Shared helpers used across tabs
    // T12 — Diagnostic tab sub-sections. Full builder concatenates the other
    // three plus any remaining Phase 2/3 content (lights/sound/particles).
    void BuildDiagnosticMarkdown_Summary(std::string& outMd) const;
    void BuildDiagnosticMarkdown_Render(std::string& outMd) const;
    void BuildDiagnosticMarkdown_Attachments(std::string& outMd) const;
    void BuildDiagnosticMarkdown_Full(std::string& outMd) const;
    float ComputeGridZ() const;

    // Phase 5.1.5a — single attach entry point used by all three attach paths
    // (Selected / Browse / Recent). Handles toast + recent bump + persistence.
    void DoAttachCommon(const std::string& socket,
                        const std::string& sourcePath,
                        const std::vector<uint8_t>& sourceBytes);
};

} // namespace GGPKExp
