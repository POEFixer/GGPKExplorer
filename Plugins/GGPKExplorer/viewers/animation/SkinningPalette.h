#pragma once

#include "../../core/parsers/AstSkeletonBuilder.h"

#include <array>
#include <cstddef>
#include <vector>

namespace GGPKExp { namespace Animation {

// Build the per-bone skinning palette from a resolved skeleton.
//
// Convention (row-vector, row-major; matches ResolvedBone::worldTransform
// where translation is at indices 12-14):
//
//   palette[i] = bindInverse[i] * skel.bones[i].worldTransform
//
// Reads `skel.bones[i].worldTransform` (i.e. the CURRENT pose, after
// AnimationPlayer::ApplyFrame has run for this frame).
//
// `bindInverse` must contain the cached inverse of each bone's BIND-pose
// worldTransform (computed once at skeleton load). Caller is responsible
// for matching its size to skel.bones.size().
//
// `outPalette` must be pre-sized to skel.bones.size() (same indexing).
//
// Caps at maxBones; when skel.bones.size() > maxBones, only the first
// maxBones entries are written and *outTruncated is set to true (if
// outTruncated is non-null). Caller decides whether to push a one-time
// warning into AnimationPlayer::GetWarnings().
//
// Thread-safety: stateless, safe to call from any thread provided the
// skel/bindInverse/outPalette buffers are not being mutated concurrently.
void BuildSkinningPalette(
    const Parsers::ResolvedSkeleton& skel,
    const std::vector<std::array<float, 16>>& bindInverse,
    std::size_t maxBones,
    std::vector<std::array<float, 16>>& outPalette,
    bool* outTruncated = nullptr);

}} // namespace GGPKExp::Animation
