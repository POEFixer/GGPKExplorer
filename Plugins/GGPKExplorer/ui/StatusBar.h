#pragma once
#include <imgui.h>
#include <string>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace GGPKExp {

struct StatusBarState {
    uint32_t fileCount = 0;
    uint32_t dirCount = 0;
    int64_t totalSize = 0;
    std::string selectedName;
    int64_t selectedSize = -1;
    std::atomic<float> progress{ 0.0f };      // 0.0 = idle, 0..1 = working
    std::atomic<bool> progressActive{ false };
    std::string progressLabel;
};

// Format a byte count as human-readable string (B, KB, MB, GB).
inline const char* FormatSize(int64_t bytes, char* buf, size_t bufSize) {
    if (bytes < 0) {
        snprintf(buf, bufSize, "?");
    } else if (bytes < 1024) {
        snprintf(buf, bufSize, "%lld B", static_cast<long long>(bytes));
    } else if (bytes < 1024 * 1024) {
        snprintf(buf, bufSize, "%.1f KB", bytes / 1024.0);
    } else if (bytes < 1024LL * 1024 * 1024) {
        snprintf(buf, bufSize, "%.1f MB", bytes / (1024.0 * 1024.0));
    } else {
        snprintf(buf, bufSize, "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

inline void RenderStatusBar(StatusBarState& state) {
    float height = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##GGPKStatusBar", ImVec2(0, height), ImGuiChildFlags_None);

    // Padding: left indent
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);

    // File / dir counts
    ImGui::Text("Files: %u  |  Dirs: %u", state.fileCount, state.dirCount);

    // Total size
    if (state.totalSize > 0) {
        char sizeBuf[64];
        FormatSize(state.totalSize, sizeBuf, sizeof(sizeBuf));
        ImGui::SameLine();
        ImGui::Text("  |  Total: %s", sizeBuf);
    }

    // Selected file info
    if (!state.selectedName.empty()) {
        ImGui::SameLine();
        if (state.selectedSize >= 0) {
            char sizeBuf[64];
            FormatSize(state.selectedSize, sizeBuf, sizeof(sizeBuf));
            ImGui::Text("  |  Selected: %s (%s)", state.selectedName.c_str(), sizeBuf);
        } else {
            ImGui::Text("  |  Selected: %s", state.selectedName.c_str());
        }
    }

    // Progress bar (when active)
    if (state.progressActive.load()) {
        ImGui::SameLine();
        float avail = ImGui::GetContentRegionAvail().x;
        float barWidth = (avail > 200.0f) ? 200.0f : avail;
        if (barWidth > 30.0f) {
            ImGui::ProgressBar(state.progress.load(), ImVec2(barWidth, 0),
                               state.progressLabel.empty() ? nullptr : state.progressLabel.c_str());
        }
    }

    ImGui::EndChild();
}

} // namespace GGPKExp
