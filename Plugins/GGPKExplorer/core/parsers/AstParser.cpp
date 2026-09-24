#include "AstParser.h"
#include "../BinaryReader.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace GGPKExp { namespace Parsers {

// .ast file parser. Format documented in Plugins/GGPKExplorer/docs/BONES.md Section 1.
// Verified against game function ParseAstFile at IDA 0x7FF7A6670C20 (POE2 v0.4.0i).
// Game ASLR delta: game_addr = ida_addr - 0x1A48F0000.

namespace {
// Fixed portion of an aux-bone on-disk record, version-keyed. Verified via IDA
// decompile of ParseAstFile (0x7FF7A6670C20) — the aux-bone loop body is a
// switch on (char)version that advances the file cursor by a fixed stride:
//   v2..v3   → 43 bytes
//   v4..v5   → 51 bytes
//   v6       → 52 bytes
//   v7..v8   → 56 bytes
//   v9..v12  → 60 bytes
// AFTER the fixed portion, a variable-length ASCII name follows whose length
// is stored at byte 0 of the fixed record (decompile reads
// `v26 = LOBYTE(_mm_cvtsi128_si32(v24))` then `memcpy(buf, v8, v26); v8 += v26;`).
// Total per-record on-disk size = AuxBoneFixedSize(version) + record[0].
//
// (NOTE: BONES.md §1.4 says "92 bytes" — that's the in-memory output struct
//  produced by onAuxBoneRecord, NOT the on-disk record.)
constexpr size_t AuxBoneFixedSize(uint8_t version) {
    if (version <= 0x03) return 43;
    if (version <= 0x05) return 51;
    if (version == 0x06) return 52;
    if (version <= 0x08) return 56;
    return 60; // v9..v12 (and forward-compatible default for any higher version)
}

// Version >= 0x08 inserts one extra byte (a flag) between name_len and the name.
// Earlier versions place the name directly after name_len.
constexpr bool HasNameFlagByte(uint8_t version) {
    return version >= 0x08;
}

// Forward declarations for the animation-section helpers defined below.
Result<AstAnimation>     ParseOneAnimation(BinaryReader& reader, uint8_t version);
Result<AstAnimationTail> ParseAnimationTail(BinaryReader& reader);
Result<void>             SkipInlineKeyframeBlock(BinaryReader& reader, uint8_t animBoneCount);

// Read a length-prefixed ASCII string: [uint8_t len][chars...].
// No null terminator in the wire format. Returns an empty string on zero length.
// The length byte is consumed by the caller (animation headers read the length
// as part of the fixed header, then call ReadAsciiBody() with that length).
Result<std::string> ReadAsciiBody(BinaryReader& reader, uint8_t len) {
    if (len == 0) return Result<std::string>::Ok(std::string());
    if (reader.Remaining() < len)
        return Result<std::string>::Fail("Truncated string bytes");
    return Result<std::string>::Ok(reader.ReadString(len));
}

// Validate that a playback byte is one of 'l' / 'n' / 'o' per BONES.md §5.2.
// Any other value is a hard error (the game throws at this point too).
bool IsValidPlayback(uint8_t b) {
    return b == 'l' || b == 'n' || b == 'o';
}

// Animation header parser — mirrors IDA sub_7FF7A6670770 per BONES.md §5.2.
// Version-branched layout:
//   v<8:    5-byte header  (animBoneCount, unknown_b74, fps, playback, string1_length)
//   v8-9:  13-byte header  (+dataOffset:u32, +dataSize:u32)
//   v10:   14-byte header  (v8-9 + compressed:u8 inserted at byte 4, then string1_length,
//                           then dataOffset / dataSize at bytes 6..13)
//   v11+:  15-byte header  (v10 + string2_length:u8 inserted at byte 6)
// After the header, name1 bytes follow (length = string1_length), then
// optional name2 bytes (length = string2_length, v>=11 only).
Result<AstAnimation> ParseOneAnimation(BinaryReader& reader, uint8_t version) {
    AstAnimation anim;
    uint8_t animBoneCount  = 0;
    uint8_t unknown_b74    = 0;
    uint8_t fps            = 30;
    uint8_t playback       = 0;
    uint8_t compressed     = 0;
    uint8_t string1_length = 0;
    uint8_t string2_length = 0;
    uint32_t dataOffset    = 0;
    uint32_t dataSize      = 0;

    if (version < 0x08) {
        // v<8: 5-byte fixed header.
        if (reader.Remaining() < 5)
            return Result<AstAnimation>::Fail("Truncated v<8 header");
        animBoneCount  = reader.Read<uint8_t>();
        unknown_b74    = reader.Read<uint8_t>();
        fps            = reader.Read<uint8_t>();
        playback       = reader.Read<uint8_t>();
        string1_length = reader.Read<uint8_t>();
    } else if (version <= 0x09) {
        // v8-9: 13-byte fixed header.
        if (reader.Remaining() < 13)
            return Result<AstAnimation>::Fail("Truncated v8-9 header");
        animBoneCount  = reader.Read<uint8_t>();
        unknown_b74    = reader.Read<uint8_t>();
        fps            = reader.Read<uint8_t>();
        playback       = reader.Read<uint8_t>();
        string1_length = reader.Read<uint8_t>();
        dataOffset     = reader.Read<uint32_t>();
        dataSize       = reader.Read<uint32_t>();
    } else if (version == 0x0A) {
        // v10: 14-byte fixed header (inserts `compressed` at byte 4).
        if (reader.Remaining() < 14)
            return Result<AstAnimation>::Fail("Truncated v10 header");
        animBoneCount  = reader.Read<uint8_t>();
        unknown_b74    = reader.Read<uint8_t>();
        fps            = reader.Read<uint8_t>();
        playback       = reader.Read<uint8_t>();
        compressed     = reader.Read<uint8_t>();
        string1_length = reader.Read<uint8_t>();
        dataOffset     = reader.Read<uint32_t>();
        dataSize       = reader.Read<uint32_t>();
    } else {
        // v11+ (observed: v0x0B, v0x0C): 15-byte fixed header.
        if (reader.Remaining() < 15)
            return Result<AstAnimation>::Fail("Truncated v11+ header");
        animBoneCount  = reader.Read<uint8_t>();
        unknown_b74    = reader.Read<uint8_t>();
        fps            = reader.Read<uint8_t>();
        playback       = reader.Read<uint8_t>();
        compressed     = reader.Read<uint8_t>();
        string1_length = reader.Read<uint8_t>();
        string2_length = reader.Read<uint8_t>();
        dataOffset     = reader.Read<uint32_t>();
        dataSize       = reader.Read<uint32_t>();
    }

    if (!reader.IsValid())
        return Result<AstAnimation>::Fail("Reader invalid after header");

    if (!IsValidPlayback(playback))
        return Result<AstAnimation>::Fail(
            "Invalid playback byte 0x" + std::to_string(playback));

    // compressed must be 0 or 1 for v>=10 (versions without the field leave it 0).
    if (version >= 0x0A && compressed > 1)
        return Result<AstAnimation>::Fail(
            "Invalid compressed byte 0x" + std::to_string(compressed));

    // Read name1 (no null terminator on the wire).
    auto name1Result = ReadAsciiBody(reader, string1_length);
    if (!name1Result)
        return Result<AstAnimation>::Fail("name1: " + name1Result.error);

    // Read name2 (v>=11 only, and only when string2_length > 0).
    std::string name2;
    if (version >= 0x0B && string2_length != 0) {
        auto name2Result = ReadAsciiBody(reader, string2_length);
        if (!name2Result)
            return Result<AstAnimation>::Fail("name2: " + name2Result.error);
        name2 = std::move(name2Result.value);
    }

    anim.name          = std::move(name1Result.value);
    anim.name2         = std::move(name2);
    anim.playbackType  = static_cast<char>(playback);
    anim.animBoneCount = animBoneCount;
    anim.unknown_b74   = unknown_b74;
    anim.fps           = fps;
    anim.compressed    = compressed;
    anim.dataOffset    = dataOffset;
    anim.dataSize      = dataSize;

    return Result<AstAnimation>::Ok(std::move(anim));
}

// Tail block parser — reads 12-byte trailer + index chunk + compressed chunk.
// See BONES.md §5.3.5.
//
// Layout:
//   [uint32 sizeUncompressedTotal][uint32 sizeCompressed][uint32 sizeIndex]
//   [indexChunk : sizeIndex bytes]
//   [compressedChunk : sizeCompressed bytes]
// The compressed chunk is NOT Oodle-decoded here — raw bytes stored for Phase 4.2.
Result<AstAnimationTail> ParseAnimationTail(BinaryReader& reader) {
    if (reader.Remaining() < 12)
        return Result<AstAnimationTail>::Fail("Truncated trailer");

    AstAnimationTail tail;
    tail.sizeUncompressedTotal = reader.Read<uint32_t>();
    tail.sizeCompressed        = reader.Read<uint32_t>();
    tail.sizeIndex             = reader.Read<uint32_t>();

    if (!reader.IsValid())
        return Result<AstAnimationTail>::Fail("Reader invalid after trailer");

    if (reader.Remaining() < tail.sizeIndex)
        return Result<AstAnimationTail>::Fail("Truncated index chunk");
    tail.indexChunk = reader.ReadBytes(tail.sizeIndex);

    if (reader.Remaining() < tail.sizeCompressed)
        return Result<AstAnimationTail>::Fail("Truncated compressed chunk");
    tail.compressedChunk = reader.ReadBytes(tail.sizeCompressed);

    if (!reader.IsValid())
        return Result<AstAnimationTail>::Fail("Reader invalid after chunk copy");

    return Result<AstAnimationTail>::Ok(std::move(tail));
}

// For version <= 7, each animation record is followed by an inline keyframe
// block consumed by sub_7FF7A6670050 (BONES.md §5.3). Phase 4.1 does NOT
// interpret keyframes — this helper merely advances the cursor past the
// block by reading the size metadata and skipping the payload. Phase 4.2
// will re-parse from file if it needs the actual data.
//
// Per-bone block: 29-byte header (flags + callbackArg + 6 uint32 counts) +
//   6 arrays (A=16B*count, B=20B*count, C=16B*count, D=16B*count,
//             E=20B*count, F=16B*count).
// Iterated animBoneCount times per animation record. The legacy XOR-decode
// path for version <= 4 (BONES.md §5.3) is irrelevant here — we skip raw
// bytes regardless of any obfuscation applied to array B.
Result<void> SkipInlineKeyframeBlock(BinaryReader& reader, uint8_t animBoneCount) {
    constexpr size_t kFlagsByteSize     = 1;
    constexpr size_t kCallbackArgSize   = 4;
    constexpr size_t kPerBoneHeaderSize = kFlagsByteSize + kCallbackArgSize + 6 * sizeof(uint32_t); // = 29
    constexpr size_t kStride16 = 16;
    constexpr size_t kStride20 = 20;

    for (uint8_t b = 0; b < animBoneCount; ++b) {
        if (reader.Remaining() < kPerBoneHeaderSize)
            return Result<void>::Fail(
                "Truncated inline keyframe header (bone " + std::to_string(b) + ")");

        reader.Skip(kFlagsByteSize + kCallbackArgSize); // ignored in skip mode
        uint32_t kfCountA = reader.Read<uint32_t>();
        uint32_t kfCountB = reader.Read<uint32_t>();
        uint32_t kfCountC = reader.Read<uint32_t>();
        uint32_t kfCountD = reader.Read<uint32_t>();
        uint32_t kfCountE = reader.Read<uint32_t>();
        uint32_t kfCountF = reader.Read<uint32_t>();

        if (!reader.IsValid())
            return Result<void>::Fail(
                "Reader invalid in inline keyframe header (bone " + std::to_string(b) + ")");

        // Each count is u32, so worst case the sum reaches ~4e9 * 20 = 80e9
        // bytes. Use uint64_t arithmetic to avoid silent overflow before the
        // bounds check.
        const uint64_t payloadSize =
            static_cast<uint64_t>(kfCountA) * kStride16 +
            static_cast<uint64_t>(kfCountB) * kStride20 +
            static_cast<uint64_t>(kfCountC) * kStride16 +
            static_cast<uint64_t>(kfCountD) * kStride16 +
            static_cast<uint64_t>(kfCountE) * kStride20 +
            static_cast<uint64_t>(kfCountF) * kStride16;

        if (payloadSize > static_cast<uint64_t>(reader.Remaining()))
            return Result<void>::Fail(
                "Truncated inline keyframe payload (bone " + std::to_string(b) + ")");

        reader.Skip(static_cast<size_t>(payloadSize));
    }
    return Result<void>::Ok();
}

} // anonymous namespace

