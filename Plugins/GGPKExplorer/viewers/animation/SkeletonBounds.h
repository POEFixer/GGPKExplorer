#pragma once

#include "../../core/parsers/AstSkeletonBuilder.h"   // ResolvedSkeleton

#include <cstdint>
#include <limits>
#include <vector>

namespace GGPKExp { namespace Animation {

// Axis-aligned bounding box in world space. Invalid (Valid()==false) when
// default-constructed.
struct SkeletonBounds {
    float minX = std::numeric_limits<float>::infinity();
    float minY = std::numeric_limits<float>::infinity();
    float minZ = std::numeric_limits<float>::infinity();
    float maxX = -std::numeric_limits<float>::infinity();
    float maxY = -std::numeric_limits<float>::infinity();
    float maxZ = -std::numeric_limits<float>::infinity();

    bool Valid() const { return minX <= maxX; }

    // Expand to include world-space point (wx,wy,wz).
    void Include(float wx, float wy, float wz);

    // Center of the box. Only call when Valid().
    void Center(float& cx, float& cy, float& cz) const {
        cx = 0.5f * (minX + maxX);
        cy = 0.5f * (minY + maxY);
        cz = 0.5f * (minZ + maxZ);
    }

    // Longest axis extent (used for inflation radius).
    float MaxExtent() const;
};

// Static local-space bbox of skinned mesh vertices. Computed once per model
// load; reused every frame by ComputeAnimatedBounds. Positions here are in
// the mesh's local space (before skinning).
SkeletonBounds ComputeStaticLocalBbox(const std::vector<float>& positions);

// Computes a per-frame world-space bbox from the skeleton's current
// worldTransforms. Each bone's translation is included; the box is then
// inflated on all axes by staticLocal.MaxExtent() * 0.5f so the center sits
// near the visible geometry even when the model has pivot offset.
//
// Returns an invalid SkeletonBounds when the skeleton has no bones.
SkeletonBounds ComputeAnimatedBounds(const Parsers::ResolvedSkeleton& sk,
                                      const SkeletonBounds& staticLocal);

}} // namespace GGPKExp::Animation
