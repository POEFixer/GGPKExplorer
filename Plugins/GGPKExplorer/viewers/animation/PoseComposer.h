#pragma once
#include "AnimationTypes.h"
#include "../../core/parsers/AocTypes.h"   // BoneGroup
#include <string>
#include <vector>

namespace GGPKExp { namespace Animation {

// Composes final pose from layered sampled poses. Per bone:
//   out_b = base_b + Σ_{i>=1} (additive_i_b - rest_b) * weight_i        (if layer is additive)
//   out_b = lerp(out_b, replacement_b, weight_i)                        (if layer is non-additive, i.e. crossfade — NOT used in Phase 4.2 MVP since crossfade deferred)
// Bone-group mask: if maskBoneGroupIdx >= 0 and boneName ∈ group XOR maskInverted, apply; else skip.
//
// User decisions 2026-04-14: only additive composition in Phase 4.2
// (crossfade deferred). Base layer always applied unmasked; additives
// conditionally applied per-bone.
//
// Layer sample inputs are already-interpolated BonePose[] arrays from
// PoseInterpolator::Sample. Caller samples each layer's animation at its
// own time/speed before calling Compose.
class PoseComposer {
public:
    struct LayerInput {
        const std::vector<BonePose>* poses;   // sampled at layer.timeSeconds
        const AnimationLayer*        layer;   // config + weight/mask
    };

    // Compose final per-bone local transforms from base + additive layers.
    // out must be pre-sized to boneNames.size() (or base.size()).
    // boneGroups may be empty (no .aoc loaded) — masks then act as full-body.
    void Compose(const std::vector<BonePose>& base,
                 const std::vector<LayerInput>& additives,
                 const std::vector<BonePose>& restPose,
                 const std::vector<std::string>& boneNames,
                 const std::vector<Parsers::BoneGroup>& boneGroups,
                 std::vector<BonePose>& out) const;

private:
    // Decompose 4x4 into TRS for additive math. out_T, out_R (quat xyzw), out_S.
    static void DecomposeTRS(const float m[16], float T[3], float R[4], float S[3]);
    // Test if bone at index `boneIdx` is affected by the mask.
    // Returns true if layer should apply, false if masked out.
    static bool IsBoneInMask(std::size_t boneIdx,
                             const std::vector<std::string>& boneNames,
                             const Parsers::BoneGroup& group,
                             bool inverted);
};

}} // namespace GGPKExp::Animation
