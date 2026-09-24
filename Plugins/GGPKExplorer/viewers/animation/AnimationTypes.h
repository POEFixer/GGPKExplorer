#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace GGPKExp { namespace Animation {

// Keyframe entries — strides match wire-format A / B / C arrays.
// See BONES.md §5.3 + §5.7. Channel mapping discovered via AnimDump (witch
// attack1_2hsword): A holds scale, B holds full-quaternion rotation curves
// (xyzw), C holds translation. D/E/F are unused for character animations
// in observed POE2 builds.
struct KeyT  { float time; float value[3]; };    // C  (16 B) — translation
struct KeyR  { float time; float value[4]; };    // B  (20 B) — rotation (full xyzw quat)
struct KeyS  { float time; float value[3]; };    // A  (16 B) — scale

// One bone's animation curves. boneIndex is the `callbackArg` from the
// per-bone keyframe header — direct index into the rig's animation-bone
// table (see BONES.md §5.3 "callbackArg mapping"). For attachment rigs,
// values >= skel.bones.size() are virtual bones; consumer should skip
// those tracks with a warning.
struct BoneTrack {
    uint16_t boneIndex = 0xFFFF;
    std::vector<KeyT>  translation;              // array C
    std::vector<KeyR>  rotation;                 // array B (full xyzw quaternions)
    std::vector<KeyS>  scale;                    // array A

    // flagsByte from per-bone header. Currently informational only —
    // sampling decision is per-channel based on keyframe count, not flags.
    bool  hasStaticPose = false;
    float staticT[3] = {0.0f, 0.0f, 0.0f};
    float staticR[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float staticS[3] = {1.0f, 1.0f, 1.0f};
};

// One parsed animation clip (corresponds to one AstAnimation header record).
struct AnimationClip {
    std::string name;                            // primary name
    std::string name2;                           // reference base anim, "" if none
    char        playbackType = 'n';              // 'l' / 'n' / 'o'
    uint8_t     fps          = 30;
    uint8_t     compressed   = 0;
    float       duration     = 0.0f;             // max keyTime across tracks
    std::vector<BoneTrack> tracks;
    std::vector<std::string> warnings;           // per-clip non-fatal issues
};

// Sampled pose at one time instant (row-major 4x4, T * R * S).
struct BonePose {
    float local[16];
};

// One playback layer (Phase 4.2 scope C — multi-track composition).
// AnimationPlayer holds up to kMaxLayers of these. Layer 0 is the base
// track; layers 1..N are additive with optional bone-group mask.
//
// User decisions 2026-04-14 (spec §16.1):
// - maxLayers = 4 (base + 3 additives)
// - per-layer bone-group masking via maskBoneGroupIdx (Phase 3 .aoc BoneGroup)
// - crossfade NOT modelled in Phase 4.2 (user ramps weight manually)
// - composition: simple additive math (see PoseComposer)
struct AnimationLayer {
    int   animIndex        = -1;      // index into AstSkeleton::animations; -1 = empty slot
    float weight           = 1.0f;    // blend weight 0..1 (user-controlled)
    float speed            = 1.0f;    // playback speed multiplier
    float timeSeconds      = 0.0f;    // current playhead
    bool  playing          = false;   // independent play/pause
    bool  loop             = true;    // per-layer loop flag (overrides clip.playbackType for additives)
    int   maskBoneGroupIdx = -1;      // index into ClientAnimCtrl::boneGroups; -1 = full-body
    bool  maskInverted     = false;   // is_masked_override semantic
    bool  additive         = false;   // layer 0: false; layers 1..N: true
};

constexpr std::size_t kMaxLayers = 4;

}} // namespace GGPKExp::Animation
