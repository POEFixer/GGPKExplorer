#include "AnimationPlayer.h"
#include "AnimationDefaults.h"
#include "MatrixMath.h"
#include "../../core/parsers/AocSkeletonAugmenter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace GGPKExp { namespace Animation {

void AnimationPlayer::AttachSkeleton(const Parsers::AstSkeleton* skel) {
    Reset();
    m_skeleton = skel;
    if (!skel) return;

    m_animNames.reserve(skel->animations.size());
    for (const auto& a : skel->animations) m_animNames.push_back(a.name);
    m_clips.resize(skel->animations.size());
    m_clipParsed.assign(skel->animations.size(), false);

    // Select default animation: prefer exact "idle", then prefix "idle", else first.
    // Does NOT auto-play; user initiates playback manually.
    const std::size_t idx = PickDefaultAnimIndex(m_animNames);
    if (idx != static_cast<std::size_t>(-1)) {
        SetLayerAnimation(0, static_cast<int>(idx));
        m_layers[0].playing = false;
    }
}

void AnimationPlayer::Reset() {
    m_skeleton = nullptr;
    m_decoder = KeyframeDecoder{};
    m_clips.clear();
    m_clipParsed.clear();
    m_animNames.clear();
    m_warnings.clear();

    // Reset layer state (all 4 slots back to defaults).
    m_layers = {};
    m_activeLayerCount = 1;

    // Preserve user's chosen speed across model reload. m_layers = {} above
    // resets every layer to AnimationLayer::speed=1.0f; we re-apply the
    // remembered default so RenderSpeedCombo doesn't snap to "0.1x" (= 1.0).
    m_layers[0].speed = m_defaultSpeed;

    for (std::size_t i = 0; i < kMaxLayers; ++i) {
        m_layerPoses[i].clear();
        m_kfCaches[i].clear();
    }
    m_baseScratch.clear();
    m_restScratch.clear();
    m_boneGroups.clear();
}

void AnimationPlayer::SetLayerAnimation(std::size_t i, int animIdx) {
    if (i >= m_activeLayerCount) return;
    if (!m_skeleton) return;
    if (animIdx < 0) return;
    if (static_cast<std::size_t>(animIdx) >= m_skeleton->animations.size()) return;

    std::string warn;
    if (!m_decoder.DecodeBlobIfNeeded(*m_skeleton, &warn)) {
        if (!warn.empty()) m_warnings.push_back("decode: " + warn);
        return;
    }

    const std::size_t idx = static_cast<std::size_t>(animIdx);
    if (!m_clipParsed[idx]) {
        std::string pwarn;
        if (!m_decoder.ParseAnimation(*m_skeleton, idx, m_clips[idx], &pwarn)) {
            if (!pwarn.empty()) m_warnings.push_back("parse: " + pwarn);
            // Keep whatever partial clip we got; still allow selection.
        }
        m_clipParsed[idx] = true;
        for (const auto& w : m_clips[idx].warnings)
            m_warnings.push_back(m_clips[idx].name + ": " + w);

        // Detect derived animation (compressed=1 + name2 present) and warn.
        // Phase 4.2 MVP parses-best-effort; proper delta decode deferred to 4.3.
        const auto& clipRef = m_clips[idx];
        if (clipRef.compressed == 1 && !clipRef.name2.empty()) {
            m_warnings.push_back("derived animation: '" + clipRef.name +
                                 "' references base '" + clipRef.name2 +
                                 "' (Phase 4.3: proper delta decode)");
        }
    }

    m_layers[i].animIndex   = animIdx;
    m_layers[i].timeSeconds = 0.0f;
    m_kfCaches[i].clear();
}

bool AnimationPlayer::AddLayer() {
    if (m_activeLayerCount >= kMaxLayers) return false;
    AnimationLayer layer{};
    layer.additive  = true;   // additive by default for layers 1..N
    layer.animIndex = -1;     // empty slot
    m_layers[m_activeLayerCount] = layer;
    m_layerPoses[m_activeLayerCount].clear();
    m_kfCaches[m_activeLayerCount].clear();
    ++m_activeLayerCount;
    return true;
}

