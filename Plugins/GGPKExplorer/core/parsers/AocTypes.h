#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace GGPKExp { namespace Parsers {

// ---------------------------------------------------------------------
// One `key = value` line of a block body. Children are the lines indented
// below it.
// ---------------------------------------------------------------------
struct AocRawNode {
    std::string              key;
    std::string              value;
    std::vector<AocRawNode>  children;
    std::size_t              sourceLine = 0;
    std::string              sourceAocPath;  // filled during resolver merge
};

// ---------------------------------------------------------------------
// A single top-level block (ClientAnimationController, SkinMesh, ...).
// ---------------------------------------------------------------------
struct AocBlock {
    std::string              name;
    std::vector<AocRawNode>  properties;
    std::size_t              sourceLine = 0;
    std::string              sourceAocPath;
};

// ---------------------------------------------------------------------
// A fully parsed .aoc file (after parser, before resolver).
// ---------------------------------------------------------------------
struct AocFile {
    int                          version = 0;
    std::string                  extendsPath;   // raw string from "extends "..." "; "Metadata/Parent" = sentinel
    std::vector<AocBlock>        blocks;
    std::string                  sourcePath;    // GGPK path for provenance
};

// ---------------------------------------------------------------------
// After AocResolver: extends chain merged, provenance preserved per-item.
// ---------------------------------------------------------------------
struct AocDocument {
    AocFile                      merged;
    std::vector<std::string>     inheritanceChain;  // index 0 = leaf, last = oldest ancestor
    std::vector<std::string>     warnings;          // resolver-level warnings (cycles, missing parents, depth exceeded)
};

}} // namespace GGPKExp::Parsers
