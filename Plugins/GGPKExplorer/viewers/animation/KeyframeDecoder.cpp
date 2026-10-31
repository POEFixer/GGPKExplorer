#include "KeyframeDecoder.h"

// Reuse the plugin's bundled Oodle wrapper (already linked for GGPK bundle decompression).
#include "../../lib/ggpk/Oodle.h"

#include <cstring>
#include <cstdio>

namespace GGPKExp { namespace Animation {

namespace {

// Per-bone header fixed-size portion (pre-array counts). Matches BONES.md §5.3.
constexpr std::size_t kPerBoneHeaderV9  = 29; // version <= 9
constexpr std::size_t kPerBoneHeaderV10 = 33; // version >= 10 (includes extra_float)

constexpr std::size_t kStrideA = 16;
constexpr std::size_t kStrideB = 20;
constexpr std::size_t kStrideC = 16;
constexpr std::size_t kStrideD = 16;
constexpr std::size_t kStrideE = 20;
constexpr std::size_t kStrideF = 16;

template <typename T>
bool ReadAt(const uint8_t* base, std::size_t& cursor, std::size_t limit, T& out) {
    if (cursor + sizeof(T) > limit) return false;
    std::memcpy(&out, base + cursor, sizeof(T));
    cursor += sizeof(T);
    return true;
}

} // anonymous namespace

bool KeyframeDecoder::DecodeBlobIfNeeded(const Parsers::AstSkeleton& skel,
                                        std::string* warning) {
    if (m_blobReady) return true;
    if (m_oodleAttempted) return false;   // don't retry after failure
    m_oodleAttempted = true;

    // Version <= 7: no Oodle; reads from rawInlineKeyframeBytes.
    if (skel.version <= 7) {
        if (skel.rawInlineKeyframeBytes.empty()) {
            if (warning) *warning = "v<=7 keyframes missing rawInlineKeyframeBytes field";
            return false;
        }
        m_blob = skel.rawInlineKeyframeBytes;
        m_blobReady = true;
        return true;
    }

    // Version >= 8: Oodle decompress the tail blob.
    if (skel.animationTail.compressedChunk.empty() ||
        skel.animationTail.sizeUncompressedTotal == 0) {
        // No animations / already empty tail — legitimate "no-anim" case.
        m_blob.clear();
        m_blobReady = true;
        return true;
    }

    if (!GGPK::Oodle::IsLoaded()) {
        if (warning) *warning = "Oodle DLL not loaded; call EnsureOodleLoaded() first";
        return false;
    }

    GGPK::Oodle::Settings settings{};
    settings.chunkSize = 256 * 1024;
    settings.compressor = GGPK::Oodle::Compressor::Leviathan;  // Sprint 1 Task 2 confirms
    settings.enableCompressing = false;
    GGPK::Oodle::Initialize(settings);

    m_blob.resize(skel.animationTail.sizeUncompressedTotal);
    intptr_t written = GGPK::Oodle::Decompress(
        skel.animationTail.compressedChunk.data(),
        static_cast<intptr_t>(skel.animationTail.compressedChunk.size()),
        m_blob.data(),
        static_cast<intptr_t>(m_blob.size()));

    if (written != static_cast<intptr_t>(m_blob.size())) {
        if (warning) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "Oodle decompress returned %lld, expected %zu",
                          (long long)written, m_blob.size());
            *warning = buf;
        }
        m_blob.clear();
        return false;
    }

    m_blobReady = true;
    return true;
}

