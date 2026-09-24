#include "AocParticleSystem.h"
#include "../../core/GGPKManager.h"

#include <algorithm>
#include <unordered_set>

namespace GGPKExp { namespace Aoc {

namespace {

std::string FilenameTailOf(const std::string& p) {
    const auto slash = p.find_last_of("/\\");
    return (slash == std::string::npos) ? p : p.substr(slash + 1);
}

// Look up a bone by name in the resolved skeleton. Returns -1 if not found.
// Unified index scheme: [0..bones.size()-1] = real bones;
// [bones.size()..bones.size()+virtualBones.size()-1] = virtual bones (sockets
// augmented into the skeleton by AocSkeletonAugmenter). Downstream render code
// decodes via (idx < bones.size()) test.
int ResolveBoneNameIndex(const Parsers::ResolvedSkeleton& skel,
                         const std::string&                boneName) {
    for (std::size_t i = 0; i < skel.bones.size(); ++i) {
        if (skel.bones[i].name == boneName) return static_cast<int>(i);
    }
    for (std::size_t i = 0; i < skel.virtualBones.size(); ++i) {
        if (skel.virtualBones[i].name == boneName) {
            return static_cast<int>(skel.bones.size() + i);
        }
    }
    return -1;
}

// Look up a bone group by name on the ClientAnimationController. Returns the
// bone index of the FIRST bone name in the group (resolved against skel), or
// -1 if the group is unknown/empty/unresolvable. `clientAnim` may be null.
int ResolveBoneGroupFirstIndex(const Parsers::ClientAnimCtrl*   clientAnim,
                               const Parsers::ResolvedSkeleton& skel,
                               const std::string&               groupName) {
    if (!clientAnim) return -1;
    for (const Parsers::BoneGroup& g : clientAnim->boneGroups) {
        if (g.name == groupName) {
            if (g.boneNames.empty()) return -1;
            return ResolveBoneNameIndex(skel, g.boneNames.front());
        }
    }
    return -1;
}

} // anonymous namespace

void AocParticleSystem::Populate(const Parsers::ParticleEffectsBlock& block,
                                 const Parsers::ResolvedSkeleton&     skel,
                                 const Parsers::ClientAnimCtrl*       clientAnim,
                                 GGPKManager*                         mgr) {
    Clear();
    m_skippedUnknownTypes = block.skippedUnknownTypes;

    m_emitters.reserve(block.clips.size());
    for (const Parsers::ParticleEffectClip& c : block.clips) {
        AocParticleEmitter e;
        e.filename       = c.filename;
        e.filenameTail   = FilenameTailOf(c.filename);
        e.boneGroup      = c.boneGroup;
        e.boneName       = c.boneName;
        e.animationName  = c.animationName;
        e.time           = c.time;

        // Bone resolution: prefer boneGroup, fall back to boneName.
        if (!c.boneGroup.empty()) {
            e.resolvedBoneIndex = ResolveBoneGroupFirstIndex(clientAnim, skel, c.boneGroup);
        } else if (!c.boneName.empty()) {
            e.resolvedBoneIndex = ResolveBoneNameIndex(skel, c.boneName);
        }

        // Warn when both are set (spec: bone_group wins).
        if (!c.boneGroup.empty() && !c.boneName.empty()) {
            m_warnings.push_back(
                "event for '" + e.filenameTail +
                "' specifies both bone_group and bone_name — using bone_group");
        }

        // Existence check. mgr may be null in degenerate paths; treat null as
        // "can't tell" → petExists=false (behaves like missing).
        e.petExists = (mgr != nullptr &&
                       mgr->FindNodeByPath(c.filename) != nullptr);

        m_emitters.push_back(std::move(e));
    }
}

void AocParticleSystem::Clear() {
    m_emitters.clear();
    m_warnings.clear();
    m_errors.clear();
    m_skippedUnknownTypes = 0;
}

void AocParticleSystem::ResetTriggers() {
    for (auto& e : m_emitters) e.lastTriggeredTime = -1.0e9f;
}

std::size_t AocParticleSystem::MissingCount() const {
    std::size_t n = 0;
    for (const auto& e : m_emitters) if (!e.petExists) ++n;
    return n;
}

std::size_t AocParticleSystem::UniqueFilenames() const {
    std::unordered_set<std::string> seen;
    seen.reserve(m_emitters.size());
    for (const auto& e : m_emitters) seen.insert(e.filename);
    return seen.size();
}

}} // namespace GGPKExp::Aoc
