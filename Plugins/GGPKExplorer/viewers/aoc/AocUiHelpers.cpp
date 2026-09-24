#include "AocUiHelpers.h"

#include "../AocViewer.h"  // for AocViewerState::UiToast + m_uiToasts

#include <cstdio>

namespace GGPKExp::Aoc::Ui {

void RenderSkinModeBadge(const AocChildMesh& child) {
    const char* label = nullptr;
    ImVec4 color;

    char buf[256];
    if (child.skinMode == ChildSkinMode::Skinned) {
        std::snprintf(buf, sizeof(buf), "[Skinned %d/%d]",
                      child.matchedBoneCount,
                      static_cast<int>(child.boneRemap.size()));
        label = buf;
        color = kOkColor;
    } else {
        const bool isStatic = child.isStaticMesh;
        std::snprintf(buf, sizeof(buf), "[Rigid: %s]",
                      child.skinModeReason.empty()
                        ? "unknown"
                        : child.skinModeReason.c_str());
        label = buf;
        color = isStatic ? kNeutralColor : kWarningColor;
    }

    ImGui::SameLine();
    ImGui::TextColored(color, "%s", label);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(child.skinModeReason.c_str());

        if (child.skinMode == ChildSkinMode::Skinned) {
            ImGui::Separator();
            int matchesShown = 0, unmatchedShown = 0;
            ImGui::TextUnformatted("Matched:");
            for (std::size_t i = 0; i < child.boneRemap.size() && matchesShown < 10; ++i) {
                if (child.boneRemap[i] >= 0) {
                    ImGui::BulletText("%s", child.childResolvedSkel.bones[i].name.c_str());
                    ++matchesShown;
                }
            }
            if (matchesShown == 0) ImGui::BulletText("(none shown)");

            const int unmatched =
                static_cast<int>(child.boneRemap.size()) - child.matchedBoneCount;
            if (unmatched > 0) {
                ImGui::Separator();
                ImGui::Text("Unmatched (%d -- using socket fallback):", unmatched);
                for (std::size_t i = 0; i < child.boneRemap.size() && unmatchedShown < 5; ++i) {
                    if (child.boneRemap[i] < 0) {
                        ImGui::BulletText("%s", child.childResolvedSkel.bones[i].name.c_str());
                        ++unmatchedShown;
                    }
                }
            }
        }
        ImGui::EndTooltip();
    }
}

void RenderTransformLine(const AocChildMesh& child) {
    if (IsTransformIdentity(child)) {
        ImGui::TextDisabled("Transform: (default)");
        return;
    }
    ImGui::Text("Transform: T [%.2f, %.2f, %.2f]  R [%.1f, %.1f, %.1f]  S [%.2f, %.2f, %.2f]",
                child.overrideT[0], child.overrideT[1], child.overrideT[2],
                child.overrideR[0], child.overrideR[1], child.overrideR[2],
                child.overrideS[0], child.overrideS[1], child.overrideS[2]);
}

// Phase 5.2.1 -- render the transform-override popup for a single attached child.
// The caller has already pushed an ImGui ID (per-row PushID at column 0), so
// the popup ID "##TOverride" is unique per socket row.
//
// Returns true if any value was changed this frame (caller should Save).
bool RenderTransformOverridePopup(AocChildMesh& child) {
    bool changed = false;

    if (!ImGui::BeginPopup("##TOverride")) {
        return false;
    }

    ImGui::Text("Socket: %s", child.socketName.c_str());

    // Basename for the source path to keep the header compact.
    std::string base = child.sourcePath;
    const std::size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    ImGui::TextDisabled("Source: %s", base.c_str());

    ImGui::Separator();

    const float kLabelWidth = 120.0f;
    const float kDragWidth  = 180.0f;

    // Translation row -------------------------------------------------------
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Translation (m)");
    ImGui::SameLine(kLabelWidth);
    ImGui::SetNextItemWidth(kDragWidth);
    if (ImGui::DragFloat3("##TOvrT", child.overrideT.data(),
                          0.01f, -10.0f, 10.0f, "%.3f")) {
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##T")) {
        child.overrideT = { 0.0f, 0.0f, 0.0f };
        changed = true;
    }

    // Rotation row ----------------------------------------------------------
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Rotation (deg)");
    ImGui::SameLine(kLabelWidth);
    ImGui::SetNextItemWidth(kDragWidth);
    if (ImGui::DragFloat3("##TOvrR", child.overrideR.data(),
                          0.5f, -360.0f, 360.0f, "%.1f")) {
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##R")) {
        child.overrideR = { 0.0f, 0.0f, 0.0f };
        changed = true;
    }

    // Scale row -------------------------------------------------------------
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Scale");
    ImGui::SameLine(kLabelWidth);
    ImGui::SetNextItemWidth(kDragWidth);
    if (ImGui::DragFloat3("##TOvrS", child.overrideS.data(),
                          0.01f, 0.001f, 100.0f, "%.3f")) {
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##S")) {
        child.overrideS = { 1.0f, 1.0f, 1.0f };
        changed = true;
    }

    // Fully-skinned hint ----------------------------------------------------
    const bool fullyMatched =
        child.skinMode == ChildSkinMode::Skinned &&
        static_cast<int>(child.boneRemap.size()) == child.matchedBoneCount &&
        child.matchedBoneCount > 0;
    if (fullyMatched) {
        ImGui::Separator();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled(
            "Fully skinned -- override has no visible effect; unmatched-bone parts only.");
        ImGui::PopTextWrapPos();
    }

    ImGui::Separator();

    // Reset All + Close -----------------------------------------------------
    if (ImGui::Button("Reset All")) {
        child.overrideT = { 0.0f, 0.0f, 0.0f };
        child.overrideR = { 0.0f, 0.0f, 0.0f };
        child.overrideS = { 1.0f, 1.0f, 1.0f };
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        ImGui::CloseCurrentPopup();
    }

    if (changed) {
        child.pendingTransformSave = true;
    }

    ImGui::EndPopup();
    return changed;
}

void PushToast(AocViewerState& state, std::string text, bool isError) {
    state.m_uiToasts.push_back({
        std::move(text),
        static_cast<float>(ImGui::GetTime()),
        isError
    });
}

} // namespace GGPKExp::Aoc::Ui
