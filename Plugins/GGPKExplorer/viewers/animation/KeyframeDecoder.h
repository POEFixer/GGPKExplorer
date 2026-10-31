#pragma once

#include "AnimationTypes.h"
#include "../../core/parsers/AstParser.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GGPKExp { namespace Animation {

// Decompresses the Oodle-packed keyframe tail blob once per skeleton, then
// parses individual animations into typed AnimationClip on demand.
//
// Thread-safety: single-threaded; call from the ImGui main thread only.
//
// Lazy decode pattern: DecodeBlobIfNeeded() is cheap to call repeatedly;
// the first call incurs ~50-150 ms Oodle cost (monster rig), subsequent
// calls are no-ops.
//
// Wire-format reference: BONES.md §5.3 (per-bone header + A..F arrays),
// §5.3.5.1 (Oodle decompression model).
class KeyframeDecoder {
public:
    // One-time decompression of skel.animationTail.compressedChunk into
    // m_blob. Idempotent. Returns false + populates *warning on failure
    // (e.g. Oodle not loaded, decompress size mismatch).
    //
    // For version <= 7 files, reads from skel.rawInlineKeyframeBytes instead
    // of Oodle-decompressing (keyframes are uncompressed inline).
    bool DecodeBlobIfNeeded(const Parsers::AstSkeleton& skel,
                            std::string* warning = nullptr);

    // Parse one animation's keyframes from the decompressed blob (or inline
    // bytes for v<=7) into the output clip. Populates clip.tracks with
    // animBoneCount entries.
    //
    // Verifies cursor-consumption invariant: exactly anim.dataSize bytes
    // consumed (game-enforced; mismatch -> warning, partial clip kept).
    //
    // Returns true on successful parse (possibly with non-fatal warnings
    // in clip.warnings); false on fatal error (returns partial clip + sets *warning).
    bool ParseAnimation(const Parsers::AstSkeleton& skel,
                        std::size_t animIndex,
                        AnimationClip& out,
                        std::string* warning = nullptr);

private:
    std::vector<uint8_t> m_blob;                // decompressed keyframe bytes
    bool m_blobReady = false;
    bool m_oodleAttempted = false;              // prevents repeated failure loops

    // Parses one BoneTrack from the cursor. Reads 29/33-byte per-bone header
    // + 6 arrays A..F. Advances cursor by the total per-bone stride.
    // Returns true on success; false + populates *warn on malformed input.
    bool ParseBoneTrack(const uint8_t* base,
                        std::size_t& cursor,
                        std::size_t limit,
                        uint8_t version,
                        BoneTrack& out,
                        std::string* warn);
};

}} // namespace GGPKExp::Animation
