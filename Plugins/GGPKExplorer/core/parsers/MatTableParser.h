#pragma once

#include "../Result.h"
#include <cstdint>
#include <string>
#include <vector>

namespace GGPKExp { namespace Parsers {

// Section 1 — main mat entry. Key is material identity hash (sorted ascending),
// value is the compiled-shader-graph identifier for that material.
struct MatEntry {
    uint64_t keyHash = 0;
    uint64_t valueHash = 0;
};

// Section 2 — graph combination with inline parameters.
//   paramOffset / paramCount reference a slice of MatTableData::paramIds.
//   Records with paramCount == 0 are null markers written verbatim by the engine
//   (the loader skips them when counting "valid" combos, but keeps them in order).
struct GraphComboEntry {
    uint64_t comboKey = 0;
    uint32_t paramOffset = 0;
    uint16_t paramCount = 0;
};

// Parsed mat_table.dat layout (file version == 2).
// Absence of this file or a wrong version triggers the engine's
// `[MAT] (...) will cause shader recompile on most shaders` warning path.
struct MatTableData {
    uint32_t version = 0;                          // expected == 2
    std::vector<MatEntry>        mats;             // Section 1
    std::vector<GraphComboEntry> combos;           // Section 2 (all, incl. null markers)
    std::vector<uint16_t>        paramIds;         // Section 3 — referenced by combos

    // Number of combos with at least one inline parameter (paramCount > 0).
    // Matches the "N graph combinations have inlined parameters" count logged
    // by the engine at load time.
    uint32_t ValidCombosCount() const {
        uint32_t n = 0;
        for (const auto& c : combos)
            if (c.paramCount > 0) ++n;
        return n;
    }
};

Result<MatTableData> ParseMatTable(const uint8_t* data, size_t size);

}} // namespace GGPKExp::Parsers
