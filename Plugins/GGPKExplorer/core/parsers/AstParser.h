#pragma once

#include "../Result.h"
#include <cstdint>
#include <string>
#include <vector>

namespace GGPKExp { namespace Parsers {

// AstBone: one node in a First-Child / Next-Sibling (FCNS) linked tree.
// See BONES.md Section 1.2 for the full spec.
//
// FCNS encoding: two uint8 pointers on the wire, both into the bone array:
//   - nextSiblingIndex: index of this bone's next sibling under the same parent
//                       (0xFF = last child of its parent, chain terminator)
//   - firstChildIndex:  index of this bone's first child
//                       (0xFF = leaf, no children)
// The parent of a bone is derived by walking the tree; it is NOT stored directly.
// Verified against live game memory in Sprint 2/3 (humanoid root -> hip -> ...).
struct AstBone {
    std::string name;
    uint8_t nextSiblingIndex = 0xFF;  // FCNS pointer: 0xFF = last child of its parent (no next sibling)
    uint8_t firstChildIndex  = 0xFF;  // FCNS pointer: 0xFF = leaf (no first child)
    uint8_t flag = 0;                 // version>=0x08; for v<=0x0B masked with & 1; for v==0x0C all 8 bits used
    float transform[16] = {};         // 4x4 row-major local transform matrix (translation in [12][13][14])
};

// One animation record — Phase 4.1 parses header fields only.
// Keyframe data lives in AstSkeleton::animationTail.compressedChunk (Oodle-
// compressed, decoded in Phase 4.2). Each animation's dataOffset / dataSize
// index into the decompressed blob.
//
// Wire-format spec: BONES.md §5.2 (header record, 4 version branches).
// Verified via IDA sub_7FF7A6670770 in POE2 v0.4.0i.
struct AstAnimation {
    std::string name;                    // primary name, length-prefixed ASCII (e.g. "idle_01")
    std::string name2;                   // optional secondary name; present only if version >= 11
    char        playbackType = 'n';      // 'l' = loop, 'n' = none, 'o' = once
    uint8_t     animBoneCount = 0;       // header byte 0 — bones affected by this animation
    uint8_t     unknown_b74 = 0;         // header byte 1 — always 0 in sampled files; preserve verbatim
    uint8_t     fps = 30;                // header byte 2 — frames-per-second (canonical 30; uint8_t on wire). Default value applies only to default-constructed instances; ParseOneAnimation overwrites unconditionally.
    uint8_t     compressed = 0;          // header byte 4 — present only if version >= 10; 0 otherwise
    uint32_t    dataOffset = 0;          // offset into decompressed keyframe blob (uint32 LE)
    uint32_t    dataSize = 0;            // size in decompressed keyframe blob (uint32 LE)
};

// Tail block trailer for the animation section — populated only for
// version >= 8. Contains the Oodle-compressed keyframe blob; decompression
// and per-bone keyframe parsing are Phase 4.2 concerns.
//
// Wire-format spec: BONES.md §5.3.5.
struct AstAnimationTail {
    uint32_t sizeUncompressedTotal = 0;  // total uncompressed size of combined keyframe stream
    uint32_t sizeCompressed = 0;         // size of Oodle-compressed chunk
    uint32_t sizeIndex = 0;              // size of index chunk
    std::vector<uint8_t> indexChunk;     // raw index bytes — copied verbatim from file
    std::vector<uint8_t> compressedChunk;// raw Oodle-compressed bytes — DO NOT decompress in Phase 4.1
};

// AST parser interface. Format spec in Plugins/GGPKExplorer/docs/BONES.md.
// Verified against game ParseAstFile at IDA 0x7FF7A6670C20 (POE2 v0.4.0i).
struct AstSkeleton {
    uint8_t  version = 0;              // format version byte (0x02..0x0C observed; game validates (version-2) <= 10)
    uint8_t  boneCountHeader = 0;      // bone count field from header (authoritative)
    uint8_t  auxBoneThreshold = 0;     // header byte 2: aux-bone count threshold (safety check in game)
    uint32_t animationCount = 0;       // uint32 at header offset 3 (drives animations-section loop in ParseAstFile — see BONES.md §1.1/§1.4). Character rigs typically 1; monster rigs up to 31. Phase 4 will parse the records; Phase 3 leaves them unread.
    std::vector<AstBone> bones;
    std::vector<AstAnimation> animations;

    // Populated for version >= 8; default-constructed (all-zero fields, empty
    // vectors) for earlier versions. See BONES.md §5.3.5.
    AstAnimationTail animationTail;

    // Non-fatal animation-section warning. Empty string = animations parsed
    // cleanly. Populated by ParseAst when the lenient animation loop
    // truncated early due to a malformed record. Bones remain valid
    // regardless of this field. See BONES.md §5.6 for lenient-mode contract.
    std::string animationParseWarning;

    // For version <= 7 files, keyframes live inline in the file bytes (not
    // in the tail blob). Phase 4.1 SkipInlineKeyframeBlock only advances
    // the cursor. Phase 4.2 needs to re-parse these bytes for playback, so
    // ParseAst populates this field with the raw inline-keyframe region
    // for v<=7 files. Empty for version >= 8 (keyframes live in
    // animationTail.compressedChunk). See BONES.md §5.1 for the
    // inline-keyframe driver loop.
    std::vector<uint8_t> rawInlineKeyframeBytes;
};

Result<AstSkeleton> ParseAst(const uint8_t* data, size_t size);

}} // namespace GGPKExp::Parsers
