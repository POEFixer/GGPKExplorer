#include "AocSceneGraph.h"
#include "../../core/GGPKManager.h"
#include "../../core/parsers/AocTokenizer.h"
#include "../../core/parsers/AocParser.h"
#include "../../core/parsers/AocResolver.h"
#include "../animation/MatrixMath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace GGPKExp {
namespace Aoc {

namespace {

// Helper: resolve a path in GGPK to bytes. Returns empty vector if not found.
// Appends "<path>|OK", "<path>|MISSING", or (when optional==true and missing)
// "<path>|OPTIONAL_MISSING" to refs.
std::vector<uint8_t> ReadFromGGPK(GGPKManager* mgr,
                                   const std::string& path,
                                   std::vector<std::string>& refs,
                                   bool optional = false) {
    const char* missingSuffix = optional ? "|OPTIONAL_MISSING" : "|MISSING";
    if (!mgr) {
        refs.push_back(path + (optional ? "|OPTIONAL_MISSING (no GGPKManager)"
                                        : "|MISSING (no GGPKManager)"));
        return {};
    }
    void* node = mgr->FindNodeByPath(path);
    if (!node) {
        refs.push_back(path + missingSuffix);
        return {};
    }
    auto result = mgr->ReadFile(node);
    if (!result.success || result.value.empty()) {
        refs.push_back(path + missingSuffix);
        return {};
    }
    refs.push_back(path + "|OK");
    return std::move(result.value);
}

// Compute cached inverse bind matrices for GPU skinning palette.
// Each entry is the inverse of the bone's BIND-pose worldTransform, so that at
// runtime the shader can compute `palette[i] = bindInverse[i] * bones[i].worldTransform`
// to get a matrix that moves a vertex from bind pose into the current pose.
// Mirrors MeshViewer::RebuildBindInverseWorld() pattern (uses the same helper).
// On singular matrices the entry falls back to identity so the mesh keeps its
// bind-pose position through that bone instead of producing NaNs.
void ComputeBindInverseWorld(const Parsers::ResolvedSkeleton& skel,
                              std::vector<std::array<float, 16>>& outInv) {
    outInv.clear();
    outInv.resize(skel.bones.size());
    for (std::size_t i = 0; i < skel.bones.size(); ++i) {
        if (!Animation::MatrixMath::Inverse4x4(skel.bones[i].worldTransform,
                                                outInv[i].data())) {
            // Singular bind matrix — substitute identity. Mesh will skin
            // incorrectly through this bone, but won't NaN out.
            std::memset(outInv[i].data(), 0, sizeof(float) * 16);
            outInv[i][0]  = 1.0f;
            outInv[i][5]  = 1.0f;
            outInv[i][10] = 1.0f;
            outInv[i][15] = 1.0f;
        }
    }
}

} // namespace

void AocSceneGraph::Clear() {
    m_hasMesh = false;
    m_mesh = {};
    m_smData = {};
    m_skeleton = {};
    m_resolvedSkeleton = {};
    m_bindInverseWorld.clear();
    m_error.clear();
    m_warnings.clear();
    m_resolvedRefs.clear();
}

bool AocSceneGraph::LoadMain(GGPKManager* mgr, const std::string& skinPath) {
    Clear();

    if (skinPath.empty()) {
        m_error = "No SkinMesh.skin path in .aoc";
        return false;
    }

    // ------------------------------------------------------------------
    // Step 1: Normalize skinPath into a base path (no extension).
    //
    // The .aoc's SkinMesh.skin field may be bare (no extension) or may carry
    // ".sm" / another extension. We always strip the trailing extension of
    // the LAST path segment before appending ".sm" / ".ast" ourselves.
    // ------------------------------------------------------------------
    std::string basePath = skinPath;
    {
        size_t dot = basePath.find_last_of('.');
        size_t slash = basePath.find_last_of("/\\");
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            basePath.resize(dot);
        }
    }

