#pragma once
#include "AocTypes.h"
#include <cstdint>
#include <string>
#include <vector>

namespace GGPKExp { namespace Parsers {

// Parse a raw .ot or .otc file (UTF-16LE + BOM accepted; bare UTF-8 accepted).
// Returns false on fatal errors only (unreadable bytes). Permissive: malformed
// Transitionable entries produce warnings in out.warnings without failing the parse.
// The caller owns `out`; on success, out.transitionable is non-null iff the file
// contained a `Transitionable { ... }` block.
bool ParseOtFile(const std::vector<uint8_t>& bytes,
                 const std::string& sourcePath,
                 OtFile& out);

// Merge data from two parsed files into one `merged`. Either argument's
// transitionable may be null. When both are present, declaredNumStates becomes
// the max, handlers are unioned (deduped by `key`, .otc wins on conflict),
// numStates is recomputed from the merged set, and both source paths are
// preserved in merged.transitionable->sourceOtPath / sourceOtcPath.
void MergeOtFiles(const OtFile& ot, const OtFile& otc, OtFile& merged);

}} // namespace GGPKExp::Parsers
