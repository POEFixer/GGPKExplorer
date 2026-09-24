#pragma once

#include "AocSceneGraph.h"  // for AocChildMesh

#include <imgui.h>

#include <string>

namespace GGPKExp {
struct AocViewerState;  // forward-decl

namespace Aoc::Ui {

// Shared color constants used across Sockets / Info / Diagnostic / Preview.
// kOkColor/kMissingColor/kOptionalColor match Info tab's reference status tags.
// kWarningColor/kNeutralColor used by skin-mode badges and transform-modified
// indicators. Consolidated here to prevent drift between sites.
inline constexpr ImVec4 kOkColor       = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
inline constexpr ImVec4 kWarningColor  = ImVec4(1.0f, 0.7f, 0.3f, 1.0f);
inline constexpr ImVec4 kNeutralColor  = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
inline constexpr ImVec4 kMissingColor  = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
inline constexpr ImVec4 kOptionalColor = ImVec4(1.0f, 0.85f, 0.3f, 1.0f);

// Phase 5.2 skin-mode classification badge + tooltip.
// Caller places this SameLine after rendering the attachment's source path.
// Tooltip shows skinModeReason + up to 10 matched + 5 unmatched bone names.
void RenderSkinModeBadge(const AocChildMesh& child);

// Phase 5.2.1 transform override readout.
// Identity transform -> grey "(default)". Non-identity -> inline T/R/S values.
// Caller places this inline on the same row as source path.
void RenderTransformLine(const AocChildMesh& child);

// Phase 5.2.1 -- render the transform-override popup for a single attached child.
// Caller must ImGui::PushID(row) before calling to scope the popup ID.
// Returns true if any value was changed this frame; on change, sets
// child.pendingTransformSave = true (T8 debounce flag).
bool RenderTransformOverridePopup(AocChildMesh& child);

// Toast queue helper. Centralises the push_back pattern used across
// Sockets/3D-Preview tabs. time captured from ImGui::GetTime() internally.
void PushToast(AocViewerState& state, std::string text, bool isError);

} // namespace Aoc::Ui
} // namespace GGPKExp