    // ------------------------------------------------------------------
    // Step 2: Read + parse .sm (UTF-16LE text descriptor).
    //
    // Unlike POE1, the .smd is NOT at `<basePath>.smd`. Its path (with a hash
    // suffix such as "_armour_24f6bd53.smd") is encoded INSIDE the .sm text
    // block as SmData::smdPath. This mirrors MeshViewer::DoMainThreadRead.
    // ------------------------------------------------------------------
    auto smBytes = ReadFromGGPK(mgr, basePath + ".sm", m_resolvedRefs);
    if (smBytes.empty()) {
        m_error = "Missing .sm for: " + basePath;
        return false;
    }

    {
        auto smResult = Parsers::ParseSm(smBytes.data(), smBytes.size());
        if (!smResult.success) {
            m_error = "Failed to parse .sm: " + basePath +
                      (smResult.error.empty() ? "" : (" -- " + smResult.error));
            return false;
        }
        m_smData = std::move(smResult.value);
    }

    if (m_smData.smdPath.empty()) {
        m_error = ".sm has no smdPath field: " + basePath;
        return false;
    }

    // ------------------------------------------------------------------
    // Step 3: Read + parse .smd from the path encoded in the .sm.
    // ------------------------------------------------------------------
    auto smdBytes = ReadFromGGPK(mgr, m_smData.smdPath, m_resolvedRefs);
    if (smdBytes.empty()) {
        m_error = "Missing .smd at: " + m_smData.smdPath;
        return false;
    }

    {
        auto smdResult = Parsers::ParseSmd(smdBytes.data(), smdBytes.size());
        if (!smdResult.success) {
            m_error = "Failed to parse .smd: " + m_smData.smdPath +
                      (smdResult.error.empty() ? "" : (" -- " + smdResult.error));
            return false;
        }
        m_mesh = std::move(smdResult.value);
    }

    // ------------------------------------------------------------------
    // Step 4: Optional companion .smd.header.
    //
    // MeshViewer reads these bytes but never parses them (vertex/index stride
    // + submesh names are already available via ParseSmd). For Phase 1 we
    // only touch the file so it shows up in ResolvedRefs for diagnostics;
    // we do NOT parse it and failure is ignored.
    // ------------------------------------------------------------------
    (void)ReadFromGGPK(mgr, m_smData.smdPath + ".header",
                       m_resolvedRefs, /*optional=*/true);

    // ------------------------------------------------------------------
    // Step 5: Resolve the per-mesh skeleton with two-tier fallback.
    //
    // POE2 layout (verified against 27,566 .sm files in MeshViewer notes):
    //   1. <smd-directory>/<sm-basename>.ast   — per-mesh rig (13.6%)
    //   2. <smd-directory>/rig.ast             — shared rig (most meshes)
    //   3. (neither)                           — static prop, no skeleton
    //
    // The .sm basename (e.g. "bittersoncorpse01") differs from the .smd
    // basename (hashed, e.g. "BitterSon01_StaticCorpse_cb7cfce7"), so the
    // per-mesh .ast is derived from `basePath`, not `smdPath`.
    // ------------------------------------------------------------------
    std::string smBase = basePath;
    {
        size_t sBSlash = smBase.find_last_of("/\\");
        if (sBSlash != std::string::npos) {
            smBase = smBase.substr(sBSlash + 1);
        }
    }

    std::string smdDir = m_smData.smdPath;
    {
        size_t smdSlash = smdDir.find_last_of("/\\");
        if (smdSlash != std::string::npos) {
            smdDir.resize(smdSlash + 1);
        } else {
            smdDir.clear();
        }
    }

    std::vector<uint8_t> astBytes;
    if (!smBase.empty()) {
        const std::string perMeshAstPath = smdDir + smBase + ".ast";
        astBytes = ReadFromGGPK(mgr, perMeshAstPath, m_resolvedRefs, /*optional=*/true);
    }
    if (astBytes.empty()) {
        const std::string rigPath = smdDir + "rig.ast";
        astBytes = ReadFromGGPK(mgr, rigPath, m_resolvedRefs, /*optional=*/true);
    }

