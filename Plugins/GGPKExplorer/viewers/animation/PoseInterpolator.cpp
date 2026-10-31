#include "PoseInterpolator.h"

#include <cmath>
#include <cstring>
#include <algorithm>

namespace GGPKExp { namespace Animation {

void PoseInterpolator::SlerpQuat(const float a[4], const float b[4],
                                 float u, float out[4]) {
    float dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    float sign = (dot < 0.0f) ? -1.0f : 1.0f;
    float bs[4] = { sign*b[0], sign*b[1], sign*b[2], sign*b[3] };
    dot = std::fabs(dot);
    float s0, s1;
    if (dot > 0.9995f) {
        s0 = 1.0f - u;
        s1 = u;
    } else {
        float theta = std::acos(dot);
        float sinT  = std::sin(theta);
        s0 = std::sin((1.0f - u) * theta) / sinT;
        s1 = std::sin(u * theta) / sinT;
    }
    for (int i = 0; i < 4; ++i) out[i] = s0 * a[i] + s1 * bs[i];
    float n = std::sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2] + out[3]*out[3]);
    if (n > 1e-6f) for (int i = 0; i < 4; ++i) out[i] /= n;
}

void PoseInterpolator::ComposeTRS(const float T[3], const float R[4], const float S[3],
                                  float m[16]) {
    const float qx=R[0], qy=R[1], qz=R[2], qw=R[3];
    const float xx=qx*qx, yy=qy*qy, zz=qz*qz;
    const float xy=qx*qy, xz=qx*qz, yz=qy*qz;
    const float wx=qw*qx, wy=qw*qy, wz=qw*qz;
    m[ 0] = (1.0f - 2.0f*(yy + zz)) * S[0];
    m[ 1] = (2.0f*(xy + wz))        * S[0];
    m[ 2] = (2.0f*(xz - wy))        * S[0];
    m[ 3] = 0.0f;
    m[ 4] = (2.0f*(xy - wz))        * S[1];
    m[ 5] = (1.0f - 2.0f*(xx + zz)) * S[1];
    m[ 6] = (2.0f*(yz + wx))        * S[1];
    m[ 7] = 0.0f;
    m[ 8] = (2.0f*(xz + wy))        * S[2];
    m[ 9] = (2.0f*(yz - wx))        * S[2];
    m[10] = (1.0f - 2.0f*(xx + yy)) * S[2];
    m[11] = 0.0f;
    m[12] = T[0];
    m[13] = T[1];
    m[14] = T[2];
    m[15] = 1.0f;
}

template <typename KeyArr>
std::size_t PoseInterpolator::FindKeyIndexForward(const KeyArr& keys,
                                                  float t,
                                                  std::size_t hint) {
    if (keys.empty()) return 0;
    if (hint >= keys.size()) hint = 0;

    // Advance hint forward while next key is still before t.
    while (hint + 1 < keys.size() && keys[hint + 1].time <= t) ++hint;
    // If t is before hint, walk backward (covers scrub).
    while (hint > 0 && keys[hint].time > t) --hint;
    return hint;
}

namespace {

inline void Lerp3(const float a[3], const float b[3], float u, float out[3]) {
    out[0] = a[0] + (b[0] - a[0]) * u;
    out[1] = a[1] + (b[1] - a[1]) * u;
    out[2] = a[2] + (b[2] - a[2]) * u;
}

template <typename KeyArr>
float ComputeLerpU(const KeyArr& keys, std::size_t i, float t) {
    if (i + 1 >= keys.size()) return 0.0f;
    float t0 = keys[i].time, t1 = keys[i+1].time;
    if (t1 <= t0) return 0.0f;
    return std::clamp((t - t0) / (t1 - t0), 0.0f, 1.0f);
}

} // anonymous namespace

void PoseInterpolator::Sample(const AnimationClip& clip,
                              float t,
                              std::vector<BonePose>& poses,
                              std::vector<TrackIndexCache>& cache) const {
    if (cache.size() < clip.tracks.size()) cache.resize(clip.tracks.size());

    for (std::size_t ti = 0; ti < clip.tracks.size(); ++ti) {
        const auto& track = clip.tracks[ti];
        if (track.boneIndex >= poses.size()) continue;     // out-of-range (attachment rig)

        float T[3] = {0,0,0};
        float R[4] = {0,0,0,1};
        float S[3] = {1,1,1};

        // Per-channel sampling: use whatever keyframes are present. Empty arrays
        // leave T/R/S at default (bind-pose identity). Single-keyframe arrays
        // produce a constant value. Multi-keyframe arrays interpolate.
        // `hasStaticPose` is informational only; the sampling decision is
        // count-based (verified by dumping witch attack1_2hsword with the
        // AnimDump dev tool, since removed).
        auto& c = cache[ti];

        if (!track.translation.empty()) {
            std::size_t i = FindKeyIndexForward(track.translation, t, c.lastT);
            c.lastT = i;
            if (i + 1 >= track.translation.size()) {
                std::memcpy(T, track.translation[i].value, sizeof(T));
            } else {
                float u = ComputeLerpU(track.translation, i, t);
                Lerp3(track.translation[i].value, track.translation[i+1].value, u, T);
            }
        }
        if (!track.rotation.empty()) {
            std::size_t i = FindKeyIndexForward(track.rotation, t, c.lastR);
            c.lastR = i;
            if (i + 1 >= track.rotation.size()) {
                std::memcpy(R, track.rotation[i].value, sizeof(R));
            } else {
                float u = ComputeLerpU(track.rotation, i, t);
                SlerpQuat(track.rotation[i].value, track.rotation[i+1].value, u, R);
            }
        }
        if (!track.scale.empty()) {
            std::size_t i = FindKeyIndexForward(track.scale, t, c.lastS);
            c.lastS = i;
            if (i + 1 >= track.scale.size()) {
                std::memcpy(S, track.scale[i].value, sizeof(S));
            } else {
                float u = ComputeLerpU(track.scale, i, t);
                Lerp3(track.scale[i].value, track.scale[i+1].value, u, S);
            }
        }

        // NaN/Inf sanitisation (spec §10). Silent per-bone clamp — per-bone
        // warnings would flood the log if an upstream float edge case poisons
        // many frames. Replace NaN/Inf with safe identity values; guard
        // near-zero scale to prevent geometry collapse.
        auto sanitize3 = [](float v[3], float fallback) {
            for (int i = 0; i < 3; ++i) if (!std::isfinite(v[i])) v[i] = fallback;
        };
        auto sanitize4 = [](float v[4]) {
            for (int i = 0; i < 4; ++i) if (!std::isfinite(v[i])) v[i] = (i == 3 ? 1.0f : 0.0f);
        };
        sanitize3(T, 0.0f);
        sanitize4(R);
        sanitize3(S, 1.0f);
        // Near-zero scale collapses geometry to a point — substitute 1.0.
        for (int i = 0; i < 3; ++i) if (std::fabs(S[i]) < 1e-6f) S[i] = 1.0f;

        ComposeTRS(T, R, S, poses[track.boneIndex].local);
    }
}

// Explicit template instantiation for the KeyArr types we need.
template std::size_t PoseInterpolator::FindKeyIndexForward<std::vector<KeyT>>(
    const std::vector<KeyT>&, float, std::size_t);
template std::size_t PoseInterpolator::FindKeyIndexForward<std::vector<KeyR>>(
    const std::vector<KeyR>&, float, std::size_t);
template std::size_t PoseInterpolator::FindKeyIndexForward<std::vector<KeyS>>(
    const std::vector<KeyS>&, float, std::size_t);

}} // namespace GGPKExp::Animation
