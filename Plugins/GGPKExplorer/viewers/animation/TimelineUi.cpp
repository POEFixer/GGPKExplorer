#include "TimelineUi.h"

#include <imgui.h>

#include <cctype>
#include <cmath>

namespace GGPKExp { namespace Animation {

namespace {

bool ContainsCaseInsensitive(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return true;
    std::string lh = hay;
    for (auto& c : lh) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string ln = needle;
    for (auto& c : ln) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lh.find(ln) != std::string::npos;
}

ImVec4 PlaybackTypeColor(char type) {
    // FluentTheme integration: for now use cached ImGui defaults.
    // Later consumer (Sprint 3 polish) wires through ThemeManager::GetCachedTheme().
    switch (type) {
        case 'l': return ImVec4(0.35f, 0.60f, 0.95f, 1.0f);  // blue - loop
        case 'o': return ImVec4(0.95f, 0.65f, 0.30f, 1.0f);  // orange - once
        case 'n': default: return ImVec4(0.60f, 0.65f, 0.75f, 1.0f); // muted - none
    }
}

const char* PlaybackTypeLabel(char type) {
    switch (type) {
        case 'l': return "[loop]";
        case 'o': return "[once]";
        case 'n': default: return "[none]";
    }
}

} // anonymous namespace

void RenderTimelineBar(AnimationPlayer& player, float height) {
    const auto& names = player.AnimNames();
    const auto& clips = player.Clips();
    if (names.empty() || height <= 1.0f) return;

    ImGui::BeginChild("##timeline_bar", ImVec2(0, height),
                      false, ImGuiWindowFlags_NoScrollbar);

    const float availW = ImGui::GetContentRegionAvail().x;
    const bool compact = availW < 700.0f;

    const std::size_t sel = player.SelectedAnimIndex();
    const bool hasSel = (sel != static_cast<std::size_t>(-1));

    // ---- Row 1: controls ----
    // Play / Pause
    if (ImGui::Button(player.IsPlaying() ? "||" : ">", ImVec2(28.0f, 24.0f))) {
        if (player.IsPlaying()) player.Pause();
        else                    player.Play();
    }

    ImGui::SameLine();
    // Animation dropdown — Combo or filtered BeginCombo.
    ImGui::SetNextItemWidth(compact ? 140.0f : 200.0f);
    const char* currentName = hasSel ? names[sel].c_str() : "(select anim)";
    if (names.size() <= 20) {
        // Simple combo.
        if (ImGui::BeginCombo("##animsel", currentName)) {
            for (std::size_t i = 0; i < names.size(); ++i) {
                bool s = (i == sel);
                std::string label = names[i];
                if (i < clips.size() && clips[i].compressed && !clips[i].name2.empty()) {
                    label += " [ref]";
                }
                if (ImGui::Selectable(label.c_str(), s)) player.SelectAnimation(i);
                if (s) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    } else {
        // Filtered combo for >20 anims (monster rigs etc.).
        static char filter[64] = {};
        if (ImGui::BeginCombo("##animselfilt", currentName)) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##filter", filter, IM_COUNTOF(filter),
                             ImGuiInputTextFlags_AutoSelectAll);
            ImGui::Separator();
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (filter[0] && !ContainsCaseInsensitive(names[i], filter)) continue;
                bool s = (i == sel);
                std::string label = names[i];
                if (i < clips.size() && clips[i].compressed && !clips[i].name2.empty()) {
                    label += " [ref]";
                }
                if (ImGui::Selectable(label.c_str(), s)) player.SelectAnimation(i);
                if (s) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    if (hasSel) {
        const auto& clip = clips[sel];

        // Time display.
        ImGui::SameLine();
        ImGui::Text("%.2fs / %.2fs", player.CurrentTime(), player.CurrentDuration());

        // Frame counter (hidden in compact).
        if (!compact) {
            ImGui::SameLine();
            uint32_t total = static_cast<uint32_t>(clip.duration * static_cast<float>(clip.fps));
            ImGui::Text("frame %u/%u", player.CurrentFrame(), total);
        }

        // Playback-type indicator.
        ImGui::SameLine();
        ImGui::TextColored(PlaybackTypeColor(clip.playbackType),
                          "%s", PlaybackTypeLabel(clip.playbackType));

        // Speed combo.
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f);
        const char* kSpeeds[] = { "0.25x", "0.5x", "1.0x", "2.0x" };
        const float  kSpeedVals[] = { 0.25f, 0.5f, 1.0f, 2.0f };
        int speedIdx = 2;
        for (int i = 0; i < 4; ++i)
            if (std::fabs(player.Speed() - kSpeedVals[i]) < 1e-3f) speedIdx = i;
        if (ImGui::Combo("##speed", &speedIdx, kSpeeds, 4)) {
            player.SetSpeed(kSpeedVals[speedIdx]);
        }

        // Warnings badge.
        const auto& warnings = player.GetWarnings();
        if (!warnings.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.30f, 1.0f),
                              "  !  %zu warnings", warnings.size());
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

        // ---- Row 2: scrubber ----
        float t = player.CurrentTime();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##scrub", &t, 0.0f, clip.duration,
                              "", ImGuiSliderFlags_NoInput)) {
            player.SeekSeconds(t);
        }
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled("(no animation selected)");
    }

    ImGui::EndChild();
}

}} // namespace GGPKExp::Animation