void AnimationPlayer::RemoveLayer(std::size_t i) {
    if (i == 0) return;                      // base is non-removable
    if (i >= m_activeLayerCount) return;     // out of range

    for (std::size_t j = i; j + 1 < m_activeLayerCount; ++j) {
        m_layers[j]      = m_layers[j + 1];
        m_layerPoses[j]  = std::move(m_layerPoses[j + 1]);
        m_kfCaches[j]    = std::move(m_kfCaches[j + 1]);
    }
    // Clear the now-unused tail slot.
    const std::size_t tail = m_activeLayerCount - 1;
    m_layers[tail]     = AnimationLayer{};
    m_layerPoses[tail].clear();
    m_kfCaches[tail].clear();
    --m_activeLayerCount;
}

void AnimationPlayer::Play() {
    auto& layer = m_layers[0];
    if (layer.animIndex >= 0 &&
        static_cast<std::size_t>(layer.animIndex) < m_clips.size()) {
        const auto& clip = m_clips[static_cast<std::size_t>(layer.animIndex)];
        // Non-looping clips pin timeSeconds at clip.duration and set playing=false
        // when they finish. A naked flip of playing back to true would immediately
        // re-pause. Rewind to 0 so the user gets a fresh playback.
        if (clip.playbackType == 'o' && layer.timeSeconds >= clip.duration) {
            layer.timeSeconds = 0.0f;
            m_kfCaches[0].clear();   // drop stale forward-scan cursors
        }
    }
    layer.playing = true;
}
void AnimationPlayer::Pause() { m_layers[0].playing = false; }

void AnimationPlayer::SeekSeconds(float t) {
    m_layers[0].playing = false;
    float clamped = std::max(0.0f, t);
    const int baseAnim = m_layers[0].animIndex;
    if (baseAnim >= 0 && static_cast<std::size_t>(baseAnim) < m_clips.size()) {
        const float dur = m_clips[static_cast<std::size_t>(baseAnim)].duration;
        if (dur > 0.0f) clamped = std::min(clamped, dur);
    }
    m_layers[0].timeSeconds = clamped;
    m_kfCaches[0].clear();
}

void AnimationPlayer::Tick(float dt) {
    // Spike guard (tab unfocus): clamp absurdly large dt so a multi-second
    // stall doesn't fast-forward the animation by that much. 10 s ceiling is
    // large enough that legitimate Tick calls (incl. test harness up to 1.5 s)
    // pass through unchanged.
    const float dtClamped = std::min(dt, 10.0f);

    for (std::size_t li = 0; li < m_activeLayerCount; ++li) {
        AnimationLayer& layer = m_layers[li];
        if (!layer.playing) continue;
        if (layer.animIndex < 0) continue;
        if (static_cast<std::size_t>(layer.animIndex) >= m_clips.size()) continue;

        const auto& clip = m_clips[static_cast<std::size_t>(layer.animIndex)];
        layer.timeSeconds += dtClamped * layer.speed;

        // Loop policy: base layer uses clip.playbackType, additive layers
        // use their own 'loop' flag (per §A.3).
        char policy;
        if (li == 0) {
            policy = clip.playbackType;
        } else {
            policy = layer.loop ? 'l' : 'o';
        }

        switch (policy) {
            case 'l':
                if (clip.duration > 0.0f) {
                    layer.timeSeconds = std::fmod(layer.timeSeconds, clip.duration);
                    if (layer.timeSeconds < 0.0f) layer.timeSeconds += clip.duration;
                }
                break;
            case 'o':
                if (layer.timeSeconds >= clip.duration) {
                    layer.timeSeconds = clip.duration;
                    layer.playing     = false;
                }
                break;
            case 'n':
            default:
                layer.timeSeconds = std::clamp(layer.timeSeconds, 0.0f, clip.duration);
                break;
        }
    }
}

