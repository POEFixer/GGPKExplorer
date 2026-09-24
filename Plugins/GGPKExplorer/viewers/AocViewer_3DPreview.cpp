#include "AocViewer.h"
#include "mesh/MeshRenderer.h"
#include "mesh/MeshMaterial.h"
#include "mesh/MeshCamera.h"
#include "animation/SkinningPalette.h"
#include "animation/MatrixMath.h"
#include "aoc/AocLightSystem.h"
#include "aoc/AocUiHelpers.h"      // T10: tabbed sidepanel uses Aoc::Ui helpers
#include "MeshShaders.h"           // kMaxSkinBones

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// The 3D Preview tab renders the AOC's main skin mesh using the shared
// `MeshRenderer` + `MeshMaterial` subsystems. The calling pattern mirrors
// `MeshViewer::Render3DView` (verified reference implementation) so the
// same D3D11 offscreen pipeline is reused. Grid anchoring, socket overlay,
// skeleton overlay, and bounding-box visualisation are drawn on top with
// `ImDrawList` — this matches MeshViewer's overlay technique and sidesteps
// the need to extend MeshRenderer itself in Phase 1.
//
// Task 6 will wire real bind-inverse matrices into AocSceneGraph, at which
// point GPU skinning will produce animated poses. Until that lands, the
// mesh renders in bind pose (palette is not passed to RenderScene, so the
// shader-side `hasSkinning` flag stays false).
//
// Task 13 is reserved for promoting the overlay draws into MeshRenderer
// proper; in Phase 1 we keep them here to avoid touching MeshRenderer.

namespace GGPKExp {

namespace {

// ---------- helpers ------------------------------------------------------

// Projects a world-space point into screen space using the camera's VP matrix
// and the viewport rectangle. Returns false for points behind the camera so
// the caller can skip lines with one endpoint off-screen behind the eye.
bool ProjectWorldToScreen(const float* vp, const ImVec2& canvasPos,
                          const ImVec2& canvasSize,
                          float px, float py, float pz,
                          ImVec2& out) {
    const float x = px * vp[0] + py * vp[4] + pz * vp[8]  + vp[12];
    const float y = px * vp[1] + py * vp[5] + pz * vp[9]  + vp[13];
    const float w = px * vp[3] + py * vp[7] + pz * vp[11] + vp[15];
    if (w < 0.0001f) return false;
    const float nx = x / w;
    const float ny = y / w;
    out.x = canvasPos.x + (nx * 0.5f + 0.5f) * canvasSize.x;
    out.y = canvasPos.y + (-ny * 0.5f + 0.5f) * canvasSize.y;
    return true;
}

// Picks a round-number grid step sized to ~1/10 of the mesh span.
float ChooseGridStep(float span) {
    if (span < 1.0f) span = 1.0f;
    const float target = span / 10.0f;
    if (target >= 100.0f) return 100.0f;
    if (target >= 50.0f)  return 50.0f;
    if (target >= 20.0f)  return 20.0f;
    if (target >= 10.0f)  return 10.0f;
    if (target >= 5.0f)   return 5.0f;
    if (target >= 2.0f)   return 2.0f;
    if (target >= 1.0f)   return 1.0f;
    if (target >= 0.5f)   return 0.5f;
    return 0.1f;
}

// T10: Sidebar panel — tabbed layout (View / State / Attach / Camera).
// Replaces the former stack of 5 CollapsingHeaders with an ImGui::TabBar so
// the sidepanel stays compact under the fixed sidebar width.
void DrawViewTogglesPanel(AocViewerState& s) {
    if (!ImGui::BeginTabBar("##PreviewSide", ImGuiTabBarFlags_None)) {
        return;
    }

    // ----------------------------------------------------------------------
    // View sub-tab — former "View Toggles" body (toggles, grid, lights,
    // phase-banner rows, Active Light State combo, Audio volume/mute).
    // ----------------------------------------------------------------------
    if (ImGui::BeginTabItem("View")) {
        ImGui::Checkbox("Show Sockets",        &s.m_view.showSockets);
        ImGui::Checkbox("Show Socket Labels",  &s.m_view.showSocketLabels);
        ImGui::Checkbox("Show Lights",         &s.m_view.showLights);
        ImGui::Checkbox("Show Audio Markers",  &s.m_view.showAudioMarkers);   // Phase 2 (no-op)
        ImGui::Separator();
        ImGui::Checkbox("Show Grid",           &s.m_view.showGrid);
        const char* gridModes[] = { "Mesh Bounds", "Skeleton", "World Origin" };
        ImGui::Combo("Grid Anchor",            &s.m_view.gridAnchor,
                     gridModes, IM_ARRAYSIZE(gridModes));
        ImGui::Checkbox("Show Parent Lines",   &s.m_view.showParentLines);
        ImGui::Separator();
        ImGui::Checkbox("Wireframe",           &s.m_view.wireframe);

        ImGui::SeparatorText("Phase 2");
        ImGui::Checkbox("Show Light Labels",   &s.m_view.showLightLabels);

        ImGui::SeparatorText("Phase 3");
        ImGui::Checkbox("Particles",        &s.m_view.showParticles);
        {
            const bool disabled = !s.m_view.showParticles;
            if (disabled) ImGui::BeginDisabled();
            ImGui::Checkbox("Particle Labels", &s.m_view.showParticleLabels);
            if (disabled) ImGui::EndDisabled();
        }

        // Active Light State dropdown — only meaningful when lights are actually
        // present in this .aoc. Mirrors the animation-name selector pattern used
        // by Phase 1's animation combo.
        if (s.m_lights.LightCount() > 0) {
            const std::vector<std::string> states = s.m_lights.CollectStateNames();
            if (!states.empty()) {
                const char* current = s.m_activeLightStateName.empty()
                    ? "(default per light)"
                    : s.m_activeLightStateName.c_str();
                if (ImGui::BeginCombo("Active Light State", current)) {
                    if (ImGui::Selectable("(default per light)",
                                          s.m_activeLightStateName.empty())) {
                        s.m_activeLightStateName.clear();
                        s.m_lights.SetActiveStateName("");
                    }
                    for (const auto& n : states) {
                        const bool sel = (n == s.m_activeLightStateName);
                        if (ImGui::Selectable(n.c_str(), sel)) {
                            s.m_activeLightStateName = n;
                            s.m_lights.SetActiveStateName(n);
                        }
                    }
                    ImGui::EndCombo();
                }
            }
        }

        ImGui::SeparatorText("Audio");

        if (s.m_audio) {
            float vol = s.m_audio->MasterVolume();
            if (ImGui::SliderFloat("Master Volume", &vol, 0.0f, 1.0f, "%.2f"))
                s.m_audio->SetMasterVolume(vol);

            bool muted = s.m_audio->Muted();
            if (ImGui::Checkbox("Mute", &muted))
                s.m_audio->SetMuted(muted);

            if (!s.m_audio->IsOperational())
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                   "Audio device failed to initialise");
        } else {
            ImGui::TextDisabled("Audio system not initialised");
        }
        ImGui::EndTabItem();
    }