    // Parse skeleton if found (non-fatal: skin-only / static prop falls through).
    if (!astBytes.empty()) {
        auto astResult = Parsers::ParseAst(astBytes.data(), astBytes.size());
        if (!astResult.success) {
            m_warnings.push_back("Failed to parse .ast: " + basePath +
                                 (astResult.error.empty() ? "" : (" -- " + astResult.error)));
        } else {
            m_skeleton = std::move(astResult.value);
            auto built = Parsers::BuildResolvedSkeleton(m_skeleton);
            if (built.success) {
                m_resolvedSkeleton = std::move(built.value);
                ComputeBindInverseWorld(m_resolvedSkeleton, m_bindInverseWorld);
            } else {
                m_warnings.push_back("Failed to build resolved skeleton: " +
                                     (built.error.empty() ? basePath : built.error));
            }
        }
    }

    m_hasMesh = true;
    return true;
}

void AocSceneGraph::ComputeMeshLocalBounds(float outMin[3], float outMax[3]) const {
    if (!m_hasMesh || m_mesh.vertices.empty()) {
        outMin[0] = outMin[1] = outMin[2] = 0.0f;
        outMax[0] = outMax[1] = outMax[2] = 0.0f;
        return;
    }

    outMin[0] = outMin[1] = outMin[2] =  1e30f;
    outMax[0] = outMax[1] = outMax[2] = -1e30f;

    // SmdVertex layout: POSITION0 is stored as 3 decoded floats (x, y, z) on the
    // struct (see SmdParser.h). No decode via HalfToFloat needed at this layer.
    for (const auto& v : m_mesh.vertices) {
        if (v.x < outMin[0]) outMin[0] = v.x;
        if (v.y < outMin[1]) outMin[1] = v.y;
        if (v.z < outMin[2]) outMin[2] = v.z;
        if (v.x > outMax[0]) outMax[0] = v.x;
        if (v.y > outMax[1]) outMax[1] = v.y;
        if (v.z > outMax[2]) outMax[2] = v.z;
    }

    // Guard against degenerate case (all non-finite / no valid vertices).
    if (outMin[0] > outMax[0]) {
        outMin[0] = outMin[1] = outMin[2] = 0.0f;
        outMax[0] = outMax[1] = outMax[2] = 0.0f;
    }
}

