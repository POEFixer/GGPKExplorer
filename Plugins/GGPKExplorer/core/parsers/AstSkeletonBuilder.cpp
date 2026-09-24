#include "AstSkeletonBuilder.h"
#include <cstring>

namespace GGPKExp { namespace Parsers {

namespace {

constexpr std::size_t kNoIdx = static_cast<std::size_t>(-1);
constexpr int kMaxDepth = 256;

// Row-major, row-vector matrix multiply: out = a * b.
// Same convention as MeshViewer::MulMat4 — a child's local transform composed
// against a parent's world transform yields the child's world transform.
void MulMat4Local(const float* a, const float* b, float* out) {
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[row * 4 + k] * b[k * 4 + col];
            }
            out[row * 4 + col] = sum;
        }
    }
}

// FCNS walk visiting bone i with given parent's array index and world matrix.
// Sets bones[i].arrayParent and bones[i].worldTransform.
// Then recurses into firstChildIndex (with i as new parent),
// then iterates nextSiblingIndex (with the SAME parent — siblings share a parent).
//
// Returns false on cycle, OOB index, or depth overflow.
bool WalkFcns(const AstSkeleton& parsed, ResolvedSkeleton& out,
              std::size_t boneIdx, std::size_t parentArr,
              const float* parentWorld, std::vector<bool>& visited, int depth) {
    if (depth > kMaxDepth) return false;
    if (boneIdx >= parsed.bones.size()) return false;
    if (visited[boneIdx]) return false;
    visited[boneIdx] = true;

    const auto& src = parsed.bones[boneIdx];
    auto& dst = out.bones[boneIdx];
    dst.arrayParent = parentArr;
    if (parentArr == kNoIdx) {
        std::memcpy(dst.worldTransform, dst.localTransform, sizeof(dst.worldTransform));
    } else {
        MulMat4Local(dst.localTransform, parentWorld, dst.worldTransform);
    }

    if (src.firstChildIndex != 0xFF) {
        if (!WalkFcns(parsed, out, src.firstChildIndex, boneIdx,
                      dst.worldTransform, visited, depth + 1)) {
            return false;
        }
    }
    if (src.nextSiblingIndex != 0xFF) {
        if (!WalkFcns(parsed, out, src.nextSiblingIndex, parentArr,
                      parentWorld, visited, depth + 1)) {
            return false;
        }
    }
    return true;
}

} // anonymous namespace

Result<ResolvedSkeleton> BuildResolvedSkeleton(const AstSkeleton& parsed) {
    ResolvedSkeleton out;
    out.bones.resize(parsed.bones.size());

    if (parsed.bones.empty()) {
        out.walkSucceeded = true;
        return Result<ResolvedSkeleton>::Ok(std::move(out));
    }

    // Copy names + local transforms first — even if the walk fails, the visualization
    // layer can still draw individual bone points.
    for (std::size_t i = 0; i < parsed.bones.size(); ++i) {
        out.bones[i].name = parsed.bones[i].name;
        std::memcpy(out.bones[i].localTransform, parsed.bones[i].transform,
                    sizeof(out.bones[i].localTransform));
    }

    static const float kIdentity[16] = {
        1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1
    };
    std::vector<bool> visited(parsed.bones.size(), false);
    out.walkSucceeded = WalkFcns(parsed, out, 0, kNoIdx, kIdentity, visited, 0);
    return Result<ResolvedSkeleton>::Ok(std::move(out));
}

}} // namespace GGPKExp::Parsers
