#pragma once

#include "../Result.h"
#include "AocTypes.h"
#include <string>

namespace GGPKExp { class GGPKManager; }

namespace GGPKExp { namespace Parsers {

// Merges the extends chain starting from `leaf`. Uses GGPKManager to resolve
// parent paths ("Metadata/Foo" → "Metadata/Foo.aoc"), parse them (tokenizer+parser),
// and merge with child-wins semantics.
//
// The special path "Metadata/Parent" is a sentinel meaning "no actual parent" —
// it short-circuits the chain walk without warning.
//
// Depth limit: 16. Exceeding → soft warning, merge truncated.
// Cycle detection: via visited-path set. Cycle → soft warning, merge stops.
// Missing parent or parse failure → soft warning, merge continues with what we have.
//
// Returns AocDocument with merged AocFile, inheritance chain, and warnings.
Result<AocDocument> ResolveExtends(const AocFile& leaf, GGPKManager* ggpkMgr);

// The model a merged .aoc names: the `skin` of its SkinMesh block (a .sm path,
// possibly without the extension) and the `skeleton` of its
// ClientAnimationController block (an .ast path). Empty when absent.
struct AocModelRefs {
    std::string skinPath;
    std::string skeletonPath;
};
AocModelRefs FindModelRefs(const AocFile& merged);

}} // namespace GGPKExp::Parsers
