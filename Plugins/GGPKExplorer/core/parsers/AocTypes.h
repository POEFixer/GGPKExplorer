#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace GGPKExp { namespace Parsers {

// ---------------------------------------------------------------------
// Shared primitive: a single key/value entry with provenance metadata
// ---------------------------------------------------------------------
struct KeyValue {
    std::string key;
    std::string value;
    std::string sourceAocPath;  // which .aoc in the extends chain defined this
    std::size_t sourceLine = 0;
};

// ---------------------------------------------------------------------
// A socket parsed from a ClientAnimationController block. Represents an
// attachment point in local space relative to a named parent bone.
// ---------------------------------------------------------------------
struct Socket {
    std::string name;
    std::string parentBoneName;
    float       translation[3] = {0.0f, 0.0f, 0.0f};
    float       rotation[3]    = {0.0f, 0.0f, 0.0f};  // Euler XYZ radians (order TBD Sprint 3 RE)
    float       scale[3]       = {1.0f, 1.0f, 1.0f};
    std::string ignoreParentAxis;      // e.g. "xy"; parsed but not applied in Phase 3 MVP
    std::string ignoreParentRot;       // e.g. "yawpitchroll"; parsed but not applied in MVP
    std::string sourceAocPath;
    std::size_t sourceLine = 0;
};

// ---------------------------------------------------------------------
// A bone_group line: name + mask flag + list of bone names.
// ---------------------------------------------------------------------
struct BoneGroup {
    std::string              name;
    bool                     maskFlag = false;
    std::vector<std::string> boneNames;
    std::string              sourceAocPath;
    std::size_t              sourceLine = 0;
};

// ---------------------------------------------------------------------
// Typed view of ClientAnimationController block.
// ---------------------------------------------------------------------
struct ClientAnimCtrl {
    std::string              skeletonPath;
    std::string              skeletonPathSource;  // which .aoc defined skeletonPath
    std::vector<Socket>      sockets;
    std::vector<BoneGroup>   boneGroups;
    std::vector<KeyValue>    otherProperties;
};

// ---------------------------------------------------------------------
// Typed view of SkinMesh block.
// ---------------------------------------------------------------------
struct SkinMeshBlock {
    std::string              skinPath;
    std::string              skinPathSource;
    std::vector<KeyValue>    shapeOverrides;  // *Shape = "..."
    std::vector<KeyValue>    otherProperties;
};

// ---------------------------------------------------------------------
// Typed view of AnimationController block.
// ---------------------------------------------------------------------
struct AnimCtrlBlock {
    std::vector<KeyValue>    eventHandlers;   // on_event_* = "..."
    std::vector<KeyValue>    otherProperties;
};

// --------------------------------------------------------------------------
// Phase 2 — Lights + SoundEvents + AnimationController events
// --------------------------------------------------------------------------

enum class LightType : int {
    Point       = 0,
    Spot        = 1,
    Directional = 2,   // Parsed but skipped with warning; no test data exists
};

// One named state variant of a light (e.g. "on", "off", "dying").
// Values are in the light's local space when `bone_name` is non-empty
// (relative to that bone), otherwise in .aoc local space.
struct LightState {
    std::string name;                      // "on", "off", "default", etc.
    std::array<float, 3> colour    { 1.f, 1.f, 1.f };   // HDR-capable
    std::array<float, 3> position  { 0.f, 0.f, 0.f };
    float radius                   = 1.0f;
    float penumbraDistance         = 0.0f;
    float penumbraRadius           = 0.0f;
    // Spot-only:
    std::array<float, 3> direction { 0.f, 0.f, -1.f };  // Unit vector expected
    float penumbraAngleRad         = 0.0f; // Outer cone half-angle
    float umbraAngleRad            = 0.0f; // Inner (fully lit) cone half-angle
};

// LightStateEventType entry from the Lights block events JSON array.
// Resolved into `stateIndex` at parse time (− 1 if unknown).
struct LightStateEvent {
    float       time       = 0.0f;
    std::string stateName;
    int         stateIndex = -1;
};