bool AocSceneGraph::AttachChild(const std::string& socketName,
                                 const std::string& sourcePath,
                                 const std::vector<uint8_t>& sourceBytes,
                                 GGPKManager* mgr,
                                 std::string& outError) {
    outError.clear();

    if (socketName.empty()) { outError = "Empty socket name"; return false; }
    if (sourcePath.empty())  { outError = "Empty source path"; return false; }
    if (sourceBytes.empty()) { outError = "Source bytes empty for: " + sourcePath; return false; }

    auto endsWith = [](const std::string& s, const char* suffix) -> bool {
        size_t n = std::strlen(suffix);
        if (s.size() < n) return false;
        for (size_t i = 0; i < n; ++i) {
            char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[s.size() - n + i])));
            char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
            if (a != b) return false;
        }
        return true;
    };

    AocChildMesh staging;
    staging.socketName = socketName;
    staging.sourcePath = sourcePath;

    if (endsWith(sourcePath, ".sm")) {
        auto smResult = Parsers::ParseSm(sourceBytes.data(), sourceBytes.size());
        if (!smResult.success) {
            outError = "ParseSm failed: " + sourcePath +
                       (smResult.error.empty() ? "" : (" -- " + smResult.error));
            return false;
        }
        staging.smData = std::move(smResult.value);
        if (staging.smData.smdPath.empty()) {
            outError = ".sm has no smdPath field: " + sourcePath;
            return false;
        }
        auto smdBytes = ReadFromGGPK(mgr, staging.smData.smdPath, staging.warnings, /*optional=*/false);
        if (smdBytes.empty()) {
            outError = "Missing .smd referenced by .sm: " + staging.smData.smdPath;
            return false;
        }
        auto smdResult = Parsers::ParseSmd(smdBytes.data(), smdBytes.size());
        if (!smdResult.success) {
            outError = "ParseSmd failed: " + staging.smData.smdPath +
                       (smdResult.error.empty() ? "" : (" -- " + smdResult.error));
            return false;
        }
        staging.mesh = std::move(smdResult.value);
    } else if (endsWith(sourcePath, ".aoc")) {
        auto tok = Parsers::TokenizeAoc(sourceBytes.data(), sourceBytes.size());
        if (!tok.success) { outError = "Tokenize .aoc failed: " + tok.error; return false; }
        auto leafRes = Parsers::ParseAoc(tok.value);
        if (!leafRes.success) { outError = "Parse .aoc failed: " + leafRes.error; return false; }
        Parsers::AocFile leaf = std::move(leafRes.value);
        leaf.sourcePath = sourcePath;

        auto docRes = Parsers::ResolveExtends(leaf, mgr, nullptr);
        if (!docRes.success) { outError = "Resolve .aoc extends failed: " + docRes.error; return false; }
        Parsers::AocDocument doc = std::move(docRes.value);

        // Phase 4 M3 — preserve parser warnings from the attached .aoc so they
        // surface in the Sockets tab's "Attached" column (orange tooltip).
        // Mirrors AocViewer::DoParsing, which feeds both doc.warnings (resolver
        // level) and doc.merged.warnings (leaf + merged) into m_parseWarnings.
        // Note: ResolveExtends already folds each leaf's `warnings` into
        // `doc.merged.warnings` (prefixed by source path), so we don't need to
        // copy `leaf.warnings` separately.
        for (const auto& w : doc.warnings)        staging.warnings.push_back(w);
        for (const auto& w : doc.merged.warnings) staging.warnings.push_back(w);

        std::string skinPath;
        for (const auto& b : doc.merged.blocks) {
            if (b.skinMesh && !b.skinMesh->skinPath.empty()) {
                skinPath = b.skinMesh->skinPath;
                break;
            }
        }
        if (skinPath.empty()) {
            outError = "Attached .aoc has no SkinMesh.skin path: " + sourcePath;
            return false;
        }

        std::string basePath = skinPath;
        {
            size_t dot = basePath.find_last_of('.');
            size_t slash = basePath.find_last_of("/\\");
            if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
                basePath.resize(dot);
            }
        }

        auto smBytes = ReadFromGGPK(mgr, basePath + ".sm", staging.warnings, /*optional=*/false);
        if (smBytes.empty()) { outError = "Missing .sm: " + basePath + ".sm"; return false; }
        auto smResult = Parsers::ParseSm(smBytes.data(), smBytes.size());
        if (!smResult.success) {
            outError = "ParseSm failed: " + basePath +
                       (smResult.error.empty() ? "" : (" -- " + smResult.error));
            return false;
        }
        staging.smData = std::move(smResult.value);
        if (staging.smData.smdPath.empty()) {
            outError = "Attached .aoc .sm has no smdPath field: " + basePath;
            return false;
        }
        auto smdBytes = ReadFromGGPK(mgr, staging.smData.smdPath, staging.warnings, /*optional=*/false);
        if (smdBytes.empty()) {
            outError = "Missing .smd referenced by attached .sm: " + staging.smData.smdPath;
            return false;
        }
        auto smdResult = Parsers::ParseSmd(smdBytes.data(), smdBytes.size());
        if (!smdResult.success) {
            outError = "ParseSmd failed: " + staging.smData.smdPath +
                       (smdResult.error.empty() ? "" : (" -- " + smdResult.error));
            return false;
        }
        staging.mesh = std::move(smdResult.value);
    } else {
        outError = "Unsupported extension (use .sm or .aoc): " + sourcePath;
        return false;
    }

    // Phase 5.2 — skinning pipeline. Non-fatal: failures leave staging in
    // Rigid mode (old Phase 4 behavior). Must run BEFORE push_back because
    // BuildChildBoneRemap operates on a mutable child reference and may
    // be called again later when parent skeleton changes (not in 5.2, but
    // the method is safe to re-run and re-derives from scratch).
    (void)LoadChildSkeleton(staging, mgr);
    BuildChildBoneRemap(staging);

    // Phase T6 (B4 fix) — atomic swap: detach old attachment only AFTER the
    // new one has been fully parsed + skinned. All early-return paths above
    // leave m_children untouched (staging is a stack local discarded on
    // return), so on-disk JSON and in-memory state cannot diverge.
    DetachChild(socketName);
    m_children.push_back(std::move(staging));
    return true;
}

