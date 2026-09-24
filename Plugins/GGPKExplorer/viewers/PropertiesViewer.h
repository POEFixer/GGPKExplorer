#pragma once
#include "../core/FileTypeDetector.h"
#include <imgui.h>
#include <string>
#include <cstdint>
#include <cstdio>

namespace GGPKExp {

struct FileProperties {
    std::string name;
    std::string fullPath;
    int64_t uncompressedSize = 0;
    int64_t compressedSize = -1; // -1 = unknown
    std::string bundleName;
    int32_t bundleOffset = 0;
    FileType type = FileType::Unknown;
    std::string formatDetail;
    std::string sha256;
};

inline void RenderPropertiesViewer(const FileProperties& props) {
    if (ImGui::BeginTable("##Props", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        auto row = [](const char* label, const char* value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", label);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", value);
        };

        row("File Name", props.name.c_str());
        row("Full Path", props.fullPath.c_str());

        // Format size
        char sizeBuf[128];
        if (props.uncompressedSize >= 1024 * 1024)
            snprintf(sizeBuf, sizeof(sizeBuf), "%lld bytes (%.2f MB)",
                     (long long)props.uncompressedSize,
                     props.uncompressedSize / (1024.0 * 1024.0));
        else if (props.uncompressedSize >= 1024)
            snprintf(sizeBuf, sizeof(sizeBuf), "%lld bytes (%.2f KB)",
                     (long long)props.uncompressedSize,
                     props.uncompressedSize / 1024.0);
        else
            snprintf(sizeBuf, sizeof(sizeBuf), "%lld bytes", (long long)props.uncompressedSize);
        row("Size", sizeBuf);

        if (props.compressedSize >= 0) {
            double ratio = props.uncompressedSize > 0 ?
                (100.0 * props.compressedSize / props.uncompressedSize) : 0.0;
            snprintf(sizeBuf, sizeof(sizeBuf), "%lld bytes (%.1f%%)",
                     (long long)props.compressedSize, ratio);
            row("Compressed", sizeBuf);
        }

        row("File Type", FileTypeToString(props.type));
        if (!props.formatDetail.empty())
            row("Format", props.formatDetail.c_str());
        if (!props.bundleName.empty())
            row("Bundle", props.bundleName.c_str());
        if (!props.sha256.empty())
            row("SHA-256", props.sha256.c_str());

        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::Button("Copy Path"))
        ImGui::SetClipboardText(props.fullPath.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Copy Name"))
        ImGui::SetClipboardText(props.name.c_str());
}

} // namespace GGPKExp
