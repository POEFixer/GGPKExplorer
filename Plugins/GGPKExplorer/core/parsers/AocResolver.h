#pragma once

#include "../Result.h"
#include "AocTypes.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace GGPKExp { class GGPKManager; }

namespace GGPKExp { namespace Parsers {

// Cache of already-parsed .aoc files, keyed by normalized GGPK path.
// Pass the same cache instance across related resolver calls to avoid
// re-parsing shared parents.
using AocResolverCache = std::unordered_map<std::string, std::shared_ptr<AocFile>>;

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
Result<AocDocument> ResolveExtends(const AocFile&    leaf,
                                    GGPKManager*      ggpkMgr,
                                    AocResolverCache* cache = nullptr);

}} // namespace GGPKExp::Parsers
