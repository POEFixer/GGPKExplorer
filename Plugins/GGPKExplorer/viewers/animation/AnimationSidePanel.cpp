#include "AnimationSidePanel.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace GGPKExp { namespace Animation {

namespace {

// Font Awesome 6 glyphs (host's atlas already loads FA6 ranges).
constexpr const char* ICON_PLAY  = "\xef\x81\x8b";   // U+F04B
constexpr const char* ICON_PAUSE = "\xef\x81\x8c";   // U+F04C

// Labels are user-facing; actual backend speed multiplier = label × 10.
// Matches empirical "normal speed feel": label "1.0x" → internal 10.0 plays
// at the rate a user perceives as 1x.
constexpr float kSpeedVals[]   = {0.5f, 1.0f, 5.0f, 10.0f, 15.0f, 20.0f, 25.0f};
constexpr const char* kSpeedLabels[] = {"0.05x", "0.1x", "0.5x", "1.0x", "1.5x", "2.0x", "2.5x"};
constexpr int kSpeedCount = 7;

int ClosestSpeedIndex(float speed) {
    int best = 3;
    float bestDiff = std::fabs(speed - kSpeedVals[3]);
    for (int i = 0; i < kSpeedCount; ++i) {
        const float d = std::fabs(speed - kSpeedVals[i]);
        if (d < bestDiff) { bestDiff = d; best = i; }
    }
    return best;
}

// Renders the chevron toggle button. Width = kCollapsedWidth.
// Writes to state.collapsed. Returns true if toggled this frame.
bool RenderChevron(AnimationPanelState& state, float panelHeight) {
    const char* glyph = state.collapsed ? "\xe2\x80\xba" : "\xe2\x80\xb9"; // rsaquo / lsaquo
    const ImVec2 size(kCollapsedWidth, panelHeight);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 4));
    const bool clicked = ImGui::Button(glyph, size);
    ImGui::PopStyleVar();
    if (clicked) state.collapsed = !state.collapsed;
    return clicked;
}

