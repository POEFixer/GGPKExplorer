#include "SkeletonBounds.h"

#include <algorithm>

namespace GGPKExp { namespace Animation {

void SkeletonBounds::Include(float wx, float wy, float wz) {
    if (wx < minX) minX = wx; if (wx > maxX) maxX = wx;
    if (wy < minY) minY = wy; if (wy > maxY) maxY = wy;
    if (wz < minZ) minZ = wz; if (wz > maxZ) maxZ = wz;
}

float SkeletonBounds::MaxExtent() const {
    if (!Valid()) return 0.0f;
    const float ex = maxX - minX;
    const float ey = maxY - minY;
    const float ez = maxZ - minZ;
    return std::max(ex, std::max(ey, ez));
}

SkeletonBounds ComputeStaticLocalBbox(const std::vector<float>& positions) {
    SkeletonBounds b;
    // Positions are packed x,y,z triplets.
    const size_t tri = positions.size() / 3;
    for (size_t i = 0; i < tri; ++i) {
        b.Include(positions[i*3 + 0], positions[i*3 + 1], positions[i*3 + 2]);
    }
    return b;
}

SkeletonBounds ComputeAnimatedBounds(const Parsers::ResolvedSkeleton& sk,
                                      const SkeletonBounds& staticLocal) {
    SkeletonBounds b;
    if (sk.bones.empty()) return b;

    for (const auto& bone : sk.bones) {
        // worldTransform is row-major; translation at [12],[13],[14]
        // (same convention used by MeshViewer's existing follow code).
        const auto& m = bone.worldTransform;
        b.Include(m[12], m[13], m[14]);
    }
    if (!b.Valid()) return b;

    // Inflate by staticLocal extent so target sits near geometry center, not
    // at the joints themselves. For most rigs this is a small fraction of
    // bbox extent (~10-30%).
    const float pad = staticLocal.MaxExtent() * 0.5f;
    b.minX -= pad; b.minY -= pad; b.minZ -= pad;
    b.maxX += pad; b.maxY += pad; b.maxZ += pad;
    return b;
}

}} // namespace GGPKExp::Animation
