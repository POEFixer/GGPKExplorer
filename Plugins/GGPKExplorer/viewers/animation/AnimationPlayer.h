#pragma once

#include "AnimationTypes.h"
#include "KeyframeDecoder.h"
#include "PoseInterpolator.h"
#include "PoseComposer.h"
#include "../../core/parsers/AstParser.h"
#include "../../core/parsers/AstSkeletonBuilder.h"
#include "../../core/parsers/AocTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace GGPKExp { namespace Animation {

// Stateful animation playback facade.
//
// Lifecycle:
//   AttachSkeleton(&skel)         - stash pointer; copy anim names for UI.
//                                   Auto-selects animation 0 on layer 0 and
//                                   sets layer 0 to playing.
//   SetLayerAnimation(i, idx)     - lazy Oodle decode + typed clip parse
//                                   (50-150 ms first time, ~free after).
//   Play()/Pause()/SeekSeconds(t) - legacy UI controls (map to layer 0).
//   AddLayer()/RemoveLayer(i)     - manage additive layers (1..kMaxLayers-1).
//   Tick(dt) + ApplyFrame(skel)   - per-frame from viewer's Render3DView.
//
// Layers:
//   Layer 0 is the base track (uses clip.playbackType for loop policy).
//   Layers 1..N are additive (per-layer loop bool overrides playbackType).
//
// Thread-safety: main thread only.
class AnimationPlayer {
public:
    void AttachSkeleton(const Parsers::AstSkeleton* skel);

    // Playback controls (operate on layer 0 for legacy single-track users).
    void Play();
    void Pause();
    void SeekSeconds(float t);                        // auto-pauses

    // Per-frame driver methods.
    void Tick(float dtSeconds);                       // advances every active layer
    void ApplyFrame(Parsers::ResolvedSkeleton& out);  // not const - updates caches

    // Test-only: inject a synthetic clip into slot 0 for playback-policy tests.
    void InjectClipForTesting(AnimationClip clip);

    // Layer management (Phase 4.2 scope C - multi-track composition).
    std::size_t       LayerCount() const                                 { return m_activeLayerCount; }
    AnimationLayer&   Layer(std::size_t i)                               { return m_layers[i]; }
    const AnimationLayer& Layer(std::size_t i) const                     { return m_layers[i]; }
    bool              AddLayer();                                        // appends additive layer, returns false if full
    void              RemoveLayer(std::size_t i);                        // removes layer i >= 1 (base not removable)
    void              SetLayerAnimation(std::size_t i, int animIdx);     // triggers lazy parse for that anim

    // Bone-group masking data (used by PoseComposer). Caller supplies from .aoc
    // (ClientAnimCtrl::boneGroups); pass nullptr to clear.
    void SetBoneGroups(const std::vector<Parsers::BoneGroup>* groups) {
        m_boneGroups = groups ? *groups : std::vector<Parsers::BoneGroup>{};
    }

    // Legacy single-track convenience (maps to Layer(0)).
    void SelectAnimation(std::size_t animIndex) { SetLayerAnimation(0, static_cast<int>(animIndex)); }
    std::size_t SelectedAnimIndex() const {
        const int idx = m_layers[0].animIndex;
        return (idx < 0) ? static_cast<std::size_t>(-1) : static_cast<std::size_t>(idx);
    }

    // UI queries (all operate on layer 0 for legacy callers).
    bool        IsPlaying()         const { return m_layers[0].playing; }
    float       CurrentTime()       const { return m_layers[0].timeSeconds; }
    float       CurrentDuration()   const;
    uint32_t    CurrentFrame()      const;
    float       Speed()             const { return m_layers[0].speed; }
    // Also stores s as the post-Reset default so model changes don't revert
    // the user's chosen playback speed (bug 2026-04-15 fix #1).
    void        SetSpeed(float s)         { m_layers[0].speed = s; m_defaultSpeed = s; }

    /// True if any animation layer is currently playing (`playing` flag set).
    bool IsAnyLayerPlaying() const;

    /// Current time on the base layer (layer 0) in seconds. Returns 0.0f if the
    /// player has no layers initialized. Used to detect "user has seeked past
    /// bind-pose-time" so paused-but-non-zero times still apply the pose.
    float BaseLayerTime() const;

    const std::vector<std::string>&   AnimNames()   const { return m_animNames; }
    const std::vector<AnimationClip>& Clips()       const { return m_clips; }
    const std::vector<std::string>&   GetWarnings() const { return m_warnings; }

    // Append a one-off warning to the public warnings list (used by
    // outside collaborators like MeshViewer to surface viewer-side
    // issues such as skinning truncation through the same TimelineUi badge).
    void PushWarning(std::string warning) { m_warnings.push_back(std::move(warning)); }

    void Reset();   // clears all state (for viewer::Clear())

private:
    const Parsers::AstSkeleton* m_skeleton = nullptr;
    KeyframeDecoder             m_decoder;
    PoseInterpolator            m_interpolator;
    PoseComposer                m_composer;
    std::vector<AnimationClip>  m_clips;              // lazy-parsed, indexed by animIndex
    std::vector<bool>           m_clipParsed;
    std::vector<std::string>    m_animNames;
    std::vector<std::string>    m_warnings;

    // Layer stack (layers[0] = base, layers[1..] = additive). Max size = kMaxLayers.
    std::array<AnimationLayer, kMaxLayers> m_layers{};
    std::size_t                            m_activeLayerCount = 1;  // starts with just base

    // Bone-group snapshot (from .aoc) used for layer-mask evaluation inside ApplyFrame.
    std::vector<Parsers::BoneGroup> m_boneGroups;

    // Speed applied to layer 0 after Reset(). Initialized to 1.0f (same as
    // AnimationLayer::speed default), overwritten by SetSpeed() so the
    // user's combo selection survives model reload.
    float m_defaultSpeed = 1.0f;

    // Per-layer scratch for sampling (pre-allocated, reused).
    mutable std::array<std::vector<BonePose>, kMaxLayers>                          m_layerPoses;
    mutable std::array<std::vector<PoseInterpolator::TrackIndexCache>, kMaxLayers> m_kfCaches;
    mutable std::vector<BonePose> m_baseScratch;       // seeded from bind pose
    mutable std::vector<BonePose> m_restScratch;       // bind pose reference

    void RecomposeWorldTransforms(Parsers::ResolvedSkeleton& skel) const;
};

}} // namespace GGPKExp::Animation