struct LightDef {
    LightType                     type          = LightType::Point;
    std::string                   name;                    // e.g. "boss_spotlight"
    std::string                   boneName;                // empty = world-space
    bool                          castsShadow   = false;
    float                         styleSpeed    = 0.0f;    // Parsed, not animated in P2
    std::vector<LightState>       states;                  // >=1 after parsing
    int                           defaultStateIndex = 0;
    std::vector<LightStateEvent>  events;                  // Timeline-sorted
};

struct LightsBlock {
    std::vector<LightDef> lights;
    int                   skippedDirectional = 0; // For diagnostic summary
};

// SoundEventType clip from the SoundEvents block.
// `filename` is an extension-less logical path (e.g. "audio/dialogue/foo/bar");
// AocAudioSystem resolves to .ogg → .wav → .oga at trigger time.
struct SoundEventClip {
    float       time      = 0.0f;
    std::string type;       // "SoundEventType" etc.
    std::string filename;
    std::string boneName;   // May be empty
};

struct SoundEventsBlock {
    std::vector<SoundEventClip> clips;   // Sorted by time after parse
};

// One handler or event entry from the AnimationController block (parsed, not executed).
enum class AnimCtrlEventKind : int {
    OnEvent      = 0,   // on_event_<name> = "script body"
    Haptic       = 1,   // HapticEventType { time, name }
    GenericEvent = 2,   // any other *EventType in the events array
};

struct AnimEventRef {
    AnimCtrlEventKind kind = AnimCtrlEventKind::OnEvent;
    std::string       name;         // event name OR script handler name
    float             time = 0.0f;  // Non-zero only for timeline events
    std::string       summary;      // Short human-readable body (first 80 chars of script, or JSON snippet)
};

struct AnimEventsBlock {
    std::vector<AnimEventRef> events;
};

// --------------------------------------------------------------------------
// Phase 3 — ParticleEffects block (stub — .pet contents NOT parsed)
// --------------------------------------------------------------------------

// One ParticleEffectEventType entry from a ParticleEffects JSON events array.
// `filename` is a GGPK logical path to a binary `.pet` file. In Phase 3 we
// only validate existence via GGPKManager::FindNodeByPath; the binary payload
// is not decoded. Either `boneGroup` or `boneName` may be set (mutually
// exclusive); the viewer prefers boneGroup when both are present.
struct ParticleEffectClip {
    std::string type;           // "ParticleEffectEventType"
    float       time      = 0.0f;
    std::string filename;       // .pet path
    std::string boneGroup;      // e.g. "root", "center_pt_grp"
    std::string boneName;       // fallback if no bone_group given
    std::string animationName;  // owner animation clip name (from enclosing nested group)
};

struct ParticleEffectsBlock {
    std::vector<ParticleEffectClip> clips;   // Flat list, sorted by time then animationName
    int skippedUnknownTypes = 0;              // For diagnostic summary (e.g. TrailEffectEventType)
};

// --------------------------------------------------------------------------
// Phase 4 — Transitionable (.ot / .otc sibling files)
// --------------------------------------------------------------------------

// One entry inside a Transitionable block:
//   Numbered (N-state):
//     `create_state_<N> = "body"`        — fires when entity is spawned directly in state N
//     `on_transition_to_<N> = "body"`    — fires on transition to state N
//   Binary (2-state, suffix-less):
//     `create_untransitioned = "body"`   — spawn directly in state 0
//     `create_transitioned = "body"`     — spawn directly in state 1
//     `on_untransition = "body"`         — transition target = state 0
//     `on_transition = "body"`           — transition target = state 1
// The script body is NOT executed — we only regex-extract the first
// `PlayAnimation(CLIP_NAME)` call to derive which .amd clip that state maps to.
struct OtStateHandler {
    std::string key;             // original key preserved verbatim, e.g. "create_state_1" or "create_transitioned"
    std::string body;            // raw script body between outer quotes
    int         stateIndex = -1; // numbered: trailing digits from key; binary: 0 or 1 derived from key name; -1 if absent
    bool        isTransition = false; // true iff key starts with "on_transition_to_" or is "on_transition"/"on_untransition"
    std::string extractedClip;   // first PlayAnimation(X) arg; empty if not found
};