    // ----------------------------------------------------------------------
    // State sub-tab — former "Transitionable State" body (state combo,
    // prev/next buttons, source + clip labels, Browse/Clear Override, popup).
    // ----------------------------------------------------------------------
    if (ImGui::BeginTabItem("State")) {
        auto& tr = s.m_transitions;
        const bool trHasOverride = !s.m_transitionOverrideOt.empty();
        const bool trAvailable   = tr.IsAvailable();

        if (!trAvailable && !trHasOverride) {
            ImGui::TextDisabled("(no sibling .ot found)");
        } else if (!trAvailable && trHasOverride) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                               "Override did not yield Transitionable block");
        } else {
            const int n = tr.NumStates();
            const bool binary = tr.OtFile().transitionable && tr.OtFile().transitionable->isBinary;
            std::vector<std::string> labels; labels.reserve(static_cast<size_t>(n));
            const auto& clipNames = s.m_animPlayer.AnimNames();
            for (int i = 0; i < n; ++i) {
                std::string resolved = tr.ResolveClipForState(i, clipNames);
                std::string label = std::string("State ") + std::to_string(i);
                if (binary && i == 0)      label += " (untransitioned)";
                else if (binary && i == 1) label += " (transitioned)";
                if (!resolved.empty()) label += " (" + resolved + ")";
                labels.push_back(std::move(label));
            }
            int cur = tr.CurrentState();
            if (cur < 0) cur = 0;
            if (n > 0 && cur >= n) cur = n - 1;

            ImGui::PushItemWidth(-80.0f);
            const char* preview = labels.empty() ? "(no states)" : labels[static_cast<size_t>(cur)].c_str();
            if (ImGui::BeginCombo("##AocState", preview)) {
                for (int i = 0; i < n; ++i) {
                    const bool selected = (i == cur);
                    if (ImGui::Selectable(labels[static_cast<size_t>(i)].c_str(), selected)) {
                        tr.SetCurrentState(i);
                        s.m_transitionStateDirty = true;
                        s.m_persisted.Save(s.m_pluginDir,
                                           s.m_filePath,
                                           s.m_sceneGraph,
                                           s.m_transitions);
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PopItemWidth();
            ImGui::SameLine();
            if (ImGui::Button("<##AocPrev") && n > 0) {
                tr.SetCurrentState(((cur - 1) % n + n) % n);
                s.m_transitionStateDirty = true;
                s.m_persisted.Save(s.m_pluginDir,
                                   s.m_filePath,
                                   s.m_sceneGraph,
                                   s.m_transitions);
            }
            ImGui::SameLine();
            if (ImGui::Button(">##AocNext") && n > 0) {
                tr.SetCurrentState((cur + 1) % n);
                s.m_transitionStateDirty = true;
                s.m_persisted.Save(s.m_pluginDir,
                                   s.m_filePath,
                                   s.m_sceneGraph,
                                   s.m_transitions);
            }

            std::string srcLine = "Source: ";
            if (trHasOverride) {
                srcLine += "override: " + s.m_transitionOverrideOt;
            } else {
                if (!tr.DiscoveredOtcPath().empty())       srcLine += tr.DiscoveredOtcPath();
                else if (!tr.DiscoveredOtPath().empty())   srcLine += tr.DiscoveredOtPath();
                else                                        srcLine += "(none)";
            }
            ImGui::TextDisabled("%s", srcLine.c_str());

            std::string clip = labels.empty() ? std::string() :
                tr.ResolveClipForState(cur, clipNames);
            if (clip.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Clip:   (no clip resolvable)");
            } else {
                bool handlerExtracted = false;
                if (tr.IsAvailable()) {
                    for (const auto& h : tr.OtFile().transitionable->handlers) {
                        if (h.stateIndex == cur && !h.extractedClip.empty()) {
                            handlerExtracted = true;
                            break;
                        }
                    }
                }

                // Second check: is the resolved clip actually in the .amd
                // clip list? A handler may reference a clip name that the
                // current .amd lacks (drift between .ot and .amd) — in that
                // case the sidepanel shouldn't celebrate with green.
                bool clipInAmd = false;
                for (const auto& cn : clipNames) {
                    if (cn == clip) { clipInAmd = true; break; }
                }

                ImVec4 clipColor;
                if (handlerExtracted && clipInAmd) {
                    // green: handler extracted + clip exists in .amd
                    clipColor = ImVec4(0.5f, 1.0f, 0.5f, 1.0f);
                } else if (handlerExtracted && !clipInAmd) {
                    // orange: handler says X but .amd lacks X
                    clipColor = ImVec4(1.0f, 0.7f, 0.3f, 1.0f);
                } else {
                    // grey: index fallback (no handler extraction)
                    clipColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
                }
                ImGui::TextColored(clipColor, "Clip:   %s", clip.c_str());
            }
        }

        if (ImGui::Button("Browse .ot...##AocBrowseOt")) {
            ImGui::OpenPopup("AocBrowseOtPopup");
        }
        if (trHasOverride) {
            ImGui::SameLine();
            if (ImGui::Button("Clear Override##AocClearOt")) {
                s.m_transitionOverrideOt.clear();
                s.m_transitions.Populate(s.m_mgr, s.m_filePath, s.m_transitionOverrideOt);
                s.m_transitionStateDirty = true;
                s.m_persisted.Save(s.m_pluginDir,
                                   s.m_filePath,
                                   s.m_sceneGraph,
                                   s.m_transitions);
            }
        }

        if (ImGui::BeginPopupModal("AocBrowseOtPopup", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize)) {
            static char buf[512] = {0};
            ImGui::TextUnformatted("Enter GGPK path to .ot or .otc file:");
            ImGui::PushItemWidth(480.0f);
            ImGui::InputText("##AocBrowseOtPath", buf, sizeof(buf));
            ImGui::PopItemWidth();
            ImGui::TextDisabled("Example: metadata/chests/abyss/abyssdepthschestbase.otc");
            if (ImGui::Button("Apply##AocBrowseOtApply")) {
                std::string p = buf;
                auto lowEnd = [&](const char* suf) {
                    size_t n = std::strlen(suf);
                    if (p.size() < n) return false;
                    for (size_t i = 0; i < n; ++i) {
                        char a = static_cast<char>(std::tolower(static_cast<unsigned char>(p[p.size()-n+i])));
                        char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suf[i])));
                        if (a != b) return false;
                    }
                    return true;
                };
                if (!lowEnd(".ot") && !lowEnd(".otc")) {
                    ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "Path must end in .ot or .otc");
                } else {
                    s.m_transitionOverrideOt = p;
                    (void)s.m_transitions.Populate(s.m_mgr, s.m_filePath,
                                                    s.m_transitionOverrideOt);
                    s.m_transitionStateDirty = true;
                    s.m_persisted.Save(s.m_pluginDir,
                                       s.m_filePath,
                                       s.m_sceneGraph,
                                       s.m_transitions);
                    buf[0] = '\0';   // reset input so reopening starts clean
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##AocBrowseOtCancel")) {
                buf[0] = '\0';   // reset input so reopening starts clean
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::EndTabItem();
    }

    // ----------------------------------------------------------------------
    // Attach sub-tab — former "Attachments" body + new inline management
    // table. The table lists each currently-attached child with its socket,
    // source basename, skin-mode badge, transform-override popup button, and
    // a Detach button. Supplemental surface — primary attach flow still lives
    // in the Sockets tab.
    // ----------------------------------------------------------------------
    if (ImGui::BeginTabItem("Attach")) {
        ImGui::Checkbox("Show Attachments##AocShowAttach", &s.m_view.showAttachments);
        ImGui::TextDisabled("(%d children \xe2\x80\x94 see Sockets tab)",
                            s.m_sceneGraph.ChildCount());

        ImGui::SeparatorText("Current Attachments");
        if (ImGui::BeginTable("##PreviewAttachList", 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Socket", ImGuiTableColumnFlags_WidthStretch, 0.25f);
            ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 0.35f);
            ImGui::TableSetupColumn("Skin",   ImGuiTableColumnFlags_WidthStretch, 0.20f);
            ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 30.0f);
            ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableHeadersRow();

            auto& children = s.m_sceneGraph.MutableChildren();
            for (std::size_t idx = 0; idx < children.size(); ++idx) {
                auto& child = children[idx];
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(idx));

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(child.socketName.c_str());

                ImGui::TableSetColumnIndex(1);
                std::string base = child.sourcePath;
                const std::size_t slash = base.find_last_of("/\\");
                if (slash != std::string::npos) base = base.substr(slash + 1);
                ImGui::TextUnformatted(base.c_str());

                ImGui::TableSetColumnIndex(2);
                Aoc::Ui::RenderSkinModeBadge(child);

                ImGui::TableSetColumnIndex(3);
                if (ImGui::SmallButton("T")) {
                    ImGui::OpenPopup("##TOverride");
                }
                if (ImGui::IsItemHovered() && !ImGui::IsPopupOpen("##TOverride")) {
                    ImGui::SetTooltip("Transform override (save flushed on popup close)");
                }
                (void)Aoc::Ui::RenderTransformOverridePopup(child);

                // T8 / B1 debounce: mirror Sockets tab's tracker so edits made
                // from this supplemental table also flush exactly once on popup
                // close. Keyed by socket name -> shared with Sockets tab, so a
                // popup opened here and closed after switching to Sockets still
                // flushes correctly.
                {
                    const bool popupOpenNow = ImGui::IsPopupOpen("##TOverride");
                    const bool wasOpenThisRow = (s.m_transformPopupOpenRow == child.socketName);
                    if (popupOpenNow && !wasOpenThisRow) {
                        s.m_transformPopupOpenRow = child.socketName;
                    } else if (!popupOpenNow && wasOpenThisRow) {
                        if (child.pendingTransformSave) {
                            s.m_persisted.Save(s.m_pluginDir, s.m_filePath,
                                               s.m_sceneGraph, s.m_transitions);
                            child.pendingTransformSave = false;
                        }
                        s.m_transformPopupOpenRow.clear();
                    }
                }

                ImGui::TableSetColumnIndex(4);
                if (ImGui::SmallButton("Detach")) {
                    s.m_sceneGraph.DetachChild(child.socketName);
                    s.m_persisted.Save(s.m_pluginDir, s.m_filePath,
                                       s.m_sceneGraph, s.m_transitions);
                    Aoc::Ui::PushToast(s, "Detached from " + child.socketName,
                                       /*isError=*/false);
                    ImGui::PopID();
                    break;  // iterator invalidated
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // ----------------------------------------------------------------------
    // Camera sub-tab — merges former "Animation" and "Camera" CollapsingHeaders
    // (previously rendered inline in RenderPreviewTab below the sidepanel
    // call). SeparatorText preserves the visual grouping of the two sections.
    // ----------------------------------------------------------------------
    if (ImGui::BeginTabItem("Camera")) {
        ImGui::SeparatorText("Animation");
        {
            float speed = s.m_animPlayer.Speed();
            if (ImGui::SliderFloat("Speed", &speed, 0.0f, 3.0f, "%.2f")) {
                s.m_animPlayer.SetSpeed(speed);
                s.m_animPanelSettingsDirty = true;
            }
            const auto& names = s.m_animPlayer.AnimNames();
            ImGui::TextDisabled("Animations: %zu", names.size());
            if (!names.empty()) {
                const bool playing = s.m_animPlayer.IsPlaying();
                if (ImGui::SmallButton(playing ? "Pause" : "Play")) {
                    if (playing) s.m_animPlayer.Pause();
                    else         s.m_animPlayer.Play();
                }
            }
        }

        ImGui::SeparatorText("Camera");
        {
            if (ImGui::Button("Reset View", ImVec2(-FLT_MIN, 0))) {
                if (s.m_sceneGraph.HasMesh()) {
                    float mn[3], mx[3];
                    s.m_sceneGraph.ComputeMeshLocalBounds(mn, mx);
                    s.m_camera.FitToBounds(mn, mx);
                } else {
                    s.m_camera.Reset();
                }
            }
        }
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
}

// ---------- Phase 2 light helpers ---------------------------------------
//
// Resolve the current state-index for a LightDef. If the light system has an
// active state name that matches one of `ld.states`, use it; otherwise fall
// back to the parsed default.
int PickStateIndex(const GGPKExp::Parsers::LightDef& ld, const std::string& activeName) {
    if (!activeName.empty()) {
        for (std::size_t i = 0; i < ld.states.size(); ++i)
            if (ld.states[i].name == activeName) return static_cast<int>(i);
    }
    return ld.defaultStateIndex;
}

// Returns the world-space position of a named bone (translation from the
// column-major 4x4 `worldTransform`), or {0,0,0} if `boneName` is empty or
// not found. Operates on the already-posed skeleton so spot/point positions
// track animation frames.
std::array<float, 3> ResolveBonePos(
        const GGPKExp::Parsers::ResolvedSkeleton& skel,
        const std::string&                        boneName) {
    if (boneName.empty()) return { 0.f, 0.f, 0.f };
    for (const auto& b : skel.bones) {
        if (b.name == boneName) {
            // Column-major 4x4 — translation in column 3 (elements [12],[13],[14]).
            return { b.worldTransform[12], b.worldTransform[13], b.worldTransform[14] };
        }
    }
    return { 0.f, 0.f, 0.f };
}

// Packs a parsed `LightDef` + chosen `LightState` into a GPU `ShaderLight`.
// - posRadius.xyz = boneAttach + state.position, .w = state.radius
// - Spot:   dirCone.w = cos(penumbra) (>=0),  colorInner.w = cos(umbra)
// - Point:  dirCone.w = -1.0 (sentinel "no cone"), colorInner.w = 0
// - params = (penumbraDistance, penumbraRadius, dominantShadow?1:0, 0)
GGPKExp::ShaderLight MakeShaderLight(
        const GGPKExp::Parsers::LightDef&         ld,
        const GGPKExp::Parsers::LightState&       st,
        const GGPKExp::Parsers::ResolvedSkeleton& skel,
        bool                                       dominantShadow) {
    GGPKExp::ShaderLight out{};

    const auto bonePos = ResolveBonePos(skel, ld.boneName);
    out.posRadius = {
        bonePos[0] + st.position[0],
        bonePos[1] + st.position[1],
        bonePos[2] + st.position[2],
        st.radius
    };

    if (ld.type == GGPKExp::Parsers::LightType::Spot) {
        out.dirCone = {
            st.direction[0],
            st.direction[1],
            st.direction[2],
            std::cos(std::max(st.penumbraAngleRad, 1e-3f))
        };
        out.colorInner = {
            st.colour[0], st.colour[1], st.colour[2],
            std::cos(std::max(st.umbraAngleRad, 0.0f))
        };
    } else {
        out.dirCone    = { 0.f, 0.f, -1.f, -1.0f };  // w<0 signals point light (no cone)
        out.colorInner = { st.colour[0], st.colour[1], st.colour[2], 0.0f };
    }

    out.params = {
        st.penumbraDistance,
        st.penumbraRadius,
        dominantShadow ? 1.0f : 0.0f,
        0.0f
    };
    return out;
}

// ---------- Phase 2 light gizmos ----------------------------------------
//
// Both helpers append to an output vector of point pairs: each pair (2 entries)
// is one line segment. Caller is expected to project each point through the
// camera VP and emit ImGui AddLine() per segment — matching the Phase 1
// socket-gizmo drawing convention (no dedicated renderer helper is used).

// Wire sphere built from 3 orthogonal great-circle rings (XY / YZ / XZ).
void BuildWireSphere(std::vector<std::array<float, 3>>& out,
                     const std::array<float, 3>&        center,
                     float                              radius,
                     int                                segments = 24) {
    if (radius <= 1e-4f) return;
    const float step = 6.2831853f / static_cast<float>(segments);
    for (int ring = 0; ring < 3; ++ring) {
        for (int i = 0; i < segments; ++i) {
            const float a0 = step * static_cast<float>(i);
            const float a1 = step * static_cast<float>(i + 1);
            std::array<float, 3> p0 = center, p1 = center;
            if (ring == 0) {           // XY plane
                p0[0] += radius * std::cos(a0); p0[1] += radius * std::sin(a0);
                p1[0] += radius * std::cos(a1); p1[1] += radius * std::sin(a1);
            } else if (ring == 1) {    // YZ plane
                p0[1] += radius * std::cos(a0); p0[2] += radius * std::sin(a0);
                p1[1] += radius * std::cos(a1); p1[2] += radius * std::sin(a1);
            } else {                   // XZ plane
                p0[0] += radius * std::cos(a0); p0[2] += radius * std::sin(a0);
                p1[0] += radius * std::cos(a1); p1[2] += radius * std::sin(a1);
            }
            out.push_back(p0);
            out.push_back(p1);
        }
    }
}

// Wire cone: apex at `apex`, opening toward `dir` (unit vector), half-angle
// `halfRad`, slant length `length`. `ribs` radial lines from apex to base plus
// a polyline ring around the base rim.
void BuildWireCone(std::vector<std::array<float, 3>>& out,
                   const std::array<float, 3>&        apex,
                   const std::array<float, 3>&        dir,
                   float                              halfRad,
                   float                              length,
                   int                                ribs = 8) {
    if (length <= 1e-3f || halfRad <= 1e-3f) return;

    // Orthonormal basis (u, v, dir). Pick a reference axis not parallel to dir.
    const std::array<float, 3> up = (std::fabs(dir[1]) < 0.9f)
        ? std::array<float, 3>{0.f, 1.f, 0.f}
        : std::array<float, 3>{1.f, 0.f, 0.f};
    auto cross = [](const std::array<float, 3>& a,
                    const std::array<float, 3>& b) {
        return std::array<float, 3>{
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]
        };
    };
    auto norm = [](std::array<float, 3> v) {
        const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        return (n > 1e-6f)
            ? std::array<float, 3>{v[0] / n, v[1] / n, v[2] / n}
            : std::array<float, 3>{0.f, 0.f, 1.f};
    };
    const auto u = norm(cross(up, dir));
    const auto v = norm(cross(dir, u));
    const float r = length * std::tan(halfRad);

    const std::array<float, 3> center{
        apex[0] + dir[0] * length,
        apex[1] + dir[1] * length,
        apex[2] + dir[2] * length
    };
    const float step = 6.2831853f / static_cast<float>(ribs);
    for (int i = 0; i < ribs; ++i) {
        const float a0 = step * static_cast<float>(i);
        const float a1 = step * static_cast<float>(i + 1);
        auto pt = [&](float a) {
            const float ca = std::cos(a);
            const float sa = std::sin(a);
            return std::array<float, 3>{
                center[0] + r * (ca * u[0] + sa * v[0]),
                center[1] + r * (ca * u[1] + sa * v[1]),
                center[2] + r * (ca * u[2] + sa * v[2])
            };
        };
        const auto p0 = pt(a0);
        const auto p1 = pt(a1);
        out.push_back(p0); out.push_back(p1);   // base rim segment
        out.push_back(apex); out.push_back(p0); // rib from apex to rim
    }
}

// Phase 4 — hotkey capture. Called once per frame from inside the 3D viewport
// child window.
// Guards:
//   - Transitionable data available
//   - ImGui is not requesting keyboard input (TextEditor etc.)
// Bindings:
//   Ctrl+Alt+1..9  → SetCurrentState(0..8), clamped to NumStates-1
//   [              → prev (wraps)
//   ]              → next (wraps)
void HandleAocStateHotkeys(AocViewerState& s) {
    if (!s.m_transitions.IsAvailable()) return;
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || io.WantCaptureKeyboard) return;

    // Require viewport focus/hover so `[` / `]` pressed in some other ImGui
    // window (e.g. a different tab's text field with focus released) don't
    // advance AOC state. Caller runs this inside the 3D viewport child, so
    // IsWindowFocused/Hovered reflects that child.
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
        return;
    }

    const int n = s.m_transitions.NumStates();
    if (n <= 0) return;

    const bool ctrl = io.KeyCtrl;
    const bool alt  = io.KeyAlt;

    if (ctrl && alt) {
        for (int i = 0; i < 9; ++i) {
            ImGuiKey key = static_cast<ImGuiKey>(static_cast<int>(ImGuiKey_1) + i);
            if (ImGui::IsKeyPressed(key, /*repeat=*/false)) {
                int target = i;
                if (target > n - 1) target = n - 1;
                s.m_transitions.SetCurrentState(target);
                s.m_transitionStateDirty = true;
                s.m_persisted.Save(s.m_pluginDir,
                                   s.m_filePath,
                                   s.m_sceneGraph,
                                   s.m_transitions);
                break;
            }
        }
        return;
    }

    if (!ctrl && !alt) {
        int cur = s.m_transitions.CurrentState();
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, /*repeat=*/false)) {
            s.m_transitions.SetCurrentState(((cur - 1) % n + n) % n);
            s.m_transitionStateDirty = true;
            s.m_persisted.Save(s.m_pluginDir,
                               s.m_filePath,
                               s.m_sceneGraph,
                               s.m_transitions);
        } else if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, /*repeat=*/false)) {
            s.m_transitions.SetCurrentState((cur + 1) % n);
            s.m_transitionStateDirty = true;
            s.m_persisted.Save(s.m_pluginDir,
                               s.m_filePath,
                               s.m_sceneGraph,
                               s.m_transitions);
        }
    }
}

