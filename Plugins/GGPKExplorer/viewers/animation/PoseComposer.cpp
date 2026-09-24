#include "PoseComposer.h"
#include "PoseInterpolator.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace GGPKExp { namespace Animation {

void PoseComposer::DecomposeTRS(const float m[16], float T[3], float R[4], float S[3]) {
    T[0] = m[12]; T[1] = m[13]; T[2] = m[14];
    S[0] = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    S[1] = std::sqrt(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
    S[2] = std::sqrt(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);
    float invSx = (S[0] > 1e-6f) ? 1.0f / S[0] : 0.0f;
    float invSy = (S[1] > 1e-6f) ? 1.0f / S[1] : 0.0f;
    float invSz = (S[2] > 1e-6f) ? 1.0f / S[2] : 0.0f;
    // Extract rotation matrix (scale-normalised).
    float r[9] = {
        m[0]*invSx, m[1]*invSx, m[2]*invSx,
        m[4]*invSy, m[5]*invSy, m[6]*invSy,
        m[8]*invSz, m[9]*invSz, m[10]*invSz
    };
    // 3x3 rotation matrix → quaternion (Shepperd's method).
    float trace = r[0] + r[4] + r[8];
    if (trace > 0.0f) {
        float s = std::sqrt(trace + 1.0f) * 2.0f;
        R[3] = 0.25f * s;
        R[0] = (r[5] - r[7]) / s;
        R[1] = (r[6] - r[2]) / s;
        R[2] = (r[1] - r[3]) / s;
    } else if (r[0] > r[4] && r[0] > r[8]) {
        float s = std::sqrt(1.0f + r[0] - r[4] - r[8]) * 2.0f;
        R[3] = (r[5] - r[7]) / s;
        R[0] = 0.25f * s;
        R[1] = (r[1] + r[3]) / s;
        R[2] = (r[2] + r[6]) / s;
    } else if (r[4] > r[8]) {
        float s = std::sqrt(1.0f + r[4] - r[0] - r[8]) * 2.0f;
        R[3] = (r[6] - r[2]) / s;
        R[0] = (r[1] + r[3]) / s;
        R[1] = 0.25f * s;
        R[2] = (r[5] + r[7]) / s;
    } else {
        float s = std::sqrt(1.0f + r[8] - r[0] - r[4]) * 2.0f;
        R[3] = (r[1] - r[3]) / s;
        R[0] = (r[2] + r[6]) / s;
        R[1] = (r[5] + r[7]) / s;
        R[2] = 0.25f * s;
    }
}

bool PoseComposer::IsBoneInMask(std::size_t boneIdx,
                                const std::vector<std::string>& boneNames,
                                const Parsers::BoneGroup& group,
                                bool inverted) {
    if (boneIdx >= boneNames.size()) return false;
    const auto& name = boneNames[boneIdx];
    for (const auto& gname : group.boneNames) {
        if (gname == name) return !inverted;
    }
    return inverted;
}

void PoseComposer::Compose(const std::vector<BonePose>& base,
                           const std::vector<LayerInput>& additives,
                           const std::vector<BonePose>& restPose,
                           const std::vector<std::string>& boneNames,
                           const std::vector<Parsers::BoneGroup>& boneGroups,
                           std::vector<BonePose>& out) const {
    const std::size_t N = base.size();
    if (out.size() != N) out.resize(N);

    // Start with base pose.
    for (std::size_t i = 0; i < N; ++i) std::memcpy(out[i].local, base[i].local, sizeof(float)*16);

    for (const auto& L : additives) {
        const AnimationLayer& layer = *L.layer;
        if (layer.animIndex < 0 || layer.weight <= 0.0f || !L.poses) continue;
        if (!layer.additive) continue;                       // MVP: only additive path

        const Parsers::BoneGroup* grp = nullptr;
        if (layer.maskBoneGroupIdx >= 0 &&
            static_cast<std::size_t>(layer.maskBoneGroupIdx) < boneGroups.size()) {
            grp = &boneGroups[layer.maskBoneGroupIdx];
        }

        for (std::size_t i = 0; i < N; ++i) {
            if (grp && !IsBoneInMask(i, boneNames, *grp, layer.maskInverted)) continue;

            float baseT[3], baseR[4], baseS[3];
            float addT[3],  addR[4],  addS[3];
            float restT[3], restR[4], restS[3];
            DecomposeTRS(out[i].local,          baseT, baseR, baseS);
            DecomposeTRS((*L.poses)[i].local,   addT,  addR,  addS);
            DecomposeTRS(restPose[i].local,     restT, restR, restS);

            // Additive T: out += (add - rest) * w
            float outT[3];
            for (int k = 0; k < 3; ++k)
                outT[k] = baseT[k] + (addT[k] - restT[k]) * layer.weight;

            // Additive R: delta = add * inv(rest); out = nlerp(out, delta * out, w)
            auto quatMul = [](const float a[4], const float b[4], float o[4]) {
                o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
                o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
                o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
                o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
            };
            float restInv[4] = { -restR[0], -restR[1], -restR[2], restR[3] }; // conjugate = inv for unit quat
            float delta[4];  quatMul(addR, restInv, delta);
            float deltaBase[4]; quatMul(delta, baseR, deltaBase);
            float outR[4];
            PoseInterpolator::SlerpQuat(baseR, deltaBase, layer.weight, outR);

            // Additive S: out *= lerp(1, add/rest, w)
            float outS[3];
            for (int k = 0; k < 3; ++k) {
                float r = (std::fabs(restS[k]) > 1e-6f) ? (addS[k] / restS[k]) : 1.0f;
                outS[k] = baseS[k] * (1.0f + (r - 1.0f) * layer.weight);
            }

            PoseInterpolator::ComposeTRS(outT, outR, outS, out[i].local);
        }
    }
}

}} // namespace GGPKExp::Animation
