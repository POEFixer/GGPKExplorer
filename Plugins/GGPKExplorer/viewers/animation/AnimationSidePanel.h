#pragma once

#include "../../core/GGPKExplorerSettings.h"
#include "AnimationPlayer.h"

namespace GGPKExp { namespace Animation {

// Renders the animation panel as a right-anchored strip at the current
// cursor position. Caller must have reserved horizontal space equal to
// `Width(state)` BEFORE calling this. `panelHeight` is the full vertical
// space the panel occupies.
//
// Returns true if any persistent setting changed this frame (collapse
// toggle or speed combo). Caller is responsible for bumping any dirty
// timestamp for debounced saves.
bool RenderAnimationSidePanel(AnimationPlayer& player,
                              AnimationPanelState& state,
                              float* playbackSpeed,   // new: if non-null,
                                                      // combo also writes here
                              float panelHeight);

// Width in pixels that the panel will occupy given `state`.
// Collapsed: kCollapsedWidth. Expanded: state.width.
float Width(const AnimationPanelState& state);

inline constexpr float kCollapsedWidth = 20.0f;
inline constexpr float kMinWidth       = 180.0f;
inline constexpr float kMaxWidth       = 800.0f;

}} // namespace GGPKExp::Animation