// Phase 4 — consume m_transitionStateDirty: resolve clip for CurrentState and
// push to AnimationPlayer if non-empty. AnimationPlayer has no
// SetClipByName — linearly search AnimNames() and use SelectAnimation(index).
void ApplyTransitionStateIfDirty(AocViewerState& s) {
    if (!s.m_transitionStateDirty) return;
    s.m_transitionStateDirty = false;
    if (!s.m_transitions.IsAvailable()) return;
    const auto& clipNames = s.m_animPlayer.AnimNames();
    const std::string clip = s.m_transitions.ResolveClipForState(
            s.m_transitions.CurrentState(), clipNames);
    if (clip.empty()) return;
    for (std::size_t i = 0; i < clipNames.size(); ++i) {
        if (clipNames[i] == clip) {
            s.m_animPlayer.SelectAnimation(i);
            break;
        }
    }
}

// Screen-space particle-emitter gizmo. Outer diamond + inner diamond (inset
// ~60 %). 8 edges total, 4 vertices each. Costs 8 AddLine calls.
void DrawEmitterDiamond(ImDrawList* dl, ImVec2 sp, float sizePx, ImU32 col) {
    const float thickness = 1.5f;
    const ImVec2 up   (sp.x,              sp.y - sizePx);
    const ImVec2 down (sp.x,              sp.y + sizePx);
    const ImVec2 left (sp.x - sizePx,     sp.y);
    const ImVec2 right(sp.x + sizePx,     sp.y);
    dl->AddLine(up,    right, col, thickness);
    dl->AddLine(right, down,  col, thickness);
    dl->AddLine(down,  left,  col, thickness);
    dl->AddLine(left,  up,    col, thickness);

    const float inner = sizePx * 0.60f;
    const ImVec2 up2   (sp.x,              sp.y - inner);
    const ImVec2 down2 (sp.x,              sp.y + inner);
    const ImVec2 left2 (sp.x - inner,      sp.y);
    const ImVec2 right2(sp.x + inner,      sp.y);
    dl->AddLine(up2,    right2, col, thickness);
    dl->AddLine(right2, down2,  col, thickness);
    dl->AddLine(down2,  left2,  col, thickness);
    dl->AddLine(left2,  up2,    col, thickness);
}

} // namespace

