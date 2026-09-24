#pragma once

#include "../../core/parsers/SmdParser.h"
#include "../../core/parsers/SmParser.h"
#include "../../core/parsers/AstParser.h"
#include "../../core/parsers/AstSkeletonBuilder.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace GGPKExp {

class GGPKManager;

namespace Aoc {

// Phase 5.2 — classification of a child's skinning state. Binary per-child:
// Skinned means the child's skeleton has ≥1 bone name matching the parent
// skeleton, so a per-frame palette can be built that makes the child deform
// with the parent's animation. Rigid falls back to Phase 4 socket parenting.
enum class ChildSkinMode {
    Rigid,    // no skeleton / no bone weights / 0 matches / parent has no skel
    Skinned,  // ≥1 bone match — per-child palette built each frame
};

// Phase 4 — user-driven socket attachment. Session-only.
// Cleared only by DetachAllChildren (new file load) — preserved across Raw Text
// re-parse. Socket keyed by NAME so it survives re-parse even if socket list
// index order changes.
struct AocChildMesh {
    std::string              socketName;
    std::string              sourcePath;      // .sm or .aoc GGPK path
    Parsers::SmdMesh         mesh;
    Parsers::SmData          smData;
    const void*              uploadedMeshAddr = nullptr;
    std::vector<std::string> warnings;

    // --- Phase 5.2 — skinned attachments ---
    Parsers::AstSkeleton              childAst;           // raw (may be empty)
    Parsers::ResolvedSkeleton         childResolvedSkel;  // bones[] + worldTransform
    std::vector<std::array<float, 16>> childBindInverse;  // parallel to childResolvedSkel.bones
    std::vector<int>                  boneRemap;          // child idx → parent idx (-1 = unmatched)
    int                               matchedBoneCount = 0;
    ChildSkinMode                     skinMode = ChildSkinMode::Rigid;
    std::string                       skinModeReason;     // human-readable
    bool                              isStaticMesh = false;  // true only when Rigid due to "no bone weights (static)"
    int                               childRootBoneIndex = -1;  // set by LoadChildSkeleton via FindRootBoneIndex; -1 only if LoadChildSkeleton never ran
    std::vector<int>                  childLocalToChildSkel;  // V01/V02 local→child-skel remap (-1 = unmatched). Empty for DOLm/V3+.

    bool                              pendingTransformSave = false;  // T8 / B1 — set on value change, flushed on popup close

    // --- Phase 5.2.1 — per-attachment socket-local TRS override ---
    // Translation in meters, rotation in Euler XYZ DEGREES, scale as a
    // multiplier (1.0 = no change). Applied in socket-local space by
    // pre-multiplying the override matrix into socketWorld at render time.
    // Identity values (T=0, R=0, S=1) are the default and produce no
    // visible effect. Persisted via AocPersistedAttachments; fields omitted
    // from JSON when identity.
    std::array<float, 3> overrideT = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> overrideR = {0.0f, 0.0f, 0.0f};  // Euler XYZ degrees
    std::array<float, 3> overrideS = {1.0f, 1.0f, 1.0f};
};

// AocSceneGraph owns the main composed mesh for an .aoc file:
//   - Main skin mesh (from SkinMesh.skin reference in .aoc)
//   - Resolved skeleton (from ClientAnimationController.skeleton in .aoc)
//   - Per-submesh material bindings (.sm metadata)
//
// Phase 1: single main mesh + skeleton.
// Phase 4: children attached to sockets (placeholder method already stubbed).
class AocSceneGraph {
public:
    AocSceneGraph() = default;
    ~AocSceneGraph() = default;

    AocSceneGraph(const AocSceneGraph&) = delete;
    AocSceneGraph& operator=(const AocSceneGraph&) = delete;

    // Attempt to load the main skin mesh referenced by the .aoc's SkinMesh block.
    // Resolves <path>.sm + <path>.smd + <path>.ast via GGPKManager.
    // Returns true on success; false if any required file is missing or corrupt.
    // On failure, Error() returns a human-readable explanation.
    bool LoadMain(GGPKManager* mgr, const std::string& skinPath);

    // Reset to empty state (no mesh loaded).
    void Clear();

    // --- Accessors (valid only after LoadMain returns true) -----------------
    bool HasMesh() const { return m_hasMesh; }
    const Parsers::SmdMesh& Mesh() const { return m_mesh; }
    const Parsers::SmData& SmData() const { return m_smData; }
    const Parsers::AstSkeleton& Skeleton() const { return m_skeleton; }
    Parsers::ResolvedSkeleton& ResolvedSkeleton() { return m_resolvedSkeleton; }
    const Parsers::ResolvedSkeleton& ResolvedSkeleton() const { return m_resolvedSkeleton; }

    // Returns AABB of mesh vertices in LOCAL bind-pose space (for grid anchor).
    // min = {minX, minY, minZ}, max = {maxX, maxY, maxZ}.
    // Returns zero-bbox if no mesh is loaded.
    void ComputeMeshLocalBounds(float outMin[3], float outMax[3]) const;