bool AocSceneGraph::DetachChild(const std::string& socketName) {
    auto it = std::find_if(m_children.begin(), m_children.end(),
                            [&](const AocChildMesh& c){ return c.socketName == socketName; });
    if (it == m_children.end()) return false;
    m_children.erase(it);
    return true;
}

void AocSceneGraph::DetachAllChildren() {
    m_children.clear();
}

AocChildMesh* AocSceneGraph::FindChildBySocket(const std::string& socketName) {
    for (auto& c : m_children) if (c.socketName == socketName) return &c;
    return nullptr;
}

const AocChildMesh* AocSceneGraph::FindChildBySocket(const std::string& socketName) const {
    for (const auto& c : m_children) if (c.socketName == socketName) return &c;
    return nullptr;
}

// Phase 5.2 — discover + parse child .ast. Mirrors LoadMain's two-tier fallback
// (per-mesh <smdDir>/<smBase>.ast, then <smdDir>/rig.ast). Non-fatal on any
// failure — caller observes populated child.warnings and empty child.childAst.
bool AocSceneGraph::LoadChildSkeleton(AocChildMesh& child, GGPKManager* mgr) {
    // 1. Derive basePath (strip extension of last segment).
    std::string basePath = child.sourcePath;
    {
        size_t dot = basePath.find_last_of('.');
        size_t slash = basePath.find_last_of("/\\");
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            basePath.resize(dot);
        }
    }

    // 2. Derive smBase (last path segment) — used for per-mesh .ast name.
    std::string smBase = basePath;
    {
        size_t sBSlash = smBase.find_last_of("/\\");
        if (sBSlash != std::string::npos) {
            smBase = smBase.substr(sBSlash + 1);
        }
    }

    // 3. Derive smdDir (directory containing .smd — where .ast lives).
    std::string smdDir = child.smData.smdPath;
    {
        size_t smdSlash = smdDir.find_last_of("/\\");
        if (smdSlash != std::string::npos) {
            smdDir.resize(smdSlash + 1);
        } else {
            smdDir.clear();
        }
    }

    // 4. Try per-mesh .ast first.
    std::vector<uint8_t> astBytes;
    if (!smBase.empty()) {
        const std::string perMeshAstPath = smdDir + smBase + ".ast";
        astBytes = ReadFromGGPK(mgr, perMeshAstPath, child.warnings, /*optional=*/true);
    }
    // 5. Fallback to shared rig.ast.
    if (astBytes.empty()) {
        const std::string rigPath = smdDir + "rig.ast";
        astBytes = ReadFromGGPK(mgr, rigPath, child.warnings, /*optional=*/true);
    }

    // 6. No .ast found — child is a static prop or foreign-format mesh.
    if (astBytes.empty()) {
        return false;
    }

    // 7. Parse + build resolved skeleton.
    auto astResult = Parsers::ParseAst(astBytes.data(), astBytes.size());
    if (!astResult.success) {
        child.warnings.push_back("Failed to parse child .ast: " + child.sourcePath +
                                 (astResult.error.empty() ? "" : (" -- " + astResult.error)));
        return false;
    }
    child.childAst = std::move(astResult.value);

    auto built = Parsers::BuildResolvedSkeleton(child.childAst);
    if (!built.success) {
        child.warnings.push_back("Failed to build child resolved skeleton: " +
                                 (built.error.empty() ? child.sourcePath : built.error));
        return false;
    }
    child.childResolvedSkel = std::move(built.value);

    // 8. Compute bind-inverse via the existing anonymous-namespace helper.
    //    ComputeBindInverseWorld substitutes identity on singular matrices.
    ComputeBindInverseWorld(child.childResolvedSkel, child.childBindInverse);
    child.childRootBoneIndex = FindRootBoneIndex(child.childResolvedSkel);

    return true;
}

