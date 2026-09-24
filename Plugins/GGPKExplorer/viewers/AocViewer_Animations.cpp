#include "AocViewer.h"

#include "animation/AnimationPlayer.h"
#include "animation/AnimationSidePanel.h"
#include "animation/AnimationTypes.h"
#include "animation/TimelineUi.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace GGPKExp {

// --------------------------------------------------------------------------
// Animations tab
//
// Layout:
//
//   +----------------------------------------------------------+
//   | TimelineBar  (play/pause, dropdown, scrubber, speed)     |
//   +---------------------------------+------------------------+
//   | Clip details                    | AnimationSidePanel    |
//   | (playback type, fps, duration,  | (layers, speed combo, |
//   | frame count, warnings)          | bone-group mask, list)|
//   |                                 |                       |
//   +---------------------------------+------------------------+
//
// The side panel on the right is right-anchored using the same pattern as
// MeshViewer::Render3DView — caller reserves horizontal space equal to
// `Animation::Width(state)`, then calls `RenderAnimationSidePanel` with
// SameLine. A fallback AnimationPanelState is used if the plugin has not
// yet wired one via `SetAnimPanelState()`.
// --------------------------------------------------------------------------

void AocViewer::RenderAnimationsTab() {
    auto& s = m_state;
    auto& player = s.m_animPlayer;

    // ---- Drive playback ----
    //
    // The 3D Preview tab ticks the player itself; when the user is viewing
    // the Animations tab alone (no 3D canvas in this frame), we still need
    // to advance the clock so Play/Pause and the scrubber behave normally.
    // Tick() early-exits on non-playing layers, so calling it here when the
    // 3D tab is inactive is safe; only the active tab renders per frame.
    const float animDt = ImGui::GetIO().DeltaTime;
    player.Tick(animDt);

    // ---- Empty-state guard ----
    //
    // AnimNames() holds one entry per parsed animation. If the .aoc has no
    // animations (or the skeleton hasn't been attached) everything below
    // would render as a single-animation no-op, so surface the state
    // explicitly.
    const auto& names = player.AnimNames();
    if (names.empty()) {
        ImGui::TextDisabled("(no animations in this file)");
        return;
    }

    // ---- Shared panel state (with defensive fallback) ----
    static AnimationPanelState s_fallbackPanelState;
    AnimationPanelState& panelState =
        s.m_animPanelState ? *s.m_animPanelState : s_fallbackPanelState;

    // ---- Row 1: full-width timeline bar at top ----
    //
    // TimelineBar renders play/pause, the animation dropdown, time, frame,
    // playback-type badge, speed combo, warnings, then a full-width
    // scrubber — two rows, ~48-56 px tall. Use a dedicated height so the
    // bar doesn't consume arbitrary vertical space.
    constexpr float kTimelineH = 56.0f;
    Animation::RenderTimelineBar(player, kTimelineH);

    ImGui::Separator();

    // ---- Row 2: split — clip details (left) + side panel (right) ----
    //
    // Reserve the side panel's width before rendering the left child so
    // the side panel aligns to the right edge via SameLine, matching
    // MeshViewer's layout pattern.
    const float panelW = Animation::Width(panelState) + 4.0f;
    const ImVec2 regionAvail = ImGui::GetContentRegionAvail();
    const float leftW = std::max(80.0f, regionAvail.x - panelW);
    const float rowH  = std::max(64.0f, regionAvail.y);

    // ---- Left: selected-clip details ----
    ImGui::BeginChild("##AocAnimClipInfo", ImVec2(leftW, rowH), true);
    {
        const std::size_t sel = player.SelectedAnimIndex();
        const bool hasSel = (sel != static_cast<std::size_t>(-1));
        const auto& clips = player.Clips();

        if (!hasSel) {
            ImGui::TextDisabled("Select an animation from the timeline "
                                "dropdown or the side panel list.");
        } else {
            // Name and (if ref) the base clip it extends.
            ImGui::Text("Selected: %s", names[sel].c_str());

            // Clips are lazy-parsed — if the slot is still empty (user
            // hasn't selected it yet) we only have the name to show.
            const bool haveClip = (sel < clips.size());
            const Animation::AnimationClip empty{};
            const Animation::AnimationClip& clip = haveClip ? clips[sel] : empty;

            ImGui::Separator();

            // Playback policy badge.
            const char* policyLabel = "none";
            switch (clip.playbackType) {
                case 'l': policyLabel = "loop"; break;
                case 'o': policyLabel = "once"; break;
                case 'n': default: policyLabel = "none"; break;
            }
            ImGui::Text("Playback:    [%s]", policyLabel);
            ImGui::Text("Frame rate:  %u fps", static_cast<unsigned>(clip.fps));
            ImGui::Text("Duration:    %.3f s", clip.duration);

            const std::size_t frameCount = (clip.fps > 0)
                ? static_cast<std::size_t>(clip.duration *
                                           static_cast<float>(clip.fps) + 0.5f)
                : 0u;
            ImGui::Text("Frames:      %zu", frameCount);
            ImGui::Text("Tracks:      %zu", clip.tracks.size());

            if (clip.compressed) {
                ImGui::TextDisabled("(compressed)");
            }
            if (!clip.name2.empty()) {
                ImGui::Text("Extends:     %s", clip.name2.c_str());
            }

            if (!clip.warnings.empty()) {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f),
                                   "Warnings (%zu):",
                                   clip.warnings.size());
                for (const auto& w : clip.warnings) {
                    ImGui::BulletText("%s", w.c_str());
                }
            }

            // Player-level warnings (decode failures, ref resolution issues,
            // skinning truncation surfaced via PushWarning).
            const auto& playerWarnings = player.GetWarnings();
            if (!playerWarnings.empty()) {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f),
                                   "Player warnings (%zu):",
                                   playerWarnings.size());
                for (const auto& w : playerWarnings) {
                    ImGui::BulletText("%s", w.c_str());
                }
            }
        }
    }
    ImGui::EndChild();

    // ---- Right: reusable side panel (speed, layers, bone-group mask) ----
    ImGui::SameLine(0.0f, 4.0f);
    const bool changed = Animation::RenderAnimationSidePanel(
        player,
        panelState,
        m_playbackSpeed,
        rowH);
    if (changed) {
        s.m_animPanelSettingsDirty = true;
    }
}

} // namespace GGPKExp
