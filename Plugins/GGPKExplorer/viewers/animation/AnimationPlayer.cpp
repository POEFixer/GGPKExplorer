#include "AnimationPlayer.h"
#include "AnimationDefaults.h"
#include "MatrixMath.h"

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
        SelectAnimation(idx);
        m_playing = false;
    }
}

void AnimationPlayer::Reset() {
    m_skeleton = nullptr;
    m_decoder = KeyframeDecoder{};
    m_clips.clear();
    m_clipParsed.clear();
    m_animNames.clear();
    m_warnings.clear();

    m_animIndex = -1;
    m_time      = 0.0f;
    m_playing   = false;
    // Preserve user's chosen speed across model reload so the speed combo
    // doesn't snap back to its default.
    m_speed     = m_defaultSpeed;

    m_pose.clear();
    m_kfCache.clear();
}

void AnimationPlayer::SelectAnimation(std::size_t animIndex) {
    if (!m_skeleton) return;
    if (animIndex >= m_skeleton->animations.size()) return;

    std::string warn;
    if (!m_decoder.DecodeBlobIfNeeded(*m_skeleton, &warn)) {
        if (!warn.empty()) m_warnings.push_back("decode: " + warn);
        return;
    }

    if (!m_clipParsed[animIndex]) {
        std::string pwarn;
        if (!m_decoder.ParseAnimation(*m_skeleton, animIndex, m_clips[animIndex], &pwarn)) {
            if (!pwarn.empty()) m_warnings.push_back("parse: " + pwarn);
            // Keep whatever partial clip we got; still allow selection.
        }
        m_clipParsed[animIndex] = true;
        for (const auto& w : m_clips[animIndex].warnings)
            m_warnings.push_back(m_clips[animIndex].name + ": " + w);

        // Detect derived animation (compressed=1 + name2 present) and warn:
        // it is parsed best-effort, without a delta decode against its base.
        const auto& clipRef = m_clips[animIndex];
        if (clipRef.compressed == 1 && !clipRef.name2.empty()) {
            m_warnings.push_back("derived animation: '" + clipRef.name +
                                 "' references base '" + clipRef.name2 +
                                 "' (no delta decode)");
        }
    }

    m_animIndex = static_cast<int>(animIndex);
    m_time      = 0.0f;
    m_kfCache.clear();
}

void AnimationPlayer::Play() {
    if (m_animIndex >= 0 && static_cast<std::size_t>(m_animIndex) < m_clips.size()) {
        const auto& clip = m_clips[static_cast<std::size_t>(m_animIndex)];
        // Non-looping clips pin the playhead at clip.duration and stop when
        // they finish. A naked flip of m_playing back to true would immediately
        // re-pause. Rewind to 0 so the user gets a fresh playback.
        if (clip.playbackType == 'o' && m_time >= clip.duration) {
            m_time = 0.0f;
            m_kfCache.clear();   // drop stale forward-scan cursors
        }
    }
    m_playing = true;
}

void AnimationPlayer::SeekSeconds(float t) {
    m_playing = false;
    float clamped = std::max(0.0f, t);
    if (m_animIndex >= 0 && static_cast<std::size_t>(m_animIndex) < m_clips.size()) {
        const float dur = m_clips[static_cast<std::size_t>(m_animIndex)].duration;
        if (dur > 0.0f) clamped = std::min(clamped, dur);
    }
    m_time = clamped;
    m_kfCache.clear();
}

void AnimationPlayer::Tick(float dt) {
    if (!m_playing) return;
    if (m_animIndex < 0) return;
    if (static_cast<std::size_t>(m_animIndex) >= m_clips.size()) return;

    // Spike guard (tab unfocus): clamp absurdly large dt so a multi-second
    // stall doesn't fast-forward the animation by that much. 10 s ceiling is
    // large enough that legitimate Tick calls (incl. test harness up to 1.5 s)
    // pass through unchanged.
    const float dtClamped = std::min(dt, 10.0f);

    const auto& clip = m_clips[static_cast<std::size_t>(m_animIndex)];
    m_time += dtClamped * m_speed;

    switch (clip.playbackType) {
        case 'l':
            if (clip.duration > 0.0f) {
                m_time = std::fmod(m_time, clip.duration);
                if (m_time < 0.0f) m_time += clip.duration;
            }
            break;
        case 'o':
            if (m_time >= clip.duration) {
                m_time    = clip.duration;
                m_playing = false;
            }
            break;
        case 'n':
        default:
            m_time = std::clamp(m_time, 0.0f, clip.duration);
            break;
    }
}

void AnimationPlayer::ApplyFrame(Parsers::ResolvedSkeleton& skel) {
    if (m_animIndex < 0) return;
    if (static_cast<std::size_t>(m_animIndex) >= m_clips.size()) return;

    // Seed from the skeleton's current local transforms: bones the clip does
    // not animate keep them.
    m_pose.resize(skel.bones.size());
    for (std::size_t i = 0; i < skel.bones.size(); ++i)
        std::memcpy(m_pose[i].local, skel.bones[i].localTransform, sizeof(float) * 16);
    m_interpolator.Sample(m_clips[static_cast<std::size_t>(m_animIndex)], m_time,
                          m_pose, m_kfCache);

    // Write sampled local transforms + recompose world transforms.
    for (std::size_t i = 0; i < skel.bones.size(); ++i)
        std::memcpy(skel.bones[i].localTransform, m_pose[i].local, sizeof(float) * 16);
    RecomposeWorldTransforms(skel);
}

void AnimationPlayer::InjectClipForTesting(AnimationClip clip) {
    m_clips.clear();
    m_clipParsed.clear();
    m_clips.push_back(std::move(clip));
    m_clipParsed.push_back(true);

    m_animIndex = 0;
    m_time      = 0.0f;
    m_playing   = false;   // tests call Play() explicitly
    m_kfCache.clear();
}

uint32_t AnimationPlayer::CurrentFrame() const {
    if (m_animIndex < 0) return 0;
    if (static_cast<std::size_t>(m_animIndex) >= m_clips.size()) return 0;
    const auto& c = m_clips[static_cast<std::size_t>(m_animIndex)];
    return static_cast<uint32_t>(m_time * static_cast<float>(c.fps));
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
