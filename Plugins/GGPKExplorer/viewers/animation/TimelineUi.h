#pragma once

#include "AnimationPlayer.h"

namespace GGPKExp { namespace Animation {

// Renders a bottom-anchored timeline bar consuming 'height' pixels of
// vertical space. Two rows: controls (play/pause, dropdown, time, frame,
// playback-type, speed, warnings) + full-width scrubber.
//
// Consumer pattern:
//   ImVec2 canvasSize = ImGui::GetContentRegionAvail();
//   canvasSize.y -= kTimelineH;                         // reserve
//   // ... render canvas at canvasSize ...
//   Animation::RenderTimelineBar(player, kTimelineH);   // anchored at bottom
//
// Compact mode kicks in when ImGui::GetContentRegionAvail().x < 700 px.
void RenderTimelineBar(AnimationPlayer& player, float height);

}} // namespace GGPKExp::Animation