void AnimationPlayer::ApplyFrame(Parsers::ResolvedSkeleton& skel) {
    if (m_activeLayerCount == 0) return;

    // Guard: layer 0 must have a valid clip for ApplyFrame to do anything.
    const int baseIdx = m_layers[0].animIndex;
    if (baseIdx < 0) return;
    if (static_cast<std::size_t>(baseIdx) >= m_clips.size()) return;

    m_baseScratch.resize(skel.bones.size());
    m_restScratch.resize(skel.bones.size());
    // Seed base + rest from bind pose.
    for (std::size_t i = 0; i < skel.bones.size(); ++i) {
        std::memcpy(m_baseScratch[i].local, skel.bones[i].localTransform, sizeof(float) * 16);
        std::memcpy(m_restScratch[i].local, skel.bones[i].localTransform, sizeof(float) * 16);
    }

    // Sample base.
    const auto& baseClip = m_clips[static_cast<std::size_t>(baseIdx)];
    m_layerPoses[0].resize(skel.bones.size());
    for (std::size_t i = 0; i < skel.bones.size(); ++i)
        std::memcpy(m_layerPoses[0][i].local, m_restScratch[i].local, sizeof(float) * 16);
    m_interpolator.Sample(baseClip, m_layers[0].timeSeconds, m_layerPoses[0], m_kfCaches[0]);

    // Sample each additive layer.
    std::vector<PoseComposer::LayerInput> adds;
    adds.reserve(m_activeLayerCount - 1);
    for (std::size_t li = 1; li < m_activeLayerCount; ++li) {
        const int li_idx = m_layers[li].animIndex;
        if (li_idx < 0) continue;
        if (static_cast<std::size_t>(li_idx) >= m_clips.size()) continue;

        const auto& clip = m_clips[static_cast<std::size_t>(li_idx)];
        m_layerPoses[li].resize(skel.bones.size());
        for (std::size_t i = 0; i < skel.bones.size(); ++i)
            std::memcpy(m_layerPoses[li][i].local, m_restScratch[i].local, sizeof(float) * 16);
        m_interpolator.Sample(clip, m_layers[li].timeSeconds, m_layerPoses[li], m_kfCaches[li]);
        adds.push_back({ &m_layerPoses[li], &m_layers[li] });
    }

    // Gather bone names for mask lookup.
    std::vector<std::string> boneNames;
    boneNames.reserve(skel.bones.size());
    for (const auto& b : skel.bones) boneNames.push_back(b.name);

    // Bone groups come from the .aoc document (if loaded), supplied via SetBoneGroups.
    std::vector<BonePose> composed(skel.bones.size());
    m_composer.Compose(m_layerPoses[0], adds, m_restScratch, boneNames, m_boneGroups, composed);

    // Write composed local transforms + recompose world transforms.
    for (std::size_t i = 0; i < skel.bones.size(); ++i)
        std::memcpy(skel.bones[i].localTransform, composed[i].local, sizeof(float) * 16);
    RecomposeWorldTransforms(skel);
    Parsers::UpdateVirtualBoneWorldTransforms(skel);
}

void AnimationPlayer::InjectClipForTesting(AnimationClip clip) {
    m_clips.clear();
    m_clipParsed.clear();
    m_clips.push_back(std::move(clip));
    m_clipParsed.push_back(true);

    m_layers[0].animIndex   = 0;
    m_layers[0].timeSeconds = 0.0f;
    m_layers[0].playing     = false;   // tests call Play() explicitly
    m_activeLayerCount      = 1;
    m_kfCaches[0].clear();
}

float AnimationPlayer::CurrentDuration() const {
    const int idx = m_layers[0].animIndex;
    if (idx < 0) return 0.0f;
    if (static_cast<std::size_t>(idx) >= m_clips.size()) return 0.0f;
    return m_clips[static_cast<std::size_t>(idx)].duration;
}

uint32_t AnimationPlayer::CurrentFrame() const {
    const int idx = m_layers[0].animIndex;
    if (idx < 0) return 0;
    if (static_cast<std::size_t>(idx) >= m_clips.size()) return 0;
    const auto& c = m_clips[static_cast<std::size_t>(idx)];
    return static_cast<uint32_t>(m_layers[0].timeSeconds * static_cast<float>(c.fps));
}

bool AnimationPlayer::IsAnyLayerPlaying() const {
    for (const auto& layer : m_layers) {
        if (layer.playing) return true;
    }
    return false;
}

float AnimationPlayer::BaseLayerTime() const {
    // m_layers is std::array<AnimationLayer, kMaxLayers> - guaranteed non-empty.
    return m_layers[0].timeSeconds;
}

void AnimationPlayer::RecomposeWorldTransforms(Parsers::ResolvedSkeleton& skel) const {
    // FCNS walk is parent-first since arrayParent indices are monotonically
    // less than the child's index (BuildResolvedSkeleton property).
    for (std::size_t i = 0; i < skel.bones.size(); ++i) {
        auto& b = skel.bones[i];
        if (b.arrayParent == static_cast<std::size_t>(-1) ||
            b.arrayParent >= skel.bones.size()) {
            // Root - world = local.
            std::memcpy(b.worldTransform, b.localTransform, sizeof(float) * 16);
        } else {
            const auto& p = skel.bones[b.arrayParent];
            MatrixMath::Multiply4x4RowMajor(b.localTransform, p.worldTransform, b.worldTransform);
        }
    }
}

}} // namespace GGPKExp::Animation
