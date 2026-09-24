#include "SkinningPalette.h"
#include "MatrixMath.h"

#include <cstring>

namespace GGPKExp { namespace Animation {

void BuildSkinningPalette(
    const Parsers::ResolvedSkeleton& skel,
    const std::vector<std::array<float, 16>>& bindInverse,
    std::size_t maxBones,
    std::vector<std::array<float, 16>>& outPalette,
    bool* outTruncated)
{
    const std::size_t n        = skel.bones.size();
    const std::size_t capped   = (n > maxBones) ? maxBones : n;
    if (outTruncated) *outTruncated = (n > maxBones);

    // Defensive: if caller's buffers are mis-sized, do nothing for the
    // out-of-range entries rather than writing past the end.
    const std::size_t safeBindN = (bindInverse.size() < capped) ? bindInverse.size() : capped;
    const std::size_t safeOutN  = (outPalette.size()  < capped) ? outPalette.size()  : capped;
    const std::size_t safeN     = (safeBindN < safeOutN) ? safeBindN : safeOutN;

    for (std::size_t i = 0; i < safeN; ++i) {
        // palette[i] = bindInverse[i] * worldTransform[i]   (row-vector convention)
        MatrixMath::Multiply4x4RowMajor(
            bindInverse[i].data(),
            skel.bones[i].worldTransform,
            outPalette[i].data());
    }
}

}} // namespace GGPKExp::Animation