bool KeyframeDecoder::ParseAnimation(const Parsers::AstSkeleton& skel,
                                    std::size_t animIndex,
                                    AnimationClip& out,
                                    std::string* warning) {
    out = AnimationClip{};

    if (!m_blobReady) {
        if (warning) *warning = "blob not decoded; call DecodeBlobIfNeeded first";
        return false;
    }
    if (animIndex >= skel.animations.size()) {
        if (warning) *warning = "animIndex out of range";
        return false;
    }

    const auto& anim = skel.animations[animIndex];
    out.name         = anim.name;
    out.name2        = anim.name2;
    out.playbackType = anim.playbackType;
    out.fps          = anim.fps;
    out.compressed   = anim.compressed;
    out.tracks.reserve(anim.animBoneCount);

    // Cursor into blob: for v>=8, offset into decompressed blob;
    // for v<=7, offset into rawInlineKeyframeBytes (currently the whole
    // animation section — ParseAnimation re-skips headers).
    //
    // Phase 4.2 MVP: v>=8 path only (modern POE2 ships v12). v<=7
    // implementation deferred to a future task; warn if encountered.
    if (skel.version <= 7) {
        if (warning) *warning = "v<=7 keyframe parse not implemented in Phase 4.2";
        return false;
    }

    const std::size_t start  = anim.dataOffset;
    const std::size_t length = anim.dataSize;
    if (start + length > m_blob.size()) {
        if (warning) *warning = "anim dataOffset+dataSize exceeds blob bounds";
        return false;
    }

    std::size_t cursor = start;
    const std::size_t limit  = start + length;
    const uint8_t*   base    = m_blob.data();

    for (std::size_t b = 0; b < anim.animBoneCount; ++b) {
        BoneTrack track;
        std::string boneWarn;
        if (!ParseBoneTrack(base, cursor, limit, skel.version, track, &boneWarn)) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "bone %zu/%u parse failed: %s",
                          b, (unsigned)anim.animBoneCount, boneWarn.c_str());
            out.warnings.emplace_back(buf);
            break;  // lenient: stop this clip, keep what we have
        }
        out.tracks.push_back(std::move(track));
    }

    // Cursor-consumption invariant check. Runs FIRST so it is not suppressed
    // by the attachment-rig warning we may push below — the two conditions
    // are independent (attachment-rig OOR is not a parse error: tracks parsed
    // fine, cursor SHOULD still reach limit).
    if (cursor != limit && out.warnings.empty()) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "cursor advance %zu != anim.dataSize %zu",
                      cursor - start, length);
        out.warnings.emplace_back(buf);
    }

    // Attachment-rig handling: tracks whose callbackArg indexed beyond the
    // actual skeleton bone count are kept parsed (so the cursor advances
    // correctly for the next track), but PoseInterpolator::Sample already
    // skips them when writing poses. Count and warn so UI surfaces it.
    std::size_t outOfRange = 0;
    for (const auto& tr : out.tracks) {
        if (tr.boneIndex >= skel.bones.size()) ++outOfRange;
    }
    if (outOfRange > 0) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "attachment rig: %zu/%zu tracks skipped (animBoneCount > bones.size)",
                      outOfRange, out.tracks.size());
        out.warnings.emplace_back(buf);
    }

    // Compute duration as max keyTime across all tracks.
    for (const auto& tr : out.tracks) {
        for (const auto& k : tr.translation) if (k.time > out.duration) out.duration = k.time;
        for (const auto& k : tr.rotation)    if (k.time > out.duration) out.duration = k.time;
        for (const auto& k : tr.scale)       if (k.time > out.duration) out.duration = k.time;
    }

    return true;
}

