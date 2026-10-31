#pragma once

#include "../Result.h"
#include "AstParser.h"
#include <cstddef>
#include <string>
#include <vector>

namespace GGPKExp { namespace Parsers {

// One bone in a fully-resolved skeleton. Same array index as the corresponding
// AstBone in the input. arrayParent is the resolved parent's index, derived from
// the FCNS walk (NOT from any field on the wire).
struct ResolvedBone {
    std::string name;
    std::size_t arrayParent = static_cast<std::size_t>(-1); // -1 = root
    float       localTransform[16] = {}; // copied from AstBone.transform
    float       worldTransform[16] = {}; // composed: world = local * parent.world (row-vector)
};

struct ResolvedSkeleton {
    std::vector<ResolvedBone>  bones;         // same count and order as parsed.bones
    bool                       walkSucceeded = false; // false = malformed file (cycle, OOB, depth limit)
};

// Walks the FCNS tree starting from parsed.bones[0]:
//   - bones[0] is the implicit root; its world == its local transform.
//   - Recurse into firstChildIndex (if != 0xFF) with this bone as new parent.
//   - Iterate nextSiblingIndex (if != 0xFF) sharing the SAME parent.
//   - bones[0]'s nextSiblingIndex chain (if any) walked as additional roots — supports multi-root skeletons.
//
// On success returns walkSucceeded=true with arrayParent and worldTransform filled
// for every reachable bone. On failure (cycle, out-of-range index, depth > 256)
// returns walkSucceeded=false; bones[] is still populated (names + local transforms)
// but arrayParent values may be SIZE_MAX. Visualization layer treats those as roots.
//
// Algorithm matches game function at IDA 0x7FF7A660D740 (POE2 v0.4.0i).
// Verified against live runtime memory in Sprint 2. See BONES.md Section 2.
Result<ResolvedSkeleton> BuildResolvedSkeleton(const AstSkeleton& parsed);

}} // namespace GGPKExp::Parsers
