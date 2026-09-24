#pragma once

#include "AnimationTypes.h"
#include <vector>
#include <cstddef>

namespace GGPKExp { namespace Animation {

// Stateless-per-call pose sampler. Consumer supplies per-track kf-index
// cache (reset on scrub-backward or anim change).
//
// Row-vector convention: M = S * R * T; translate stored in m[12..14].
// Matches AstBone::transform layout so that AstSkeletonBuilder's world
// matmul (world = local * parent.world) continues to work unchanged.
class PoseInterpolator {
public:
    // Per-track kf-index cache entry. Advances forward as time progresses.
    // Reset on scrub-backward or animation change (caller's responsibility).
    struct TrackIndexCache {
        std::size_t lastT = 0;
        std::size_t lastR = 0;
        std::size_t lastS = 0;
    };

    // Sample clip at time t; write per-bone local 4x4 into poses[track.boneIndex].
    // poses[] must be pre-sized to at least max(boneIndex)+1; entries not
    // covered by a track are left unchanged (caller pre-seeds them from
    // bind pose).
    //
    // cache[] is one entry per clip.tracks[i]; advances forward when t
    // increases, reset externally on scrub backward.
    void Sample(const AnimationClip& clip,
                float t,
                std::vector<BonePose>& poses,
                std::vector<TrackIndexCache>& cache) const;

    // Slerp with shortest-path + near-parallel lerp fallback.
    static void SlerpQuat(const float a[4], const float b[4],
                          float u, float out[4]);

    // Compose row-major 4x4 from TRS. m[12..14] = translation.
    static void ComposeTRS(const float T[3], const float R[4], const float S[3],
                           float m[16]);

private:
    // Binary-search entry point for kf-index cache initialisation.
    template <typename KeyArr>
    static std::size_t FindKeyIndexForward(const KeyArr& keys,
                                           float t,
                                           std::size_t hint);
};

}} // namespace GGPKExp::Animation