// -------------------------------------------------------------------------
// RenderPreviewTab — main entry point
// -------------------------------------------------------------------------

void AocViewer::RenderPreviewTab() {
    auto& s = m_state;

    // --- Lazy-init render subsystems (needs device from Initialize()) ----
    //
    // MeshRenderer.Initialize + MeshMaterial.Initialize mirror the pattern in
    // MeshViewer::Initialize. Fallback textures (1x1 white, flat normal) are
    // installed here so materials that fail to resolve still render correctly.
    if (!m_renderer && s.m_device) {
        m_renderer = std::make_unique<MeshRenderer>();
        m_renderer->Initialize(s.m_device);
    }
    if (!m_material && s.m_device) {
        m_material = std::make_unique<MeshMaterial>();
        m_material->Initialize(s.m_device);
        m_material->CreateFallbackTextures();
    }

    // --- Upload GPU resources for the currently-loaded scene mesh (once) --
    //
    // AocSceneGraph holds the parsed mesh + .sm material metadata. When a new
    // file is loaded, `m_uploadedMeshAddr` is stale so we re-create the VB/IB
    // and re-load materials. Idempotent: skipped when the same mesh pointer
    // is still uploaded.
    //
    // Phase 4 note: drawing child meshes via RenderAdditionalChildMesh
    // clobbers the shared VB/IB slots. After the child loop we set
    // `m_mainBuffersDirtyFromChildRender` so the next frame only re-uploads
    // geometry (LoadAllMaterials is skipped — textures are still valid since
    // the main mesh pointer hasn't changed). The flag lives on AocViewer so
    // it is scoped per-instance and cleared on Clear()/destruction.
    //
    // Failure handling: on upload failure we still update `m_uploadedMeshAddr`
    // so we don't retry every frame, but set `m_uploadFailed = true` so the
    // render pass below skips RenderScene. A fresh mesh pointer (e.g. after
    // RebuildScene) will trigger a new attempt.
    const void* currentMeshAddr = s.m_sceneGraph.HasMesh()
        ? static_cast<const void*>(&s.m_sceneGraph.Mesh())
        : nullptr;
    if (m_renderer && m_material && currentMeshAddr &&
        currentMeshAddr != m_uploadedMeshAddr) {
        std::string uploadErr;
        if (m_renderer->CreateMeshBuffers(s.m_sceneGraph.Mesh(), uploadErr)) {
            const auto& smData = s.m_sceneGraph.SmData();
            const size_t submeshCount = s.m_sceneGraph.Mesh().submeshRanges.empty()
                ? 1u
                : s.m_sceneGraph.Mesh().submeshRanges.size();
            m_material->LoadAllMaterials(smData, submeshCount, s.m_mgr);
            m_uploadedMeshAddr = currentMeshAddr;
            m_uploadFailed = false;
            m_mainBuffersDirtyFromChildRender = false;
        } else {
            // Push the warning once per unique failure (guards against an
            // infinite log spam if the same mesh pointer keeps failing).
            if (!uploadErr.empty()) {
                s.m_parseWarnings.push_back("MeshRenderer upload failed: " + uploadErr);
            }
            // Mark attempted so the above equality check short-circuits next
            // frame; only a new scene-graph mesh (RebuildScene / LoadData)
            // will clear m_uploadedMeshAddr and re-attempt.
            m_uploadedMeshAddr = currentMeshAddr;
            m_uploadFailed = true;
            m_mainBuffersDirtyFromChildRender = false;
        }
    } else if (m_renderer && m_material && currentMeshAddr &&
               m_mainBuffersDirtyFromChildRender && !m_uploadFailed) {
        // Same mesh pointer, but child render clobbered m_vb/m_ib last frame.
        // Just rebuild buffers — materials are still valid.
        std::string uploadErr;
        if (!m_renderer->CreateMeshBuffers(s.m_sceneGraph.Mesh(), uploadErr)) {
            // Rare: ran out of GPU memory between frames. Mark upload-failed
            // so overlay falls back to the "Mesh upload failed" stub.
            if (!uploadErr.empty()) {
                s.m_parseWarnings.push_back("Main-mesh rebuild after child render failed: " + uploadErr);
            }
            m_uploadFailed = true;
        }
        m_mainBuffersDirtyFromChildRender = false;
    } else if (!currentMeshAddr && m_uploadedMeshAddr) {
        // Scene cleared — release stale buffers so they don't render with a
        // freshly-loaded skeleton later.
        if (m_renderer) m_renderer->ClearMeshBuffers();
        m_uploadedMeshAddr = nullptr;
        m_uploadFailed = false;
        m_mainBuffersDirtyFromChildRender = false;
    }

    // --- Layout: viewport (left) + controls sidepanel (right) -------------
    const float kSidePanelW = 260.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float viewportW = std::max(100.0f, avail.x - kSidePanelW - 8.0f);

    // Viewport child --------------------------------------------------------
    ImGui::BeginChild("##AocViewport", ImVec2(viewportW, 0), false,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);
    {
        if (!s.m_sceneGraph.HasMesh()) {
            const std::string& err = s.m_sceneGraph.Error();
            if (err.empty()) {
                ImGui::TextDisabled("No renderable skin mesh.");
            } else {
                ImGui::TextDisabled("No renderable skin mesh: %s", err.c_str());
            }
        } else if (!m_renderer) {
            // Device not yet available (shouldn't happen after ContentPanel
            // wires Initialize(), but defensive: surface the state rather
            // than silently rendering nothing).
            ImGui::TextDisabled("Waiting for D3D11 device ...");
        } else {
            // --- Per-frame animation tick + ApplyFrame ------------------
            //
            // Mirrors MeshViewer's gating: we only write animated world
            // transforms into the resolved skeleton when the user has
            // actively pressed Play (or scrubbed off t=0). Static-pose V01
            // meshes left alone then render at their baked bind positions.
            //
            // Phase 4 — hotkey capture (Ctrl+Alt+1..9 / [ / ]) must run BEFORE
            // ApplyTransitionStateIfDirty so the dirty flag it sets gets
            // consumed this same frame; ApplyTransitionStateIfDirty runs BEFORE
            // Tick so the new clip is active for this frame's ApplyFrame.
            // HandleAocStateHotkeys guards on WantTextInput /
            // WantCaptureKeyboard itself.
            HandleAocStateHotkeys(s);
            ApplyTransitionStateIfDirty(s);

            const float animDt = ImGui::GetIO().DeltaTime;
            const float prevAnimT = s.m_animPlayer.CurrentTime();
            s.m_animPlayer.Tick(animDt);
            const float curAnimT  = s.m_animPlayer.CurrentTime();

            // Phase 2 audio-trigger hook: on each frame, fire any SoundEventClip whose
            // `time` crossed the half-open interval (prevAnimT, curAnimT]. Handle loops
            // and backward seeks by clearing the debounce cache.
            if (s.m_audio) {
                const Parsers::SoundEventsBlock* soundBlock = nullptr;
                for (const auto& b : s.m_parsedDoc.merged.blocks) {
                    if (b.soundEvents) { soundBlock = b.soundEvents.get(); break; }
                }

                if (soundBlock && !soundBlock->clips.empty()) {
                    if (curAnimT < prevAnimT) {
                        // Backward seek / loop wrap — reset debounce state.
                        s.m_audioLastTriggered.clear();
                    }
                    // On the very first frame prevAnimT is exactly 0.0; without
                    // this nudge, clips authored at time=0.0 would never fire
                    // (since `0.0 > 0.0` is false). The 50 ms debounce below
                    // prevents re-firing on later frames.
                    const bool firstFrame   = (prevAnimT <= 1e-4f);
                    const float effectivePrev = firstFrame ? -1e-3f : prevAnimT;
                    for (const auto& clip : soundBlock->clips) {
                        if (clip.time > effectivePrev && clip.time <= curAnimT) {
                            const auto it = s.m_audioLastTriggered.find(clip.filename);
                            const float last = (it != s.m_audioLastTriggered.end()) ? it->second : -1000.0f;
                            if (curAnimT - last >= 0.050f) {
                                s.m_audio->Trigger(clip);
                                s.m_audioLastTriggered[clip.filename] = curAnimT;
                            }
                        }
                    }
                }

                s.m_audio->MainThreadPump();
            }

            // Phase 3 particle-trigger hook: per-emitter, mark lastTriggeredTime
            // when the animation clock crosses the event time within the active
            // clip. Pure state update — rendering happens below.
            {
                auto& emitters = s.m_particles.MutableEmitters();
                if (!emitters.empty()) {
                    // Active clip name (used to filter emitters).
                    std::string activeClip;
                    {
                        const std::size_t sel = s.m_animPlayer.SelectedAnimIndex();
                        const auto& names = s.m_animPlayer.AnimNames();
                        if (sel != static_cast<std::size_t>(-1) && sel < names.size()) {
                            activeClip = names[sel];
                        }
                    }

                    // Detect clip change: if the cached name doesn't match the
                    // current one, wipe trigger state.
                    if (activeClip != s.m_particlesLastClip) {
                        s.m_particles.ResetTriggers();
                        s.m_particlesLastClip = activeClip;
                    }

                    // Detect backward seek / loop wrap same way the audio loop does.
                    if (curAnimT < prevAnimT) {
                        s.m_particles.ResetTriggers();
                    }

                    // First-frame nudge — mirror the audio loop. Events authored
                    // at time=0.0 need a tiny negative effectivePrev to fire on
                    // the very first Tick.
                    const bool firstFrame     = (prevAnimT <= 1e-4f);
                    const float effectivePrev = firstFrame ? -1e-3f : prevAnimT;

                    for (auto& e : emitters) {
                        if (e.animationName != activeClip) continue;
                        if (e.time > effectivePrev && e.time <= curAnimT) {
                            e.lastTriggeredTime = curAnimT;
                        }
                    }
                }
            }

            s.m_lastAnimTimeForAudio = curAnimT;
            // Use the AOC's authoritative skeleton (augmented with sockets via
            // AocSkeletonAugmenter during DoParsing). AnimationPlayer was
            // attached to &s.m_astSkel, which is what ResolvedSkeleton is built
            // from, so ApplyFrame's writes land in the same object the overlay
            // and palette both read.
            auto& resolvedSkel = s.m_resolvedSkel;
            const bool wantApplyFrame =
                s.m_animPlayer.SelectedAnimIndex() != static_cast<std::size_t>(-1) &&
                (s.m_animPlayer.IsAnyLayerPlaying() ||
                 s.m_animPlayer.BaseLayerTime() > 0.0f) &&
                !resolvedSkel.bones.empty();
            if (wantApplyFrame) {
                s.m_animPlayer.ApplyFrame(resolvedSkel);
            }

            // --- Canvas: InvisibleButton first so ProcessInput sees hover --
            ImVec2 canvasSize = ImGui::GetContentRegionAvail();
            canvasSize.x = std::max(64.0f, canvasSize.x);
            canvasSize.y = std::max(64.0f, canvasSize.y);
            ImVec2 canvasPos = ImGui::GetCursorScreenPos();

            ImGui::InvisibleButton("##AocMeshCanvas", canvasSize,
                                   ImGuiButtonFlags_MouseButtonLeft  |
                                   ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);

            // Camera input (orbit/pan/zoom) — checks IsItemHovered() internally.
            s.m_camera.ProcessInput(canvasPos, canvasSize);

            // --- Build skinning palette ---------------------------------
            //
            // s.m_aocBindInverseWorld caches the inverse of each bone's
            // bind-pose worldTransform (computed once in DoParsing before
            // any ApplyFrame ran). Building the palette every frame —
            // even when the animation is paused or no clip is selected —
            // keeps vertices in the bind-pose mesh space: with a static
            // pose the palette collapses to ~identity, so the mesh still
            // renders correctly. During animation, the palette becomes
            // `bindInverse × animated-worldTransform` and the mesh deforms.
            const auto& bindInv = s.m_aocBindInverseWorld;
            const float* palettePtr  = nullptr;
            std::size_t  paletteCount = 0;
            std::vector<std::array<float, 16>>& scratch = s.m_paletteScratch;
            if (!bindInv.empty() &&
                bindInv.size() == resolvedSkel.bones.size()) {
                bool truncated = false;
                Animation::BuildSkinningPalette(
                    resolvedSkel, bindInv,
                    /*maxBones*/ kMaxSkinBones,
                    scratch,
                    &truncated);

                // V01/V02 palette shuffle (mirrors MeshViewer.cpp:878-898).
                // After BuildSkinningPalette, scratch[gi] holds the matrix for
                // global bone gi. Shader expects scratch[local] to hold the
                // matrix the mesh's local-bone `local` should use. Remap
                // in-place by snapshotting the global-indexed result and
                // copying per-local-slot via s.m_boneRemap. For DOLm/V3+
                // meshes s.m_boneRemap is empty → shuffle block skipped and
                // the palette stays global-indexed (which matches DOLm).
                if (!s.m_boneRemap.empty() && !scratch.empty()) {
                    static constexpr std::array<float, 16> kIdentity = {
                        1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1
                    };
                    const auto original = scratch;  // snapshot global-indexed palette
                    const size_t localCount = s.m_boneRemap.size();
                    for (size_t i = 0; i < localCount && i < scratch.size(); ++i) {
                        const int gi = s.m_boneRemap[i];
                        if (gi >= 0 && static_cast<size_t>(gi) < original.size()) {
                            scratch[i] = original[gi];
                        } else {
                            scratch[i] = kIdentity;
                        }
                    }
                }

                if (!scratch.empty()) {
                    palettePtr   = scratch[0].data();
                    paletteCount = scratch.size();
                }
                (void)truncated;
            }

            // --- Render mesh to offscreen target ---------------------------
            //
            // Skip RenderScene entirely when the last upload failed — the
            // MeshRenderer has no valid buffers and draw calls would noop
            // or read stale GPU state. Show an explanatory stub instead.
            if (m_uploadFailed) {
                ImGui::SetCursorScreenPos(canvasPos);
                ImGui::TextDisabled("Mesh upload failed; see warnings.");
            } else {
                // --- Phase 2: upload per-frame multi-light block ------------
                //
                // Translates the AOC Lights block into GPU `ShaderLight`
                // entries. Each LightDef is resolved against the current
                // active-state name (falling back to its default), attached
                // to its named bone's world position, and tagged as the
                // dominant shadow caster for the one spot-light with
                // `castsShadow=true` (if any). The sun direction is always
                // refreshed to the Phase 1 value so toggling files doesn't
                // leave a stale direction from a previous upload.
                {
                    const auto& ls = s.m_lights;
                    GGPKExp::ShaderLight gpu[GGPKExp::kMaxShaderLights] = {};
                    int total = 0;

                    // `Show Lights` gates scene lighting entirely — when OFF,
                    // upload zero lights so the PS's count==0 branch falls
                    // back to the Phase-1 sun term. Gizmo overlay below is
                    // also gated by the same toggle.
                    if (s.m_view.showLights) {
                        const int shadowIdx = ls.DominantShadowCasterIndex();
                        total = static_cast<int>(std::min<std::size_t>(
                            ls.Block().lights.size(),
                            static_cast<std::size_t>(GGPKExp::kMaxShaderLights)));

                        for (int i = 0; i < total; ++i) {
                            const auto& ld = ls.Block().lights[static_cast<std::size_t>(i)];
                            const int   si = PickStateIndex(ld, ls.ActiveStateName());
                            const auto& st = ld.states[static_cast<std::size_t>(si)];
                            gpu[i] = MakeShaderLight(ld, st, s.m_resolvedSkel, i == shadowIdx);
                        }
                    }
                    m_renderer->UpdateLights(gpu, total);

                    // Phase 1 sun direction (matches MeshRenderer.cpp:624 default).
                    m_renderer->UpdateSunDir({ 0.4f, -0.6f, 0.5f });
                }

                const int cw = static_cast<int>(canvasSize.x);
                const int ch = static_cast<int>(canvasSize.y);
                const int renderMode = s.m_view.wireframe ? 1 : 0;
                m_renderer->RenderScene(cw, ch, s.m_camera, *m_material,
                                        s.m_sceneGraph.Mesh(), renderMode,
                                        /*hasSkinning*/ (palettePtr != nullptr),
                                        palettePtr, paletteCount);

                // Draw the offscreen SRV into the canvas rect.
                //
                // ImGui::Image captures the SRV pointer; the actual GPU sample
                // happens when ImGui flushes at frame end. Child-mesh render
                // passes below render INTO THE SAME RT, so by the time ImGui
                // finally samples the RT, it contains main + all children.
                if (m_renderer->GetRenderTargetSRV()) {
                    ImGui::SetCursorScreenPos(canvasPos);
                    ImGui::Image(
                        static_cast<ImTextureID>(
                            reinterpret_cast<uintptr_t>(m_renderer->GetRenderTargetSRV())),
                        canvasSize);
                }

                // --- Phase 4 — render attached child meshes at socket world
                // transforms. Rigid parenting: each child is drawn with
                // identity skinning palette, World = socket world transform
                // (from virtualBones[i].worldTransform, which already bakes
                // parent bone × local socket TRS via AocSkeletonAugmenter).
                //
                // MeshRenderer::RenderAdditionalChildMesh reuses m_vb/m_ib
                // buffer slots, so the next frame's main-mesh upload guard
                // needs to re-upload. We invalidate m_uploadedMeshAddr after
                // the loop so RenderScene() on the next frame rebuilds main
                // buffers before its own draw.
                if (s.m_view.showAttachments && s.m_sceneGraph.ChildCount() > 0) {
                    // Per-child material cache (stable across frames keyed by
                    // "sourcePath|socketName"). Each entry holds a
                    // MeshMaterial whose m_materialTextures list is loaded
                    // once and reused on every frame. Cleared in Clear()
                    // (cross-file load) and on destruction; memory footprint
                    // is bounded by the number of uniquely-attached child
                    // sources per file.
                    auto& childMaterials = m_childMaterials;

                    // Phase 4 M8 — use the explicit mutable accessor; Phase 3
                    // MutableEmitters() pattern — no more const_cast.
                    auto& children = s.m_sceneGraph.MutableChildren();
                    bool drewAnyChild = false;

                    for (auto& child : children) {
                        // 1. Find socket by NAME in the merged doc (name-keyed
                        //    so lookups survive re-parse / socket reorder).
                        const Parsers::Socket* matchedSocket = nullptr;
                        for (const auto& b : s.m_parsedDoc.merged.blocks) {
                            if (!b.clientAnim) continue;
                            for (const auto& sock : b.clientAnim->sockets) {
                                if (sock.name == child.socketName) {
                                    matchedSocket = &sock;
                                    break;
                                }
                            }
                            if (matchedSocket) break;
                        }
                        if (!matchedSocket) continue;  // socket removed from model

                        // 2. Resolve socket world transform. Prefer the
                        //    pre-composed `virtualBones[]` entry (it already
                        //    has TRS × parent applied by AocSkeletonAugmenter
                        //    + ApplyParentIgnoreFlags + UpdateVirtualBoneWorldTransforms,
                        //    which means animation + ignore_parent_* flags
                        //    are honoured for free). Fall back to raw parent
                        //    bone × socket translation if the augmenter
                        //    skipped this socket (orphan warning case).
                        const Parsers::ResolvedSkeleton& skel = s.m_resolvedSkel;
                        float socketWorld[16] = {};
                        bool  haveSocket = false;
                        for (const auto& vb : skel.virtualBones) {
                            if (vb.name == child.socketName) {
                                std::memcpy(socketWorld, vb.worldTransform,
                                            sizeof(float) * 16);
                                haveSocket = true;
                                break;
                            }
                        }
                        if (!haveSocket) {
                            // Fallback: translation-only composition against
                            // the resolved parent bone (treats rotation/scale
                            // as identity). Matches the Phase 4 MVP spec.
                            int parentIdx = -1;
                            for (std::size_t i = 0; i < skel.bones.size(); ++i) {
                                if (skel.bones[i].name == matchedSocket->parentBoneName) {
                                    parentIdx = static_cast<int>(i);
                                    break;
                                }
                            }
                            if (parentIdx < 0) continue;
                            const float* pW =
                                skel.bones[static_cast<std::size_t>(parentIdx)]
                                    .worldTransform;
                            std::memcpy(socketWorld, pW, sizeof(float) * 16);
                            const float* lT = matchedSocket->translation;
                            socketWorld[12] = pW[12] + pW[0]*lT[0] + pW[4]*lT[1] + pW[8] *lT[2];
                            socketWorld[13] = pW[13] + pW[1]*lT[0] + pW[5]*lT[1] + pW[9] *lT[2];
                            socketWorld[14] = pW[14] + pW[2]*lT[0] + pW[6]*lT[1] + pW[10]*lT[2];
                        }

                        // Phase 5.2.1 — apply per-attachment TRS override.
                        // overrideMat × socketWorld → effectiveSocketWorld.
                        // Fast path: when identity, pointer-alias to socketWorld
                        // and skip the matmul. Value-equal but saves 64 muls.
                        float effectiveSocketWorldStorage[16];
                        const float* effectiveSocketWorld = socketWorld;
                        if (!Aoc::IsTransformIdentity(child)) {
                            float overrideMat[16];
                            Aoc::ComposeOverrideMatrix(child, overrideMat);
                            Animation::MatrixMath::Multiply4x4RowMajor(
                                overrideMat,
                                socketWorld,
                                effectiveSocketWorldStorage);
                            effectiveSocketWorld = effectiveSocketWorldStorage;
                        }

                        // 3. Resolve / lazy-create per-child material cache entry.
                        const std::string cacheKey =
                            child.sourcePath + "|" + child.socketName;
                        auto it = childMaterials.find(cacheKey);
                        if (it == childMaterials.end()) {
                            auto mm = std::make_unique<MeshMaterial>();
                            mm->Initialize(s.m_device);
                            mm->CreateFallbackTextures();
                            const size_t submeshCount =
                                child.mesh.submeshRanges.empty()
                                    ? 1u
                                    : child.mesh.submeshRanges.size();
                            mm->LoadAllMaterials(child.smData, submeshCount, s.m_mgr);
                            it = childMaterials.emplace(cacheKey, std::move(mm)).first;
                        }

                        // 4. Phase 5.2 — branch on per-child skin mode.
                        //    Skinned: build palette from parent's current pose
                        //    via child.boneRemap; unmatched slots fall back to
                        //    the socket world matrix (acting as a virtual root
                        //    for decoration bones that exist only on the child).
                        //    Rigid: Phase 4 path — socket world as childWorld.
                        bool drewThisChild = false;

                        if (child.skinMode == Aoc::ChildSkinMode::Skinned &&
                            !child.boneRemap.empty() &&
                            !child.childBindInverse.empty() &&
                            child.boneRemap.size() <= child.childBindInverse.size()) {

                            auto& palette = s.m_childPaletteScratch;
                            palette.resize(child.boneRemap.size());

                            const auto& parentBones = s.m_resolvedSkel.bones;
                            for (std::size_t i = 0; i < child.boneRemap.size(); ++i) {
                                const int pIdx = child.boneRemap[i];
                                const float* srcWorld =
                                    (pIdx >= 0 &&
                                     static_cast<std::size_t>(pIdx) < parentBones.size())
                                        ? parentBones[static_cast<std::size_t>(pIdx)].worldTransform
                                        : effectiveSocketWorld;
                                // palette[i] = bindInverse[i] * srcWorld
                                // Same row-vector convention as SkinningPalette.cpp:27.
                                Animation::MatrixMath::Multiply4x4RowMajor(
                                    child.childBindInverse[i].data(),
                                    srcWorld,
                                    palette[i].data());
                            }

                            // V01/V02 child meshes: shuffle palette from child-skel-indexed
                            // to mesh-local-indexed, matching vertex BLENDINDICES semantics.
                            // DOLm/V3+ meshes have empty boneNames and skip this.
                            // Mirrors the parent-mesh pattern at AocViewer_3DPreview.cpp:894-908.
                            if (!child.mesh.boneNames.empty() &&
                                !child.childLocalToChildSkel.empty()) {
                                static constexpr std::array<float, 16> kIdentity = {
                                    1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1
                                };
                                const auto original = palette;  // snapshot child-skel-indexed
                                const std::size_t localCount = child.childLocalToChildSkel.size();
                                palette.resize(localCount);
                                for (std::size_t i = 0; i < localCount; ++i) {
                                    const int gi = child.childLocalToChildSkel[i];
                                    if (gi >= 0 && static_cast<std::size_t>(gi) < original.size()) {
                                        palette[i] = original[gi];
                                    } else {
                                        palette[i] = kIdentity;
                                    }
                                }
                            }

                            drewThisChild = m_renderer->RenderAdditionalChildMesh(
                                s.m_camera,
                                *it->second,
                                child.mesh,
                                effectiveSocketWorld,  // still passed for symmetry; unused for MVP in skinned path
                                renderMode,
                                &palette[0][0],
                                palette.size());
                        } else {
                            // Rigid path — Phase 5.2.1:
                            //  1. Apply per-attachment override (already baked into
                            //     effectiveSocketWorld above).
                            //  2. For children with their own .ast skeleton, also
                            //     pre-apply childBindInverse[0] so vertices authored
                            //     in child-local space anchor at the intended grip
                            //     rather than at (root_bone → grip) distance. Fixes
                            //     the weapon bind-pose bug for Rigid-fallback renders.
                            float rigidChildWorld[16];
                            if (!child.childBindInverse.empty()) {
                                const std::size_t rootIdx = static_cast<std::size_t>(
                                    child.childRootBoneIndex >= 0 ? child.childRootBoneIndex : 0);
                                Animation::MatrixMath::Multiply4x4RowMajor(
                                    child.childBindInverse[rootIdx].data(),
                                    effectiveSocketWorld,
                                    rigidChildWorld);
                            } else {
                                std::memcpy(rigidChildWorld,
                                            effectiveSocketWorld,
                                            sizeof(rigidChildWorld));
                            }

                            drewThisChild = m_renderer->RenderAdditionalChildMesh(
                                s.m_camera,
                                *it->second,
                                child.mesh,
                                rigidChildWorld,
                                renderMode);
                        }

                        if (drewThisChild) {
                            drewAnyChild = true;
                            // Track per-child upload so the scene graph knows
                            // which pointer MeshRenderer last saw. Matches the
                            // Phase 4 cache pattern in AocChildMesh.
                            child.uploadedMeshAddr = &child.mesh;
                        }
                    }

                    // After the loop, m_vb/m_ib hold the LAST child's buffers.
                    // Flag the main-mesh buffers as dirty so the next frame's
                    // upload block rebuilds them BEFORE RenderScene. We avoid
                    // nulling m_uploadedMeshAddr here — that would also trigger
                    // LoadAllMaterials next frame, which re-reads textures from
                    // GGPK. Buffers-only rebuild keeps textures cached.
                    if (drewAnyChild) {
                        m_mainBuffersDirtyFromChildRender = true;
                    }
                }
            }

            // --- Overlay pass (grid / bones / sockets / parent lines) ------
            //
            // This is the Phase-1-local fallback for the overlays the plan's
            // original MeshRenderer::Scene envisioned. Keeping them in
            // ImDrawList is cheap and keeps MeshRenderer untouched. Task 13
            // in the plan covers moving these into MeshRenderer proper.
            if (canvasSize.x > 1 && canvasSize.y > 1) {
                float vp[16];
                const float aspect = canvasSize.x / canvasSize.y;
                s.m_camera.BuildVPMatrix(vp, aspect);

                ImDrawList* dl = ImGui::GetWindowDrawList();

                // Grid ---------------------------------------------------
                //
                // POE2 meshes are Z-down: the floor plane is X-Y at constant
                // Z. This matches MeshViewer's pattern (see MeshViewer.cpp
                // lines 961-1002): gridZ is bboxMax[2] (max Z = floor in
                // Z-down), grid lines vary X and Y at constant Z, and the
                // Z axis of the gizmo points toward -Z (world-up).
                if (s.m_view.showGrid) {
                    float mn[3], mx[3];
                    s.m_sceneGraph.ComputeMeshLocalBounds(mn, mx);
                    const float dx = mx[0] - mn[0];
                    const float dy = mx[1] - mn[1];
                    const float maxSpan = std::max(dx, dy);
                    const float step = ChooseGridStep(maxSpan);
                    const float extent = step * 10.0f;
                    // ComputeGridZ() returns the Z floor height for the
                    // current gridAnchor mode (mesh bbox max Z, skeleton
                    // bbox max Z, or world origin 0).
                    const float gridZ = ComputeGridZ();
                    const ImU32 gridCol = IM_COL32(255, 255, 255, 35);

                    for (float x = -extent; x <= extent + 0.01f; x += step) {
                        ImVec2 a, b;
                        if (ProjectWorldToScreen(vp, canvasPos, canvasSize,
                                                  x, -extent, gridZ, a) &&
                            ProjectWorldToScreen(vp, canvasPos, canvasSize,
                                                  x,  extent, gridZ, b)) {
                            dl->AddLine(a, b, gridCol, 1.0f);
                        }
                    }
                    for (float y = -extent; y <= extent + 0.01f; y += step) {
                        ImVec2 a, b;
                        if (ProjectWorldToScreen(vp, canvasPos, canvasSize,
                                                  -extent, y, gridZ, a) &&
                            ProjectWorldToScreen(vp, canvasPos, canvasSize,
                                                   extent, y, gridZ, b)) {
                            dl->AddLine(a, b, gridCol, 1.0f);
                        }
                    }

                    // World axes through the grid origin. Z-down means
                    // world-up is -Z, so the Z axis points to gridZ - axLen.
                    const float axLen = extent * 0.6f;
                    ImVec2 origin, xEnd, yEnd, zEnd;
                    if (ProjectWorldToScreen(vp, canvasPos, canvasSize,
                                             0, 0, gridZ, origin)) {
                        if (ProjectWorldToScreen(vp, canvasPos, canvasSize,
                                                 axLen, 0, gridZ, xEnd)) {
                            dl->AddLine(origin, xEnd,
                                        IM_COL32(220, 60, 60, 200), 2.0f);
                        }
                        if (ProjectWorldToScreen(vp, canvasPos, canvasSize,
                                                 0, axLen, gridZ, yEnd)) {
                            dl->AddLine(origin, yEnd,
                                        IM_COL32(60, 200, 60, 200), 2.0f);
                        }
                        if (ProjectWorldToScreen(vp, canvasPos, canvasSize,
                                                 0, 0, gridZ - axLen, zEnd)) {
                            dl->AddLine(origin, zEnd,
                                        IM_COL32(60, 100, 220, 200), 2.0f);
                        }
                    }
                }

                // Skeleton + parent-bone lines ----------------------------
                //
                // Draws a point per resolved bone and a line from child to
                // parent. Matches MeshViewer::showBones visual, keyed off
                // `showParentLines` as a convenient single toggle for the
                // bone overlay in Phase 1.
                if (s.m_view.showParentLines &&
                    !resolvedSkel.bones.empty()) {
                    const ImU32 boneCol  = IM_COL32(0, 255, 200, 220);
                    const ImU32 jointCol = IM_COL32(255, 255, 0, 255);
                    for (const auto& bone : resolvedSkel.bones) {
                        const float bx = bone.worldTransform[12];
                        const float by = bone.worldTransform[13];
                        const float bz = bone.worldTransform[14];
                        if (!std::isfinite(bx) || !std::isfinite(by) ||
                            !std::isfinite(bz)) continue;
                        ImVec2 bp;
                        if (!ProjectWorldToScreen(vp, canvasPos, canvasSize,
                                                  bx, by, bz, bp)) continue;

                        if (bone.arrayParent < resolvedSkel.bones.size()) {
                            const auto& parent = resolvedSkel.bones[bone.arrayParent];
                            ImVec2 pp;
                            if (ProjectWorldToScreen(vp, canvasPos, canvasSize,
                                                     parent.worldTransform[12],
                                                     parent.worldTransform[13],
                                                     parent.worldTransform[14],
                                                     pp)) {
                                dl->AddLine(pp, bp, boneCol, 2.0f);
                            }
                        }
                        dl->AddCircleFilled(bp, 3.0f, jointCol);
                    }
                }

                // Sockets (virtual bones) ---------------------------------
                if (s.m_view.showSockets &&
                    !resolvedSkel.virtualBones.empty()) {
                    const ImU32 colSocket = IM_COL32(0, 220, 220, 255);
                    const ImU32 colLine   = IM_COL32(0, 220, 220, 160);
                    for (const auto& vb : resolvedSkel.virtualBones) {
                        const float wx = vb.worldTransform[12];
                        const float wy = vb.worldTransform[13];
                        const float wz = vb.worldTransform[14];
                        if (!std::isfinite(wx) || !std::isfinite(wy) ||
                            !std::isfinite(wz)) continue;
                        ImVec2 sp;
                        if (!ProjectWorldToScreen(vp, canvasPos, canvasSize,
                                                  wx, wy, wz, sp)) continue;

                        // Diamond marker.
                        const float r = 4.0f;
                        dl->AddLine(ImVec2(sp.x - r, sp.y),
                                    ImVec2(sp.x,     sp.y - r), colSocket, 1.5f);
                        dl->AddLine(ImVec2(sp.x,     sp.y - r),
                                    ImVec2(sp.x + r, sp.y),     colSocket, 1.5f);
                        dl->AddLine(ImVec2(sp.x + r, sp.y),
                                    ImVec2(sp.x,     sp.y + r), colSocket, 1.5f);
                        dl->AddLine(ImVec2(sp.x,     sp.y + r),
                                    ImVec2(sp.x - r, sp.y),     colSocket, 1.5f);

                        if (s.m_view.showSocketLabels) {
                            dl->AddText(ImVec2(sp.x + 6, sp.y - 6),
                                        colSocket, vb.name.c_str());
                        }

                        if (s.m_view.showParentLines &&
                            vb.parentBoneIndex < resolvedSkel.bones.size()) {
                            const auto& pb = resolvedSkel.bones[vb.parentBoneIndex];
                            ImVec2 pp;
                            if (ProjectWorldToScreen(vp, canvasPos, canvasSize,
                                                     pb.worldTransform[12],
                                                     pb.worldTransform[13],
                                                     pb.worldTransform[14],
                                                     pp)) {
                                dl->AddLine(pp, sp, colLine, 1.0f);
                            }
                        }
                    }
                }

                // Light gizmos (Phase 2, Task 11) ------------------------
                //
                // Per-light wire overlays drawn after scene render. Each light
                // generates its own segment batch so we can colour lines with
                // the light's own state.colour (floored to 0.3 per channel so
                // dim/off lights still render). Label draw mirrors the socket
                // label convention (offset (+6,-6), world-space projection via
                // ProjectWorldToScreen, one call per light).
                if (s.m_view.showLights && s.m_lights.LightCount() > 0) {
                    std::vector<std::array<float, 3>> segs;

                    for (const auto& ld : s.m_lights.Block().lights) {
                        if (ld.states.empty()) continue;

                        const int si = PickStateIndex(ld, s.m_lights.ActiveStateName());
                        if (si < 0 ||
                            static_cast<std::size_t>(si) >= ld.states.size()) continue;
                        const auto& st = ld.states[static_cast<std::size_t>(si)];

                        const auto bonePos = ResolveBonePos(resolvedSkel, ld.boneName);
                        const std::array<float, 3> pos{
                            bonePos[0] + st.position[0],
                            bonePos[1] + st.position[1],
                            bonePos[2] + st.position[2]
                        };

                        segs.clear();
                        if (ld.type == GGPKExp::Parsers::LightType::Point) {
                            BuildWireSphere(segs, pos, st.radius * 0.08f, 12);  // centre marker
                            BuildWireSphere(segs, pos, st.radius, 16);          // outer radius
                        } else if (ld.type == GGPKExp::Parsers::LightType::Spot) {
                            BuildWireCone(segs, pos, st.direction,
                                          st.penumbraAngleRad, st.radius);
                            BuildWireCone(segs, pos, st.direction,
                                          std::max(st.umbraAngleRad, 0.0f),
                                          st.radius * 0.98f);
                        } else {
                            // Directional lights are parsed but not drawn.
                            continue;
                        }

                        // Colour: max(channel, 0.3) floor so dim lights remain
                        // visible. Packed as IM_COL32 RGBA with fixed alpha.
                        const auto clampCh = [](float c) {
                            const float v = std::max(c, 0.3f);
                            return static_cast<int>(std::min(1.0f, v) * 255.0f + 0.5f);
                        };
                        const ImU32 lineCol = IM_COL32(
                            clampCh(st.colour[0]),
                            clampCh(st.colour[1]),
                            clampCh(st.colour[2]),
                            200);

                        // Emit line segments (pair-by-pair).
                        for (std::size_t i = 0; i + 1 < segs.size(); i += 2) {
                            const auto& p0 = segs[i];
                            const auto& p1 = segs[i + 1];
                            if (!std::isfinite(p0[0]) || !std::isfinite(p0[1]) ||
                                !std::isfinite(p0[2]) ||
                                !std::isfinite(p1[0]) || !std::isfinite(p1[1]) ||
                                !std::isfinite(p1[2])) continue;
                            ImVec2 a, b;
                            if (ProjectWorldToScreen(vp, canvasPos, canvasSize,
                                                     p0[0], p0[1], p0[2], a) &&
                                ProjectWorldToScreen(vp, canvasPos, canvasSize,
                                                     p1[0], p1[1], p1[2], b)) {
                                dl->AddLine(a, b, lineCol, 1.0f);
                            }
                        }

                        // Label at the light's anchor point. Uses the same
                        // (+6,-6) offset + brighter alpha as socket labels, but
                        // with the light's colour so multiple lights stay
                        // distinguishable.
                        if (s.m_view.showLightLabels && !ld.name.empty()) {
                            ImVec2 lp;
                            if (ProjectWorldToScreen(vp, canvasPos, canvasSize,
                                                     pos[0], pos[1], pos[2], lp)) {
                                const ImU32 labelCol = IM_COL32(
                                    clampCh(st.colour[0]),
                                    clampCh(st.colour[1]),
                                    clampCh(st.colour[2]),
                                    235);
                                dl->AddText(ImVec2(lp.x + 6, lp.y - 12),
                                            labelCol, ld.name.c_str());
                            }
                        }
                    }
                }

                // Phase 3 particle gizmos — draw ALL resolved emitters so the user
                // can always see where particles would spawn. Pulse brightening
                // only fires when the emitter's animationName matches the
                // currently active clip AND the trigger loop has touched it.
                // .pet-missing emitters get a red outline regardless.
                if (s.m_view.showParticles && !s.m_particles.Emitters().empty()) {
                    std::string activeClip;
                    {
                        const std::size_t sel = s.m_animPlayer.SelectedAnimIndex();
                        const auto& names = s.m_animPlayer.AnimNames();
                        if (sel != static_cast<std::size_t>(-1) && sel < names.size()) {
                            activeClip = names[sel];
                        }
                    }

                    for (const auto& e : s.m_particles.Emitters()) {
                        if (e.resolvedBoneIndex < 0) continue;

                        // Bone world-space position from the animated skeleton.
                        // Unified index: [0..bones-1] = real bones;
                        // [bones..bones+virtual-1] = virtual bones (augmented
                        // sockets). worldTransform is float[16], row-vector;
                        // translation at indices 12/13/14.
                        const std::size_t nBones = s.m_resolvedSkel.bones.size();
                        const std::size_t rbi    = static_cast<std::size_t>(e.resolvedBoneIndex);
                        const float* wm =
                            (rbi < nBones)
                                ? s.m_resolvedSkel.bones[rbi].worldTransform
                                : s.m_resolvedSkel.virtualBones[rbi - nBones].worldTransform;
                        const float bx = wm[12];
                        const float by = wm[13];
                        const float bz = wm[14];

                        ImVec2 sp;
                        if (!ProjectWorldToScreen(vp, canvasPos, canvasSize, bx, by, bz, sp)) continue;

                        // Pulse only applies when this emitter belongs to the
                        // currently-playing clip (so a clip mismatch just shows
                        // the static gizmo without ever flashing).
                        const bool clipMatch = (e.animationName == activeClip);
                        const float age    = clipMatch ? (curAnimT - e.lastTriggeredTime) : 1.0f;
                        const bool  active = clipMatch && (age >= 0.0f && age < 0.5f);
                        const float pulse  = active ? (1.0f - age / 0.5f) : 0.0f;

                        const ImU32 col =
                            e.petExists
                                ? (active ? IM_COL32(255, 240, 120, 255)
                                          : IM_COL32(120, 200, 255, 220))
                                :             IM_COL32(255,  80,  80, 220);

                        const float sizePx = 10.0f + pulse * 8.0f;
                        DrawEmitterDiamond(dl, sp, sizePx, col);

                        if (s.m_view.showParticleLabels) {
                            std::string lbl = e.filenameTail;
                            if (!e.petExists) lbl += "  [missing]";
                            dl->AddText(ImVec2(sp.x + sizePx + 4.0f, sp.y - 6.0f), col, lbl.c_str());
                        }
                    }
                }

                // UI toast overlay (2-second fade, bottom-left of the 3D canvas
                // rect so it never clips outside the viewport). Renders
                // attach/detach results, browse errors, missing audio, etc.
                {
                    const float now = static_cast<float>(ImGui::GetTime());
                    auto& toasts = s.m_uiToasts;
                    toasts.erase(std::remove_if(toasts.begin(), toasts.end(),
                        [&](const AocViewerState::UiToast& t) {
                            return (now - t.startTime) > 2.0f;
                        }), toasts.end());

                    if (!toasts.empty()) {
                        float y = canvasPos.y + canvasSize.y - 20.0f;
                        const float x = canvasPos.x + 12.0f;
                        for (auto it = toasts.rbegin(); it != toasts.rend(); ++it) {
                            const float age   = now - it->startTime;
                            const float alpha = (age > 0.0f && age < 2.0f)
                                ? (1.0f - age / 2.0f) : 0.0f;
                            if (alpha <= 0.01f) continue;
                            // Phase 4 M11: red for errors (missing audio, attach
                            // failure), green for successes (attach/detach OK).
                            const int a = static_cast<int>(alpha * 255.0f);
                            const ImU32 col = it->isError
                                ? IM_COL32(255, 96, 96, a)
                                : IM_COL32(96, 255, 96, a);
                            dl->AddText(ImVec2(x, y), col, it->text.c_str());
                            y -= 18.0f;
                        }
                    }
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right sidepanel -------------------------------------------------------
    // T10: sidepanel is now fully tabbed inside DrawViewTogglesPanel —
    // tabs: View / State / Attach / Camera (Animation merged into Camera).
    ImGui::BeginChild("##AocSidePanel", ImVec2(kSidePanelW, 0), true);
    {
        DrawViewTogglesPanel(s);
    }
    ImGui::EndChild();
}

} // namespace GGPKExp
