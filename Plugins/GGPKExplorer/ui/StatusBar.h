#pragma once
#include "UiText.h"
#include <imgui.h>
#include <string>
#include <atomic>
#include <cstdint>

namespace GGPKExp {

struct StatusBarState {
    uint32_t fileCount = 0;
    uint32_t dirCount = 0;
    int64_t totalSize = 0;
    std::string selectedName;
    int64_t selectedSize = -1;
    std::atomic<float> progress{ 0.0f };      // 0.0 = idle, 0..1 = working
    std::atomic<bool> progressActive{ false };
    LocText progressLabel;                    // set on the drawing thread before a job starts
};

inline void RenderStatusBar(StatusBarState& state) {
    float height = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##GGPKStatusBar", ImVec2(0, height), ImGuiChildFlags_None);

    // Padding: left indent
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);

    const float gap = ImGui::GetFontSize() * 0.75f;
    auto separator = [gap]() {
        ImGui::SameLine(0.0f, gap);
        ImGui::TextDisabled("|");
        ImGui::SameLine(0.0f, gap);
    };

    // File / dir counts
    ImGui::TextUnformatted(I18n::TrF("status_files", { { "count", Num("%u", state.fileCount) } }).c_str());
    separator();
    ImGui::TextUnformatted(I18n::TrF("status_dirs", { { "count", Num("%u", state.dirCount) } }).c_str());

    // Total size
    if (state.totalSize > 0) {
        separator();
        ImGui::TextUnformatted(I18n::TrF("status_total", { { "size", SizeText(state.totalSize) } }).c_str());
    }

    // Selected file info
    if (!state.selectedName.empty()) {
        separator();
        if (state.selectedSize >= 0) {
            ImGui::TextUnformatted(I18n::TrF("status_selected_size", { { "name", state.selectedName },
                                                                       { "size", SizeText(state.selectedSize) } }).c_str());
        } else {
            ImGui::TextUnformatted(I18n::TrF("status_selected", { { "name", state.selectedName } }).c_str());
        }
    }

    // Progress bar (when active)
    if (state.progressActive.load()) {
        ImGui::SameLine();
        float avail = ImGui::GetContentRegionAvail().x;
        const float wanted = ImGui::GetFontSize() * 15.0f;
        float barWidth = (avail > wanted) ? wanted : avail;
        if (barWidth > 30.0f) {
            const std::string label = TextOf(state.progressLabel);
            ImGui::ProgressBar(state.progress.load(), ImVec2(barWidth, 0),
                               label.empty() ? nullptr : label.c_str());
        }
    }

    ImGui::EndChild();
}

} // namespace GGPKExp