// Phase 5.2 — classify child skin state and build bone remap.
//
// Binary classification per child. Check order (first match wins):
//   1. parent has no skeleton            -> Rigid "parent has no skeleton"
//   2. child has no skeleton             -> Rigid "child has no skeleton"
//   3. mesh has no bone weights          -> Rigid "mesh has no bone weights (static)"
//   4. bone count truncation (warn-only; classification continues on first 256)
//   5. 0 matches after linear scan       -> Rigid "no bones match parent (<N> child bones)"
//   6. >=1 matches                       -> Skinned "skinned <M>/<N>"
//
// When Skinned, boneRemap holds one entry per child bone (index into
// m_resolvedSkeleton.bones, or -1 for unmatched -- unmatched slots get
// socket-world fallback at frame time). When Rigid, boneRemap is cleared.
void AocSceneGraph::BuildChildBoneRemap(AocChildMesh& child) {
    child.boneRemap.clear();
    child.matchedBoneCount = 0;
    child.skinMode = ChildSkinMode::Rigid;
    child.skinModeReason.clear();
    child.isStaticMesh = false;
    child.childLocalToChildSkel.clear();

    // 1. Parent skeleton present?
    if (m_resolvedSkeleton.bones.empty()) {
        child.skinModeReason = "parent has no skeleton";
        return;
    }

    // 2. Child skeleton present?
    if (child.childResolvedSkel.bones.empty()) {
        child.skinModeReason = "child has no skeleton";
        return;
    }

    // 3. Mesh has bone weights? (checked via the decoded vertex layout)
    if (child.mesh.vertexLayout.boneIOff < 0 || child.mesh.vertexLayout.boneWOff < 0) {
        child.isStaticMesh = true;
        child.skinModeReason = "mesh has no bone weights (static)";
        return;
    }

    // 4. Truncation -- warn but continue on the capped set.
    constexpr std::size_t kMaxSkinBones = 256;  // matches MeshShaders.h
    const std::size_t n = child.childResolvedSkel.bones.size();
    const std::size_t capped = (n > kMaxSkinBones) ? kMaxSkinBones : n;
    if (n > kMaxSkinBones) {
        child.warnings.push_back("Child bone count " + std::to_string(n) +
                                 " exceeds kMaxSkinBones (" +
                                 std::to_string(kMaxSkinBones) +
                                 ") -- first " + std::to_string(kMaxSkinBones) +
                                 " bones used.");
    }

    // 5. Linear name match (case-sensitive exact match -- matches parent remap
    //    at AocViewer.cpp:488-498).
    child.boneRemap.reserve(capped);
    for (std::size_t i = 0; i < capped; ++i) {
        const std::string& name = child.childResolvedSkel.bones[i].name;
        int match = -1;
        for (std::size_t j = 0; j < m_resolvedSkeleton.bones.size(); ++j) {
            if (m_resolvedSkeleton.bones[j].name == name) {
                match = static_cast<int>(j);
                break;
            }
        }
        child.boneRemap.push_back(match);
        if (match >= 0) ++child.matchedBoneCount;
    }

    // 6. Classify.
    if (child.matchedBoneCount == 0) {
        child.boneRemap.clear();
        child.skinModeReason = "no bones match parent (" +
                               std::to_string(capped) + " child bones)";
        return;
    }

    child.skinMode = ChildSkinMode::Skinned;
    child.skinModeReason = "skinned " + std::to_string(child.matchedBoneCount) +
                           "/" + std::to_string(capped);

    // V01/V02 meshes: build mesh-local → child-skel remap for vertex BLENDINDICES
    // semantics. DOLm/V3+ meshes have empty boneNames and skip this entirely.
    child.childLocalToChildSkel.clear();
    if (!child.mesh.boneNames.empty() && !child.childResolvedSkel.bones.empty()) {
        child.childLocalToChildSkel.reserve(child.mesh.boneNames.size());
        for (const auto& localName : child.mesh.boneNames) {
            int match = -1;
            for (std::size_t j = 0; j < child.childResolvedSkel.bones.size(); ++j) {
                if (child.childResolvedSkel.bones[j].name == localName) {
                    match = static_cast<int>(j);
                    break;
                }
            }
            child.childLocalToChildSkel.push_back(match);
        }
    }
}