// Play/Pause icon button. Cyan background when playing.
// Returns true if clicked.
bool RenderPlayPause(AnimationPlayer& player) {
    const bool playing = player.IsPlaying();
    const ImVec4 bgIdle   (0.20f, 0.20f, 0.22f, 1.0f);
    const ImVec4 bgActive (0.20f, 0.55f, 0.85f, 1.0f);
    const ImVec4 bgHover  = playing
        ? ImVec4(0.30f, 0.65f, 0.95f, 1.0f)
        : ImVec4(0.28f, 0.28f, 0.30f, 1.0f);
    const ImVec4 bgPress  = playing
        ? ImVec4(0.15f, 0.45f, 0.75f, 1.0f)
        : ImVec4(0.14f, 0.14f, 0.16f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Button,        playing ? bgActive : bgIdle);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bgHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  bgPress);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

    const ImVec2 btnSize(28.0f, 28.0f);
    const char* icon = playing ? ICON_PAUSE : ICON_PLAY;

    // Centered icon: wrap button + overlay text.
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::Button("##playpause", btnSize);

    // Overlay the icon centered inside the button.
    const ImVec2 iconSize = ImGui::CalcTextSize(icon);
    const ImVec2 iconPos(cur.x + (btnSize.x - iconSize.x) * 0.5f,
                         cur.y + (btnSize.y - iconSize.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(iconPos, IM_COL32_WHITE, icon);

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    if (pressed) {
        if (playing) player.Pause();
        else         player.Play();
    }
    return pressed;
}

// Speed combo whose width shrinks/grows to fit the selected label.
// Returns true if user picked a different speed.
bool RenderSpeedCombo(AnimationPlayer& player, float* settingsSpeed) {
    const int curIdx = ClosestSpeedIndex(player.Speed());
    const char* label = kSpeedLabels[curIdx];

    const float labelW = ImGui::CalcTextSize(label).x;
    const float arrowW = ImGui::GetFrameHeight();
    const float pad    = ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(labelW + arrowW + pad + 4.0f);

    bool changed = false;
    if (ImGui::BeginCombo("##speed", label, ImGuiComboFlags_None)) {
        for (int i = 0; i < kSpeedCount; ++i) {
            const bool sel = (i == curIdx);
            if (ImGui::Selectable(kSpeedLabels[i], sel)) {
                if (i != curIdx) {
                    player.SetSpeed(kSpeedVals[i]);
                    if (settingsSpeed) *settingsSpeed = kSpeedVals[i];
                    changed = true;
                }
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void RenderAnimList(AnimationPlayer& player, float listHeight) {
    const auto& names = player.AnimNames();
    const auto& clips = player.Clips();
    if (names.empty() || listHeight < 32.0f) return;

    if (ImGui::BeginListBox("##animlist", ImVec2(-FLT_MIN, listHeight))) {
        const std::size_t sel = player.SelectedAnimIndex();
        for (std::size_t i = 0; i < names.size(); ++i) {
            const bool isSel = (i == sel);
            const float dur = (i < clips.size()) ? clips[i].duration : 0.0f;

            char row[192];
            std::snprintf(row, sizeof(row), "%s%s  %.2fs",
                          isSel ? "* " : "  ",
                          names[i].c_str(),
                          dur);

            if (ImGui::Selectable(row, isSel,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                player.SelectAnimation(i);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    player.Play();
                }
            }
        }
        ImGui::EndListBox();
    }
}

void RenderStatusRow(AnimationPlayer& player) {
    const std::size_t sel = player.SelectedAnimIndex();
    if (sel == static_cast<std::size_t>(-1)) return;
    const auto& clips = player.Clips();
    if (sel >= clips.size()) return;

    const auto& clip = clips[sel];
    const uint32_t total = static_cast<uint32_t>(clip.duration * static_cast<float>(clip.fps));
    ImGui::Text("frame %u/%u", player.CurrentFrame(), total);

    ImGui::SameLine();
    ImVec4 pbColor(0.60f, 0.65f, 0.75f, 1.0f);
    const char* pbLabel = "[none]";
    switch (clip.playbackType) {
        case 'l': pbColor = ImVec4(0.35f, 0.60f, 0.95f, 1.0f); pbLabel = "[loop]"; break;
        case 'o': pbColor = ImVec4(0.95f, 0.65f, 0.30f, 1.0f); pbLabel = "[once]"; break;
        default:  break;
    }
    ImGui::TextColored(pbColor, "%s", pbLabel);

    const auto& warnings = player.GetWarnings();
    if (!warnings.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.30f, 1.0f),
                          "!  %zu warnings", warnings.size());
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            int maxShow = 10;
            for (const auto& w : warnings) {
                if (--maxShow < 0) { ImGui::Text("... (more)"); break; }
                ImGui::TextUnformatted(w.c_str());
            }
            ImGui::EndTooltip();
        }
    }
}

void RenderScrubber(AnimationPlayer& player) {
    const std::size_t sel = player.SelectedAnimIndex();
    if (sel == static_cast<std::size_t>(-1)) return;
    const auto& clips = player.Clips();
    if (sel >= clips.size()) return;

    float t = player.CurrentTime();
    ImGui::SetNextItemWidth(-FLT_MIN);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2fs / %.2fs", t, clips[sel].duration);
    if (ImGui::SliderFloat("##scrub", &t, 0.0f, clips[sel].duration,
                          buf, ImGuiSliderFlags_NoInput)) {
        player.SeekSeconds(t);
    }
}

} // anonymous namespace

float Width(const AnimationPanelState& state) {
    if (state.collapsed) return kCollapsedWidth;
    float w = state.width;
    if (w < kMinWidth) w = kMinWidth;
    if (w > kMaxWidth) w = kMaxWidth;
    return w;
}

bool RenderAnimationSidePanel(AnimationPlayer& player,
                              AnimationPanelState& state,
                              float* playbackSpeed,
                              float panelHeight) {
    const float panelW = Width(state);
    bool changed = false;

    // Collapsed: just the chevron column.
    if (state.collapsed) {
        ImGui::BeginChild("##animpanel_collapsed",
                          ImVec2(panelW, panelHeight),
                          false,
                          ImGuiWindowFlags_NoScrollbar);
        changed |= RenderChevron(state, panelHeight);
        ImGui::EndChild();
        return changed;
    }

    // Expanded layout: content on the LEFT, chevron (20 px) on the RIGHT edge.
    ImGui::BeginChild("##animpanel_expanded",
                      ImVec2(panelW, panelHeight),
                      false,
                      ImGuiWindowFlags_NoScrollbar);

    const float contentW = panelW - kCollapsedWidth - 4.0f;

    // Content column (nested child to constrain width to contentW).
    ImGui::BeginChild("##animpanel_content",
                      ImVec2(contentW, panelHeight),
                      false,
                      ImGuiWindowFlags_NoScrollbar);

    const auto& names = player.AnimNames();
    if (names.empty()) {
        ImGui::TextDisabled("(no animations)");
    } else {
        // Row 1: Play/Pause + Follow toggle + Speed
        RenderPlayPause(player);
        ImGui::SameLine();
        if (ImGui::Checkbox("Follow", &state.followModel)) {
            changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Camera follows the model during animation playback.");
        }
        ImGui::SameLine();
        if (RenderSpeedCombo(player, playbackSpeed)) changed = true;

        ImGui::Separator();
        ImGui::TextDisabled("Animations");

        // List: fills between header and the bottom rows.
        // Reserve: 1 line (status) + 1 line (scrubber) + spacing.
        const float bottomReserve = ImGui::GetFrameHeight() * 2.0f
                                  + ImGui::GetStyle().ItemSpacing.y * 3.0f;
        const float listH = ImGui::GetContentRegionAvail().y - bottomReserve;
        RenderAnimList(player, std::max(48.0f, listH));

        ImGui::Separator();
        RenderStatusRow(player);
        RenderScrubber(player);
    }

    ImGui::EndChild();   // end ##animpanel_content

    // Chevron column on the right edge.
    ImGui::SameLine(0.0f, 4.0f);
    changed |= RenderChevron(state, panelHeight);

    ImGui::EndChild();   // end ##animpanel_expanded
    return changed;
}

}} // namespace GGPKExp::Animation
