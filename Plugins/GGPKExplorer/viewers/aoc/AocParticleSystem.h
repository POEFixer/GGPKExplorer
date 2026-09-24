#pragma once
#include "../../core/parsers/AocTypes.h"
#include "../../core/parsers/AstSkeletonBuilder.h"   // ResolvedSkeleton, ResolvedBone
#include <cstdint>
#include <string>
#include <vector>

namespace GGPKExp {
class GGPKManager;
namespace Aoc {

struct AocParticleEmitter {
    // Identity
    std::string filename;        // .pet path, as written in the .aoc
    std::string filenameTail;    // e.g. "gold_drop_root.pet" — cached for label render
    bool        petExists = false;

    // Binding
    std::string boneGroup;       // empty if boneName used
    std::string boneName;        // empty if boneGroup used
    int         resolvedBoneIndex = -1;   // -1 = unresolvable (gizmo hidden)

    // Timeline
    std::string animationName;   // owner animation clip name (may be "" for flat form)
    float       time = 0.0f;

    // Runtime state (updated by the trigger loop in AocViewer_3DPreview.cpp).
    float       lastTriggeredTime = -1.0e9f;
};

class AocParticleSystem {
public:
    // Resolve the parsed block into emitter records. Call once per DoParsing.
    //
    //   `skel`       — skeleton (provides bone name → index).
    //   `clientAnim` — ClientAnimationController block (provides bone groups).
    //                  May be nullptr if the .aoc has no CAC block — in which
    //                  case bone_group lookups fail with resolvedBoneIndex=-1
    //                  and the emitter simply isn't drawn (still listed in
    //                  Diagnostic with [UNRESOLVED BONE]).
    //   `mgr`        — used to check `.pet` existence via FindNodeByPath.
    void Populate(const Parsers::ParticleEffectsBlock& block,
                  const Parsers::ResolvedSkeleton&     skel,
                  const Parsers::ClientAnimCtrl*       clientAnim,
                  GGPKManager*                         mgr);

    void Clear();

    // Set lastTriggeredTime = -inf on all emitters. Called by the viewer on
    // animation clip change and on backward seek / loop wrap.
    void ResetTriggers();

    const std::vector<AocParticleEmitter>& Emitters()        const { return m_emitters; }
    std::vector<AocParticleEmitter>&       MutableEmitters()       { return m_emitters; }

    std::size_t EmitterCount()      const { return m_emitters.size(); }
    std::size_t MissingCount()      const;
    std::size_t UniqueFilenames()   const;
    std::size_t SkippedUnknownTypes() const { return m_skippedUnknownTypes; }

    const std::vector<std::string>& Warnings() const { return m_warnings; }
    const std::vector<std::string>& Errors()   const { return m_errors;   }

private:
    std::vector<AocParticleEmitter>  m_emitters;
    std::vector<std::string>         m_warnings;
    std::vector<std::string>         m_errors;
    std::size_t                      m_skippedUnknownTypes = 0;
};

}} // namespace GGPKExp::Aoc