// --- Phase 5.2.1 — transform override helpers ---

bool IsTransformIdentity(const AocChildMesh& c) {
    constexpr float kEps = 1e-6f;
    for (float v : c.overrideT) {
        if (std::fabs(v) > kEps) return false;
    }
    for (float v : c.overrideR) {
        if (std::fabs(v) > kEps) return false;
    }
    for (float v : c.overrideS) {
        if (std::fabs(v - 1.0f) > kEps) return false;
    }
    return true;
}

void ComposeOverrideMatrix(const AocChildMesh& c, float out[16]) {
    // Row-major, row-vector convention:  v' = v x S x R x T
    // where R = Rx x Ry x Rz (intrinsic XYZ Euler).
    //
    // Derivation worked out in the spec (sections 3-4.4). Resulting upper-left 3x3:
    //   r00 =  cy*cz
    //   r01 =  cy*sz
    //   r02 = -sy
    //   r10 =  sx*sy*cz - cx*sz
    //   r11 =  sx*sy*sz + cx*cz
    //   r12 =  sx*cy
    //   r20 =  cx*sy*cz + sx*sz
    //   r21 =  cx*sy*sz - sx*cz
    //   r22 =  cx*cy
    // Scale multiplies each row of R (uniform basis scale).
    // Translation sits in [12], [13], [14].

    constexpr float kPi = 3.14159265358979323846f;
    const float rx = c.overrideR[0] * (kPi / 180.0f);
    const float ry = c.overrideR[1] * (kPi / 180.0f);
    const float rz = c.overrideR[2] * (kPi / 180.0f);
    const float cx = std::cos(rx), sx = std::sin(rx);
    const float cy = std::cos(ry), sy = std::sin(ry);
    const float cz = std::cos(rz), sz = std::sin(rz);

    const float r00 =  cy * cz;
    const float r01 =  cy * sz;
    const float r02 = -sy;
    const float r10 =  sx * sy * cz - cx * sz;
    const float r11 =  sx * sy * sz + cx * cz;
    const float r12 =  sx * cy;
    const float r20 =  cx * sy * cz + sx * sz;
    const float r21 =  cx * sy * sz - sx * cz;
    const float r22 =  cx * cy;

    const float sX = c.overrideS[0];
    const float sY = c.overrideS[1];
    const float sZ = c.overrideS[2];

    out[ 0] = sX * r00;  out[ 1] = sX * r01;  out[ 2] = sX * r02;  out[ 3] = 0.0f;
    out[ 4] = sY * r10;  out[ 5] = sY * r11;  out[ 6] = sY * r12;  out[ 7] = 0.0f;
    out[ 8] = sZ * r20;  out[ 9] = sZ * r21;  out[10] = sZ * r22;  out[11] = 0.0f;
    out[12] = c.overrideT[0];
    out[13] = c.overrideT[1];
    out[14] = c.overrideT[2];
    out[15] = 1.0f;
}

// Phase 5.2.1 / T5 — multi-root .ast defense.
//
// ResolvedBone::arrayParent is std::size_t with root sentinel
// static_cast<std::size_t>(-1) (== SIZE_MAX). Scan for the first bone whose
// arrayParent equals that sentinel (semantically "arrayParent < 0" when read
// as signed). Valid .ast files always contain at least one such bone; the
// fallback to 0 only triggers on malformed files where BuildResolvedSkeleton
// failed to populate parent links.
int FindRootBoneIndex(const Parsers::ResolvedSkeleton& skel) {
    for (std::size_t i = 0; i < skel.bones.size(); ++i) {
        if (skel.bones[i].arrayParent == static_cast<std::size_t>(-1)) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

} // namespace Aoc
} // namespace GGPKExp