bool KeyframeDecoder::ParseBoneTrack(const uint8_t* base,
                                    std::size_t& cursor,
                                    std::size_t limit,
                                    uint8_t version,
                                    BoneTrack& out,
                                    std::string* warn) {
    const std::size_t headerSize = (version >= 10) ? kPerBoneHeaderV10 : kPerBoneHeaderV9;
    if (cursor + headerSize > limit) {
        if (warn) *warn = "per-bone header truncated";
        return false;
    }

    uint8_t  flagsByte;
    uint32_t callbackArg, ca, cb, cc, cd, ce, cf;
    ReadAt(base, cursor, limit, flagsByte);
    ReadAt(base, cursor, limit, callbackArg);
    ReadAt(base, cursor, limit, ca);
    ReadAt(base, cursor, limit, cb);
    ReadAt(base, cursor, limit, cc);
    ReadAt(base, cursor, limit, cd);
    ReadAt(base, cursor, limit, ce);
    ReadAt(base, cursor, limit, cf);
    if (version >= 10) {
        uint32_t extra_float;
        ReadAt(base, cursor, limit, extra_float);   // per-track end-time (Addendum B §B.6)
        (void)extra_float;
    }

    // callbackArg -> bone index mapping per BONES.md §5.3.
    // Phase 4.2 default (direct-index interpretation, confirmed in Sprint 1 Task 3.4).
    // If Sprint 1 proved sub-table mapping, replace line below with sub-table lookup.
    out.boneIndex = static_cast<uint16_t>(callbackArg & 0xFFFF);

    const std::size_t needed =
          static_cast<std::size_t>(ca) * kStrideA
        + static_cast<std::size_t>(cb) * kStrideB
        + static_cast<std::size_t>(cc) * kStrideC
        + static_cast<std::size_t>(cd) * kStrideD
        + static_cast<std::size_t>(ce) * kStrideE
        + static_cast<std::size_t>(cf) * kStrideF;
    if (cursor + needed > limit) {
        if (warn) *warn = "per-bone arrays truncated";
        return false;
    }

    // Channel mapping (verified 2026-04-14 by dumping witch attack1_2hsword with
    // the AnimDump dev tool, since removed):
    //   A (16 B) = SCALE      keyframes (time + xyz)
    //   B (20 B) = ROTATION   keyframes (time + xyzw quaternion) - both static and curve
    //   C (16 B) = TRANSLATION keyframes (time + xyz)
    //   D, E, F  = unused for character animations (always 0 in observed data).
    // The earlier "A=translation, B=static-only, D=compressed rotation" layout was
    // a pre-RE hypothesis from Sprint 1; per-bone count dump shows cD=0 always.

    // Array A (16 B) — SCALE.
    out.scale.reserve(ca);
    for (uint32_t i = 0; i < ca; ++i) {
        KeyS k;
        std::memcpy(&k.time,  base + cursor, 4);
        std::memcpy(k.value,  base + cursor + 4, 12);
        cursor += kStrideA;
        out.scale.push_back(k);
    }

    // Array B (20 B) — ROTATION (full xyzw quaternion per keyframe).
    out.rotation.reserve(cb);
    for (uint32_t i = 0; i < cb; ++i) {
        KeyR k;
        std::memcpy(&k.time,  base + cursor, 4);
        std::memcpy(k.value,  base + cursor + 4, 16);
        cursor += kStrideB;
        out.rotation.push_back(k);
    }

    // Array C (16 B) — TRANSLATION.
    out.translation.reserve(cc);
    for (uint32_t i = 0; i < cc; ++i) {
        KeyT k;
        std::memcpy(&k.time,  base + cursor, 4);
        std::memcpy(k.value,  base + cursor + 4, 12);
        cursor += kStrideC;
        out.translation.push_back(k);
    }

    // Arrays D/E/F — always empty for character animations. Skip bytes.
    cursor += static_cast<std::size_t>(cd) * kStrideD;
    cursor += static_cast<std::size_t>(ce) * kStrideE;
    cursor += static_cast<std::size_t>(cf) * kStrideF;

    // flagsByte stored for diagnostics; sampling decision is per-channel based
    // on keyframe count (1 = constant, >1 = curve, 0 = bind-pose default).
    out.hasStaticPose = (flagsByte == 0);

    // Static fallbacks: index 0 of each populated array.
    if (!out.translation.empty()) {
        out.staticT[0] = out.translation[0].value[0];
        out.staticT[1] = out.translation[0].value[1];
        out.staticT[2] = out.translation[0].value[2];
    }
    if (!out.rotation.empty()) {
        out.staticR[0] = out.rotation[0].value[0];
        out.staticR[1] = out.rotation[0].value[1];
        out.staticR[2] = out.rotation[0].value[2];
        out.staticR[3] = out.rotation[0].value[3];
    }
    if (!out.scale.empty()) {
        out.staticS[0] = out.scale[0].value[0];
        out.staticS[1] = out.scale[0].value[1];
        out.staticS[2] = out.scale[0].value[2];
    }

    return true;
}

}} // namespace GGPKExp::Animation
