#pragma once

#include "AnimationTypes.h"
#include "KeyframeDecoder.h"
#include "PoseInterpolator.h"
#include "../../core/parsers/AstParser.h"
#include "../../core/parsers/AstSkeletonBuilder.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace GGPKExp { namespace Animation {

// Single-track animation playback.
//
// Lifecycle:
//   AttachSkeleton(&skel)         - stash pointer; copy anim names for UI and
//                                   select the default clip ("idle", else the
//                                   first) without playing it.
//   SelectAnimation(idx)          - lazy Oodle decode + typed clip parse
//                                   (50-150 ms first time, ~free after).
//   Play()/Pause()/SeekSeconds(t) - UI controls.
//   Tick(dt) + ApplyFrame(skel)   - per frame, from the viewer.
//
// The loop policy is the clip's playbackType: 'l' loops, 'o' plays once and
// pauses at the end, anything else clamps to [0, duration].
//
// Thread-safety: main thread only.
class AnimationPlayer {
public:
    void AttachSkeleton(const Parsers::AstSkeleton* skel);

    void SelectAnimation(std::size_t animIndex);
    std::size_t SelectedAnimIndex() const {
        return (m_animIndex < 0) ? static_cast<std::size_t>(-1)
                                 : static_cast<std::size_t>(m_animIndex);
    }

    void Play();
    void Pause() { m_playing = false; }
    void SeekSeconds(float t);                        // auto-pauses

    void Tick(float dtSeconds);
    void ApplyFrame(Parsers::ResolvedSkeleton& out);  // not const - updates caches

    // Test-only: inject a synthetic clip into slot 0 for playback-policy tests.
    void InjectClipForTesting(AnimationClip clip);

    bool        IsPlaying()   const { return m_playing; }
    float       CurrentTime() const { return m_time; }
    uint32_t    CurrentFrame() const;
    float       Speed()       const { return m_speed; }
    // Also stores s as the post-Reset default so model changes don't revert
    // the user's chosen playback speed (bug 2026-04-15 fix #1).
    void        SetSpeed(float s)   { m_speed = s; m_defaultSpeed = s; }

    const std::vector<std::string>&   AnimNames()   const { return m_animNames; }
    const std::vector<AnimationClip>& Clips()       const { return m_clips; }
    const std::vector<std::string>&   GetWarnings() const { return m_warnings; }

    // Append a one-off warning to the public warnings list (used by
    // MeshViewer to surface viewer-side issues such as skinning truncation
    // through the side panel's warnings badge).
    void PushWarning(std::string warning) { m_warnings.push_back(std::move(warning)); }

    void Reset();   // clears all state (for viewer::Clear())

private:
    const Parsers::AstSkeleton* m_skeleton = nullptr;
    KeyframeDecoder             m_decoder;
    PoseInterpolator            m_interpolator;
    std::vector<AnimationClip>  m_clips;              // lazy-parsed, indexed by animIndex
    std::vector<bool>           m_clipParsed;
    std::vector<std::string>    m_animNames;
    std::vector<std::string>    m_warnings;

    int   m_animIndex = -1;     // index into AstSkeleton::animations; -1 = none
    float m_time      = 0.0f;   // playhead in seconds
    bool  m_playing   = false;
    float m_speed     = 1.0f;
    // Speed applied after Reset(), overwritten by SetSpeed() so the user's
    // combo selection survives model reload.
    float m_defaultSpeed = 1.0f;

    // Sampling scratch (reused across frames).
    std::vector<BonePose>                          m_pose;
    std::vector<PoseInterpolator::TrackIndexCache> m_kfCache;

    void RecomposeWorldTransforms(Parsers::ResolvedSkeleton& skel) const;
};

}} // namespace GGPKExp::Animation
