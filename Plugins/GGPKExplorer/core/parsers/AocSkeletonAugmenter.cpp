#include "AocSkeletonAugmenter.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

namespace GGPKExp { namespace Parsers {

namespace {

// Row-vector matrix multiply: out = a × b.
// Matches AstSkeletonBuilder::MulMat4.
void MulMat4(const float* a, const float* b, float* out) {
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

void BuildScaleMatrix(const float s[3], float out[16]) {
    std::memset(out, 0, sizeof(float) * 16);
    out[0]  = s[0];
    out[5]  = s[1];
    out[10] = s[2];
    out[15] = 1.0f;
}

void BuildTranslationMatrix(const float t[3], float out[16]) {
    std::memset(out, 0, sizeof(float) * 16);
    out[0]  = 1.0f;
    out[5]  = 1.0f;
    out[10] = 1.0f;
    out[12] = t[0];
    out[13] = t[1];
    out[14] = t[2];
    out[15] = 1.0f;
}

// Euler XYZ → 4x4 rotation matrix, row-major, row-vector convention.
// Applies Rx, then Ry, then Rz.
void EulerXyzToMat4(const float r[3], float out[16]) {
    const float cx = std::cos(r[0]), sx = std::sin(r[0]);
    const float cy = std::cos(r[1]), sy = std::sin(r[1]);
    const float cz = std::cos(r[2]), sz = std::sin(r[2]);

    // Rz * Ry * Rx   (column-vector convention would multiply in this order;
    // for row-vector we write the transposed product into the row-major buffer).
    // We produce out such that  v' = v × out  applies Rx, then Ry, then Rz.
    float rx[16] = {
        1, 0,  0, 0,
        0, cx, sx, 0,
        0, -sx, cx, 0,
        0, 0, 0, 1,
    };
    float ry[16] = {
        cy, 0, -sy, 0,
        0,  1,  0,  0,
        sy, 0,  cy, 0,
        0,  0,  0,  1,
    };
    float rz[16] = {
        cz, sz, 0, 0,
        -sz, cz, 0, 0,
        0,   0, 1, 0,
        0,   0, 0, 1,
    };
    float rxry[16];
    MulMat4(rx, ry, rxry);
    MulMat4(rxry, rz, out);
}

void ComposeSocketLocalTRS(const float t[3], const float r[3], const float s[3], float out[16]) {
    float mS[16]; BuildScaleMatrix(s, mS);
    float mR[16]; EulerXyzToMat4(r, mR);
    float mT[16]; BuildTranslationMatrix(t, mT);
    float mSR[16];
    MulMat4(mS, mR, mSR);
    MulMat4(mSR, mT, out);
}

// -----------------------------------------------------------------------
// ignore_parent_* flag helpers (Phase 3.5)
// -----------------------------------------------------------------------

// Treat raw string value as boolean. Empty, "0", "false", "no" (any case) -> false.
bool IsTruthy(const std::string& v) {
    if (v.empty()) return false;
    std::string lower;
    lower.reserve(v.size());
    for (char c : v) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lower == "0" || lower == "false" || lower == "no" || lower == "off") return false;
    return true;
}

// Extract x/y/z letters from raw value (strips whitespace, quotes, punctuation).
std::string NormalizeAxes(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (char c : v) {
        const char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lc == 'x' || lc == 'y' || lc == 'z') out.push_back(lc);
    }
    return out;
}

// Copy parent world's translation (indices 12-14) into an otherwise-identity 4x4.
void ExtractTranslationOnly(const float world[16], float out[16]) {
    std::memset(out, 0, sizeof(float) * 16);
    out[0]  = 1.0f;
    out[5]  = 1.0f;
    out[10] = 1.0f;
    out[12] = world[12];
    out[13] = world[13];
    out[14] = world[14];
    out[15] = 1.0f;
}

// Decompose the rotation portion (3x3 upper-left) of a row-major row-vector matrix
// produced by EulerXyzToMat4 (Rx × Ry × Rz) back into Euler XYZ angles in radians.
// Assumes orthonormal rotation (no scale). Gimbal-lock edge case (|sy|→1) is
// handled by clamping and falling back to atan2 of the non-degenerate row.
void ExtractEulerXyz(const float m[16], float eulerOut[3]) {
    // From EulerXyzToMat4: m[2] = -sy, m[6] = sx*cy, m[10] = cx*cy, m[1] = cy*sz, m[0] = cy*cz.
    float sy = -m[2];
    if (sy >  1.0f) sy =  1.0f;
    if (sy < -1.0f) sy = -1.0f;
    const float y = std::asin(sy);
    const float cy = std::cos(y);

    if (std::fabs(cy) > 1e-5f) {
        eulerOut[0] = std::atan2(m[6], m[10]); // x
        eulerOut[1] = y;                        // y
        eulerOut[2] = std::atan2(m[1], m[0]);   // z
    } else {
        // Gimbal lock: cy ~ 0, meaning y = ±π/2. Put all remaining rotation on x.
        eulerOut[0] = std::atan2(-m[9], m[5]);
        eulerOut[1] = y;
        eulerOut[2] = 0.0f;
    }
}

// Build an "effective parent" matrix with rotation stripped per the ignore flags.
//   - ignoreRot = true  -> identity rotation + parent translation
//   - else ignoreAxes non-empty -> zero the specified Euler axes, keep others + translation
//   - else passthrough parent as-is.
void ApplyParentIgnoreFlags(const float parentWorld[16],
                             bool ignoreRot,
                             const std::string& ignoreAxes,
                             float out[16]) {
    if (ignoreRot) {
        ExtractTranslationOnly(parentWorld, out);
        return;
    }
    if (ignoreAxes.empty()) {
        std::memcpy(out, parentWorld, sizeof(float) * 16);
        return;
    }

    float euler[3];
    ExtractEulerXyz(parentWorld, euler);
    if (ignoreAxes.find('x') != std::string::npos) euler[0] = 0.0f;
    if (ignoreAxes.find('y') != std::string::npos) euler[1] = 0.0f;
    if (ignoreAxes.find('z') != std::string::npos) euler[2] = 0.0f;

    float rotMat[16];
    EulerXyzToMat4(euler, rotMat);

    // Preserve parent translation (overwrite row 3 of rotMat).
    std::memcpy(out, rotMat, sizeof(float) * 16);
    out[12] = parentWorld[12];
    out[13] = parentWorld[13];
    out[14] = parentWorld[14];
    out[15] = 1.0f;
}

} // anonymous namespace

Result<void> AugmentSkeleton(const AocDocument&         doc,
                              ResolvedSkeleton&          skel,
                              std::vector<std::string>*  warnings) {
    skel.virtualBones.clear();

    // Find ClientAnimationController in merged blocks
    const ClientAnimCtrl* ctrl = nullptr;
    for (const AocBlock& b : doc.merged.blocks) {
        if (b.clientAnim) { ctrl = b.clientAnim.get(); break; }
    }
    if (!ctrl || ctrl->sockets.empty()) {
        return Result<void>::Ok();
    }

    // Build bone-name → index map. Names are case-sensitive (game uses them that way).
    std::unordered_map<std::string, std::size_t> nameIndex;
    nameIndex.reserve(skel.bones.size());
    for (std::size_t i = 0; i < skel.bones.size(); ++i) {
        nameIndex.emplace(skel.bones[i].name, i);
    }

    for (const Socket& sock : ctrl->sockets) {
        // Socket without an explicit `parent = "..."` field falls back to the
        // root bone (index 0). Observed in real data: lakestash.aoc /
        // thethirdedict.aoc define sockets with only `translation = "..."`
        // under ClientAnimationController, and the game treats them as root-
        // bone children. Without this fallback they are dropped as orphans
        // and downstream features (particle gizmos, bone_group resolution)
        // cannot locate them.
        std::size_t parentIdx;
        if (sock.parentBoneName.empty() && !skel.bones.empty()) {
            parentIdx = 0;
        } else {
            auto it = nameIndex.find(sock.parentBoneName);
            if (it == nameIndex.end()) {
                if (warnings) {
                    warnings->push_back("Orphan socket '" + sock.name +
                                        "' — parent bone '" + sock.parentBoneName + "' not in skeleton");
                }
                continue;
            }
            parentIdx = it->second;
        }
        const float* parentWorld = skel.bones[parentIdx].worldTransform;

        VirtualBone vb;
        vb.name = sock.name;
        vb.parentBoneIndex = parentIdx;
        vb.sourceAocPath = sock.sourceAocPath;

        // Apply ignore_parent_* flags to the effective parent world matrix (Phase 3.5).
        const bool        ignoreRot   = IsTruthy(sock.ignoreParentRot);
        const std::string ignoreAxes  = NormalizeAxes(sock.ignoreParentAxis);

        float effectiveParent[16];
        ApplyParentIgnoreFlags(parentWorld, ignoreRot, ignoreAxes, effectiveParent);

        ComposeSocketLocalTRS(sock.translation, sock.rotation, sock.scale, vb.localTRS);
        MulMat4(vb.localTRS, effectiveParent, vb.worldTransform);

        // Report any raw ignore_parent_axis letters we could not interpret (non-x/y/z chars).
        if (warnings && !sock.ignoreParentAxis.empty() && ignoreAxes.empty() && !ignoreRot) {
            warnings->push_back("Socket '" + sock.name +
                                "' ignore_parent_axis='" + sock.ignoreParentAxis +
                                "' contains no x/y/z axis letters -- flag ignored");
        }

        skel.virtualBones.push_back(std::move(vb));
    }
    return Result<void>::Ok();
}

void UpdateVirtualBoneWorldTransforms(ResolvedSkeleton& skel) {
    auto matmul = [](const float a[16], const float b[16], float out[16]) {
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                float v = 0.0f;
                for (int k = 0; k < 4; ++k) v += a[r * 4 + k] * b[k * 4 + c];
                out[r * 4 + c] = v;
            }
        }
    };

    for (auto& vb : skel.virtualBones) {
        if (vb.parentBoneIndex >= skel.bones.size()) {
            // Orphan or invalid - world = localTRS (matches initial AugmentSkeleton behaviour
            // when a socket has no live parent to compose against).
            std::memcpy(vb.worldTransform, vb.localTRS, sizeof(float) * 16);
            continue;
        }
        const auto& parent = skel.bones[vb.parentBoneIndex];
        matmul(vb.localTRS, parent.worldTransform, vb.worldTransform);
    }
}

}} // namespace GGPKExp::Parsers