    // Cached inverse-bind matrices for GPU skinning. Populated in Task 6.
    const std::vector<std::array<float, 16>>& BindInverseWorld() const { return m_bindInverseWorld; }

    // Diagnostic -- last fatal error from LoadMain.
    const std::string& Error() const { return m_error; }

    // Non-fatal warnings accumulated during LoadMain (e.g., .ast parse failure
    // on a skinned mesh). Surfaced in Diagnostic tab alongside Error().
    const std::vector<std::string>& Warnings() const { return m_warnings; }

    // Paths of files that were referenced during LoadMain (for Info + Diagnostic tabs).
    // Each entry is "path|OK", "path|MISSING", or "path|OPTIONAL_MISSING"
    // (the last used for files whose absence is legitimate, e.g. .ast on static props).
    const std::vector<std::string>& ResolvedRefs() const { return m_resolvedRefs; }

    // -------- Phase 4 — child attachments --------

    // Attach a .sm or .aoc at the named socket. For .aoc: runs tokenize +
    // parse + resolve, reads resolved SkinMesh.skin, parses .sm (flat — any
    // attached .aoc's skeleton / sockets / animations are discarded).
    //
    // Atomic-swap contract (T6 / B4):
    // - On success: any existing attachment at socketName is replaced atomically.
    // - On failure: the existing attachment (if any) is preserved untouched; JSON
    //   persistence stays consistent with in-memory state. outError is populated.
    //
    // Returns true on success; false on failure (outError populated).
    bool AttachChild(const std::string& socketName,
                     const std::string& sourcePath,
                     const std::vector<uint8_t>& sourceBytes,
                     GGPKManager* mgr,
                     std::string& outError);

    bool DetachChild(const std::string& socketName);
    void DetachAllChildren();

    // Phase 5.2 — child skinning pipeline.
    //
    // Called by AttachChild after a successful .sm/.smd parse. Non-fatal:
    // on failure, child stays attached with Rigid mode and populated warnings.
    // Two-stage: LoadChildSkeleton populates childAst/childResolvedSkel/
    // childBindInverse; BuildChildBoneRemap matches child→parent bone names
    // and sets skinMode/skinModeReason/boneRemap/matchedBoneCount.
    bool LoadChildSkeleton(AocChildMesh& child, GGPKManager* mgr);
    void BuildChildBoneRemap(AocChildMesh& child);

    int  ChildCount() const { return static_cast<int>(m_children.size()); }
    const std::vector<AocChildMesh>& Children() const { return m_children; }
    // Phase 4 M8 — mutable access for the 3D preview render pass to update
    // per-child cache fields (e.g. uploadedMeshAddr) without const_cast.
    // Mirrors AocParticleSystem::MutableEmitters() pattern from Phase 3.
    std::vector<AocChildMesh>& MutableChildren() { return m_children; }
    AocChildMesh*       FindChildBySocket(const std::string& socketName);
    const AocChildMesh* FindChildBySocket(const std::string& socketName) const;

private:
    bool m_hasMesh = false;
    Parsers::SmdMesh m_mesh;
    Parsers::SmData m_smData;
    Parsers::AstSkeleton m_skeleton;
    Parsers::ResolvedSkeleton m_resolvedSkeleton;
    std::vector<std::array<float, 16>> m_bindInverseWorld;

    std::string m_error;
    std::vector<std::string> m_warnings;
    std::vector<std::string> m_resolvedRefs;

    std::vector<AocChildMesh> m_children;   // Phase 4
};

// Phase 5.2.1 — transform override helpers.
//
// IsTransformIdentity: true when all T components <= 1e-6, all R components
// <= 1e-6 (degrees), and all S components within 1e-6 of 1.0. Used by UI
// (to toggle the [modified] badge) and persistence (to omit the transform
// field when default).
//
// ComposeOverrideMatrix: builds a row-major 4x4 in row-vector convention
// (v' = v x M). Multiplication order: v x Scale x Rx x Ry x Rz x Translate.
// Rotation is intrinsic XYZ (rotate around child-local X first, then Y,
// then Z in the frame after prior rotations). Scale multiplies each of
// the three basis rows of the rotation. Identity input produces the
// identity matrix (verified by ZERO-terms in S=1, R=0, T=0 collapse).
bool IsTransformIdentity(const AocChildMesh& c);
void ComposeOverrideMatrix(const AocChildMesh& c, float outRowMajor4x4[16]);

// Walks a resolved skeleton's arrayParent chain to find the root bone
// (first bone with arrayParent < 0). Defensive fallback: returns 0 when no
// bone has parent == -1 (shouldn't happen in valid .ast, but guards against
// malformed files).
int FindRootBoneIndex(const Parsers::ResolvedSkeleton& skel);

} // namespace Aoc
} // namespace GGPKExp