// Typed view of a Transitionable block, typically merged from sibling
// .ot (has `num_states = N`) and .otc (has `create_state_*` / `on_transition_to_*`
// or `create_(un)transitioned` / `on_(un)transition` handlers).
struct OtTransitionable {
    int numStates         = 0;   // max(declaredNumStates, max(handler.stateIndex)+1, isBinary ? 2 : 0)
    int declaredNumStates = 0;   // raw "num_states = N" value (for diagnostic display)
    bool isBinary         = false; // true iff any of create_(un)transitioned / on_(un)transition was parsed
    std::vector<OtStateHandler> handlers;
    std::string sourceOtPath;    // provenance: .ot path (empty if absent)
    std::string sourceOtcPath;   // provenance: .otc path (empty if absent)
};

// Minimal .ot/.otc view — Phase 4 only extracts Transitionable. Extensible
// via additional unique_ptr fields in later phases (StateMachine, Animated, ...).
struct OtFile {
    int                               version = 0;
    std::string                       extendsPath;       // raw; not walked in Phase 4
    std::unique_ptr<OtTransitionable> transitionable;    // nullptr if absent
    std::string                       sourcePath;
    std::vector<std::string>          warnings;

    OtFile() = default;
    OtFile(const OtFile&) = delete;
    OtFile(OtFile&&) noexcept = default;
    OtFile& operator=(const OtFile&) = delete;
    OtFile& operator=(OtFile&&) noexcept = default;
};

// ---------------------------------------------------------------------
// Generic AST node — used before typed post-processing and for the 11
// non-typed blocks. Children represent sub-properties at deeper indent.
// ---------------------------------------------------------------------
struct AocRawNode {
    std::string              key;
    std::string              value;
    std::vector<AocRawNode>  children;
    std::size_t              sourceLine = 0;
    std::string              sourceAocPath;  // filled during resolver merge
};

// ---------------------------------------------------------------------
// A single top-level block (ClientAnimationController, Lights, etc.)
// Typed view via unique_ptr fields (non-null for the 3 known block types).
// ---------------------------------------------------------------------
struct AocBlock {
    std::string                       name;
    std::vector<AocRawNode>           properties;      // raw tree always populated
    std::unique_ptr<ClientAnimCtrl>   clientAnim;      // non-null iff name=="ClientAnimationController"
    std::unique_ptr<SkinMeshBlock>    skinMesh;        // non-null iff name=="SkinMesh"
    std::unique_ptr<AnimCtrlBlock>    animController;  // non-null iff name=="AnimationController"
    // Phase 2 typed blocks (nullptr when block absent from .aoc).
    std::unique_ptr<LightsBlock>      lights;
    std::unique_ptr<SoundEventsBlock> soundEvents;
    std::unique_ptr<AnimEventsBlock>  animEvents;
    // Phase 3 typed blocks (nullptr when block absent from .aoc).
    std::unique_ptr<ParticleEffectsBlock> particleEffects;
    std::size_t                       sourceLine = 0;
    std::string                       sourceAocPath;

    AocBlock() = default;
    AocBlock(const AocBlock&) = delete;
    AocBlock(AocBlock&&) noexcept = default;
    AocBlock& operator=(const AocBlock&) = delete;
    AocBlock& operator=(AocBlock&&) noexcept = default;
};

// ---------------------------------------------------------------------
// A fully parsed .aoc file (after parser, before resolver).
// ---------------------------------------------------------------------
struct AocFile {
    int                          version = 0;
    std::string                  extendsPath;   // raw string from "extends "..." "; "Metadata/Parent" = sentinel
    std::vector<AocBlock>        blocks;
    std::string                  sourcePath;    // GGPK path for provenance
    std::vector<std::string>     warnings;
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
