#pragma once

#include "../Result.h"
#include "AocTypes.h"
#include "AstSkeletonBuilder.h"
#include <string>
#include <vector>

namespace GGPKExp { namespace Parsers {

// Populates skel.virtualBones[] from the sockets in doc.merged.clientAnim.
// Orphan sockets (parent bone name not in skel.bones) are skipped, with a
// warning appended if `warnings` is non-null.
//
// Composition convention (matches AstSkeletonBuilder):
//   M_local  = S × R × T   (scale first, then rotate, then translate)
//   M_world  = M_local × parentBone.worldTransform   (row-vector order)
//
// Rotation: Euler XYZ in radians. Order MAY be wrong — flagged as Phase 3.5
// RE work. If L3 smoke test shows rotation mismatch, Sprint 3 investigation
// will decompile the game's socket composer to verify.
Result<void> AugmentSkeleton(const AocDocument&         doc,
                              ResolvedSkeleton&          skel,
                              std::vector<std::string>*  warnings = nullptr);

// Per-frame helper: recompute virtualBones[i].worldTransform from stored
// localTRS + live parent bone's worldTransform. Called after any change to
// ResolvedSkeleton::bones[].worldTransform (e.g. per-frame from AnimationPlayer::ApplyFrame).
// No-op if virtualBones is empty (no .aoc loaded).
//
// Distinct from AugmentSkeleton: that is one-time setup; this is an incremental
// refresh using pre-computed localTRS.
void UpdateVirtualBoneWorldTransforms(ResolvedSkeleton& skel);

}} // namespace GGPKExp::Parsers