Result<AstSkeleton> ParseAst(const uint8_t* data, size_t size) {
    if (!data || size < 8)
        return Result<AstSkeleton>::Fail("Data too small for AST header");

    BinaryReader reader(data, size);
    AstSkeleton result;

    // Header layout: see BONES.md Section 1.1.
    uint8_t version          = reader.Read<uint8_t>();
    uint8_t boneCount        = reader.Read<uint8_t>();
    uint8_t auxBoneThreshold = reader.Read<uint8_t>();
    uint32_t attachmentGroups = reader.Read<uint32_t>();
    // Byte 7: aux-section count. Per BONES.md §1.4 / §5.5, when byte7 > 0 the
    // file contains an aux-bone preamble of (92 * byte7) bytes between the
    // primary-bone records and the animation section. Observed 0 in all three
    // sampled rigs (OneRig, anchoritespawn, HuntressHair).
    uint8_t auxRecordCount   = reader.Read<uint8_t>();

    if (!reader.IsValid())
        return Result<AstSkeleton>::Fail("Failed to read AST header");

    // Game accepts versions 0x02..0x0C (validated as (version - 2) <= 10).
    // The unsigned-subtraction trick means values < 2 wrap to large numbers and fail the check.
    if (static_cast<uint8_t>(version - 2) > 10)
        return Result<AstSkeleton>::Fail("Unknown AST version: 0x" + std::to_string(version));

    result.version = version;
    result.boneCountHeader = boneCount;
    result.auxBoneThreshold = auxBoneThreshold;
    result.animationCount = attachmentGroups;

    if (boneCount == 0)
        return Result<AstSkeleton>::Ok(std::move(result)); // empty skeleton is valid

    const bool hasFlagByte = HasNameFlagByte(version);

    // Bone record layout: see BONES.md Section 1.2. Byte order verified A -- see Section 1.3.
    // On-disk byte order per bone: [next_sibling, first_child, matrix(64), name_len, (flag if v>=0x08), name bytes].
    // FCNS semantics: both bytes are indices into this bones[] array, 0xFF is the sentinel (no-sibling / leaf).
    result.bones.reserve(boneCount);
    for (uint8_t i = 0; i < boneCount; ++i) {
        // Minimum record size = 2 (FCNS pointers) + 64 (matrix) + 1 (nameLen) + optional 1 flag
        size_t minRec = 67 + (hasFlagByte ? 1u : 0u);
        if (reader.Remaining() < minRec) {
            return Result<AstSkeleton>::Fail("Truncated bone record #" + std::to_string(i));
        }

        uint8_t nextSiblingIdx = reader.Read<uint8_t>(); // 0xFF = last child of its parent
        uint8_t firstChildIdx  = reader.Read<uint8_t>(); // 0xFF = leaf

        float matrix[16];
        for (int k = 0; k < 16; ++k)
            matrix[k] = reader.Read<float>();

        uint8_t nameLen = reader.Read<uint8_t>();
        uint8_t flagByte = 0;
        if (hasFlagByte)
            flagByte = reader.Read<uint8_t>();

        if (reader.Remaining() < nameLen) {
            return Result<AstSkeleton>::Fail("Truncated bone name #" + std::to_string(i));
        }

        AstBone bone;
        bone.nextSiblingIndex = nextSiblingIdx;
        bone.firstChildIndex  = firstChildIdx;
        bone.flag             = flagByte;
        std::memcpy(bone.transform, matrix, sizeof(matrix));

        if (nameLen > 0) {
            std::string rawName = reader.ReadString(nameLen);
            auto pipePos = rawName.find('|');
            bone.name = (pipePos != std::string::npos) ? rawName.substr(0, pipePos) : rawName;
        }

        result.bones.push_back(std::move(bone));
    }

    // --- Animation section (Phase 4.1) ---
    // Wire-format spec: BONES.md §5. Driver loop mirrored from IDA
    // sub_7FF7A6670C20 @ 0x7FF7A6671948 (POE2 v0.4.0i).
    //
    // Layout order:
    //   [aux-bone records : version-keyed fixed portion + variable-length name, per AuxBoneFixedSize()] ; only if byte7 > 0
    //   [animation records : animationCount entries]
    //   [tail block : 12-byte trailer + index + compressed] ; only if version >= 8
    //
    // Lenient per-section mode (BONES.md §5.6): malformed animation record
    // breaks the loop and records a warning; bones remain valid, partial
    // animation list is returned.

    // Aux-bone preamble. Each aux record has a fixed portion (43/51/52/56/60
    // bytes — version-keyed via AuxBoneFixedSize) followed by a variable-length
    // ASCII name whose length is stored at byte 0 of the fixed record.
    // Verified via IDA at 0x7FF7A6671020+ (ParseAstFile aux-bone switch).
    // We don't expose aux bones yet; just walk past each record.
    if (auxRecordCount > 0) {
        const size_t fixedSize = AuxBoneFixedSize(version);
        for (uint8_t i = 0; i < auxRecordCount; ++i) {
            if (reader.Remaining() < fixedSize) {
                result.animationParseWarning =
                    "Truncated aux-bone fixed record #" + std::to_string(i);
                return Result<AstSkeleton>::Ok(std::move(result));
            }
            // Peek byte 0 (name length) without advancing past the fixed block.
            const uint8_t nameLen = *reader.CurrentPtr();
            const size_t recordSize = fixedSize + nameLen;
            if (reader.Remaining() < recordSize) {
                result.animationParseWarning =
                    "Truncated aux-bone name #" + std::to_string(i);
                return Result<AstSkeleton>::Ok(std::move(result));
            }
            reader.Skip(recordSize);
        }
    }

    // Animation header records.
    // For version <= 7 the game's driver loop calls sub_7FF7A6670050 inline
    // between headers to consume the per-bone keyframe block (BONES.md §5.1).
    // We mirror that here with SkipInlineKeyframeBlock — Phase 4.1 doesn't
    // interpret keyframes, but the cursor MUST advance past the block or the
    // next header parse lands on garbage. For version >= 8 the keyframes
    // live in the Oodle-compressed tail (read after the loop) instead.
    //
    // Phase 4.2: for v<=7, capture the byte range spanning the inline-keyframe
    // section into `rawInlineKeyframeBytes` so downstream consumers can
    // re-parse without revisiting the source file. We record the cursor
    // position before the loop and again after it completes (or
    // lenient-breaks early). The lenient-error contract is unchanged:
    // malformed records set `animationParseWarning` and break out; bones
    // remain valid; the tail-block parse below is already gated by
    // `animationParseWarning.empty()`.
    const size_t animSectionStart = reader.Position();
    result.animations.reserve(result.animationCount);
    for (uint32_t i = 0; i < result.animationCount; ++i) {
        auto animResult = ParseOneAnimation(reader, version);
        if (!animResult) {
            result.animationParseWarning =
                "Animation #" + std::to_string(i) + ": " + animResult.error;
            break;
        }

        if (version <= 7) {
            auto skipResult = SkipInlineKeyframeBlock(reader, animResult.value.animBoneCount);
            if (!skipResult) {
                result.animationParseWarning =
                    "Animation #" + std::to_string(i) + " inline keyframes: " + skipResult.error;
                // Retain the successfully-parsed header before bailing — only
                // the downstream keyframe block was malformed.
                result.animations.push_back(std::move(animResult.value));
                break;
            }
        }

        result.animations.push_back(std::move(animResult.value));
    }

    // Capture the inline-keyframe byte range for Phase 4.2 re-parse (v<=7 only).
    // For v>=8, keyframes live in animationTail.compressedChunk; this field
    // stays empty. Uses the raw `data` pointer passed into ParseAst (same
    // buffer BinaryReader was constructed over) rather than a BinaryReader
    // accessor — the reader does not expose its backing pointer by design.
    if (version <= 7 && !result.animations.empty()) {
        const size_t animSectionEnd = reader.Position();
        if (animSectionEnd >= animSectionStart && animSectionEnd <= size) {
            result.rawInlineKeyframeBytes.assign(
                data + animSectionStart,
                data + animSectionEnd);
        }
    }

    // Tail block (version >= 8 with at least one animation). When
    // animationCount == 0 the tail block's semantics are undefined (sum of zero
    // animation sizes); BONES.md §5.3.5 does not cover that corner, so skip it
    // defensively rather than risk misinterpreting trailing bytes.
    // Phase 4.1 stores raw bytes; Phase 4.2 will Oodle-decompress
    // `compressedChunk` and walk per-bone keyframes.
    if (version >= 0x08 && result.animationCount > 0 && result.animationParseWarning.empty()) {
        auto tailResult = ParseAnimationTail(reader);
        if (!tailResult) {
            result.animationParseWarning = "Tail block: " + tailResult.error;
        } else {
            result.animationTail = std::move(tailResult.value);
        }
    }

    return Result<AstSkeleton>::Ok(std::move(result));
}

}} // namespace GGPKExp::Parsers
