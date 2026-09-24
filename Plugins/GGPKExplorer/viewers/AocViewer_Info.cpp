#include "AocViewer.h"
#include "aoc/AocUiHelpers.h"

#include <imgui.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <unordered_set>

namespace GGPKExp {

void AocViewer::RenderOverviewTab() {
    const auto& s = m_state;

    if (ImGui::BeginTabBar("##OverviewSub", ImGuiTabBarFlags_None)) {
        // ==================================================================
        // Meta sub-tab: File / Extends Chain / References / Features /
        //               Warnings / Errors
        // ==================================================================
        if (ImGui::BeginTabItem("Meta")) {
            // --- File --------------------------------------------------------------
            if (ImGui::CollapsingHeader("File", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Path: %s", s.m_filePath.c_str());
                if (s.m_persisted.JsonExistsOnDisk() && s.m_persisted.IsLoaded()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("[persisted]");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Persisted: %d attachments%s",
                                          s.m_persisted.AttachmentCount(),
                                          s.m_persisted.HasTransitionState() ? " + state" : "");
                    }
                }
                ImGui::Text("Size: %zu bytes", s.m_rawBytes.size());
                if (!s.m_sha256Hex.empty()) {
                    ImGui::Text("SHA256: %s", s.m_sha256Hex.c_str());
                }
                ImGui::Text("AOC version: %d", s.m_parsedDoc.merged.version);
            }

            // --- Extends Chain -----------------------------------------------------
            if (ImGui::CollapsingHeader("Extends Chain", ImGuiTreeNodeFlags_DefaultOpen)) {
                const auto& ext = s.m_parsedDoc.merged.extendsPath;
                if (ext.empty()) {
                    ImGui::TextDisabled("(no extends)");
                } else {
                    ImGui::Text("Extends: %s", ext.c_str());
                }

                // inheritanceChain is populated by AocResolver -- index 0 = leaf,
                // last element = oldest ancestor in the chain.
                if (s.m_parsedDoc.inheritanceChain.empty()) {
                    ImGui::TextDisabled("(chain unavailable)");
                } else {
                    ImGui::Separator();
                    ImGui::TextDisabled("Inheritance chain (leaf -> root):");
                    for (std::size_t i = 0; i < s.m_parsedDoc.inheritanceChain.size(); ++i) {
                        ImGui::BulletText("[%zu] %s", i,
                                          s.m_parsedDoc.inheritanceChain[i].c_str());
                    }
                }
            }

            // --- References --------------------------------------------------------
            if (ImGui::CollapsingHeader("References", ImGuiTreeNodeFlags_DefaultOpen)) {
                const auto& refs = s.m_sceneGraph.ResolvedRefs();
                if (refs.empty()) {
                    ImGui::TextDisabled("(no references)");
                } else {
                    for (const auto& ref : refs) {
                        // Entries are "path|OK", "path|MISSING", or
                        // "path|OPTIONAL_MISSING[ (reason)]" -- colour-code based
                        // on which suffix is present.
                        const bool isOk             = ref.find("|OK")               != std::string::npos;
                        const bool isOptionalMiss   = ref.find("|OPTIONAL_MISSING") != std::string::npos;
                        const bool isMissing        = !isOk && !isOptionalMiss
                                                      && ref.find("|MISSING") != std::string::npos;

                        const char* tag = "[?]";
                        ImVec4 color = Aoc::Ui::kWarningColor;
                        if (isOk) {
                            tag = "[OK]";
                            color = Aoc::Ui::kOkColor;
                        } else if (isOptionalMiss) {
                            tag = "[OPTIONAL]";
                            color = Aoc::Ui::kOptionalColor;
                        } else if (isMissing) {
                            tag = "[MISSING]";
                            color = Aoc::Ui::kMissingColor;
                        }
                        ImGui::TextColored(color, "%s %s", tag, ref.c_str());
                    }
                }
            }

            // --- Features Detected -------------------------------------------------
            if (ImGui::CollapsingHeader("Features Detected", ImGuiTreeNodeFlags_DefaultOpen)) {
                // Aggregate socket / bone-group counts across all
                // ClientAnimationController blocks in the merged document. Usually
                // there's only one, but iterating keeps us robust.
                std::size_t sockets    = 0;
                std::size_t boneGroups = 0;
                bool hasClientAnim     = false;
                for (const auto& b : s.m_parsedDoc.merged.blocks) {
                    if (b.clientAnim) {
                        hasClientAnim = true;
                        sockets    += b.clientAnim->sockets.size();
                        boneGroups += b.clientAnim->boneGroups.size();
                    }
                }

                ImGui::Text("Sockets:     %zu", sockets);
                ImGui::Text("Bone Groups: %zu", boneGroups);
                ImGui::Text("Blocks:      %zu", s.m_parsedDoc.merged.blocks.size());
                if (!hasClientAnim) {
                    ImGui::TextDisabled("  (no ClientAnimationController block)");
                }
                if (s.m_sceneGraph.HasMesh()) {
                    ImGui::Text("Mesh Verts:  %zu", s.m_sceneGraph.Mesh().vertices.size());
                    ImGui::Text("Submeshes:   %zu", s.m_sceneGraph.SmData().materialPaths.size());
                } else {
                    ImGui::TextDisabled("Mesh:        (not loaded)");
                }
                ImGui::Text("Bones:       %zu", s.m_resolvedSkel.bones.size());
            }

            // --- Warnings ----------------------------------------------------------
            if (!s.m_parseWarnings.empty()) {
                if (ImGui::CollapsingHeader("Warnings", ImGuiTreeNodeFlags_DefaultOpen)) {
                    for (const auto& w : s.m_parseWarnings) {
                        ImGui::TextColored(Aoc::Ui::kWarningColor, "%s", w.c_str());
                    }
                }
            }

            // --- Errors ------------------------------------------------------------
            if (!s.m_parseErrors.empty()) {
                if (ImGui::CollapsingHeader("Errors", ImGuiTreeNodeFlags_DefaultOpen)) {
                    for (const auto& e : s.m_parseErrors) {
                        ImGui::TextColored(Aoc::Ui::kMissingColor, "%s", e.c_str());
                    }
                }
            }

            ImGui::EndTabItem();
        }

        // ==================================================================
        // Composition sub-tab: Lights / Sound Events / Animation Events /
        //                      Particles / Transitionable
        // ==================================================================
        if (ImGui::BeginTabItem("Composition")) {
            // -------------------------------------------------------------------------
            // Phase 2 sections — Lights / Sound Events / Animation Events
            // -------------------------------------------------------------------------
            // Locate the typed block pointers from the merged AOC document. Each field
            // is a unique_ptr that's non-null only when the corresponding block was
            // present somewhere in the extends chain (resolver merges these).
            const Parsers::LightsBlock*      lightsB  = nullptr;
            const Parsers::SoundEventsBlock* soundsB  = nullptr;
            const Parsers::AnimCtrlBlock*    animCtlB = nullptr;
            const Parsers::AnimEventsBlock*  animEvB  = nullptr;
            for (const auto& b : s.m_parsedDoc.merged.blocks) {
                if (b.lights         && !lightsB)  lightsB  = b.lights.get();
                if (b.soundEvents    && !soundsB)  soundsB  = b.soundEvents.get();
                if (b.animController && !animCtlB) animCtlB = b.animController.get();
                if (b.animEvents     && !animEvB)  animEvB  = b.animEvents.get();
            }

            // --- Lights ------------------------------------------------------------
            {
                const int n = lightsB ? static_cast<int>(lightsB->lights.size()) : 0;
                const std::string hdr = "Lights (" + std::to_string(n) + ")###aoc_info_lights";
                if (ImGui::CollapsingHeader(hdr.c_str())) {
                    if (!lightsB || lightsB->lights.empty()) {
                        ImGui::TextDisabled("- No Lights block");
                    } else {
                        if (lightsB->skippedDirectional > 0) {
                            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                                               "Skipped %d directional_light (unsupported)",
                                               lightsB->skippedDirectional);
                        }
                        if (ImGui::BeginTable("##aoc_lights_tbl", 6,
                                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                            ImGui::TableSetupColumn("Type");
                            ImGui::TableSetupColumn("Name");
                            ImGui::TableSetupColumn("Bone");
                            ImGui::TableSetupColumn("States");
                            ImGui::TableSetupColumn("Events");
                            ImGui::TableSetupColumn("Shadow");
                            ImGui::TableHeadersRow();
                            for (const auto& ld : lightsB->lights) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                const char* tStr = (ld.type == Parsers::LightType::Spot) ? "spot" : "point";
                                ImGui::TextUnformatted(tStr);
                                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(ld.name.c_str());
                                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(ld.boneName.c_str());
                                ImGui::TableSetColumnIndex(3); ImGui::Text("%zu", ld.states.size());
                                ImGui::TableSetColumnIndex(4); ImGui::Text("%zu", ld.events.size());
                                ImGui::TableSetColumnIndex(5);
                                ImGui::TextUnformatted(ld.castsShadow ? "yes" : "no");
                            }
                            ImGui::EndTable();
                        }
                    }
                }
            }

            // --- Sound Events ------------------------------------------------------
            {
                const int n = soundsB ? static_cast<int>(soundsB->clips.size()) : 0;
                const std::string hdr = "Sound Events (" + std::to_string(n) + ")###aoc_info_sounds";
                if (ImGui::CollapsingHeader(hdr.c_str())) {
                    if (!soundsB || soundsB->clips.empty()) {
                        ImGui::TextDisabled("- No SoundEvents block");
                    } else {
                        if (ImGui::BeginTable("##aoc_sounds_tbl", 6,
                                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                            ImGui::TableSetupColumn("t (s)");
                            ImGui::TableSetupColumn("Type");
                            ImGui::TableSetupColumn("Filename");
                            ImGui::TableSetupColumn("Bone");
                            ImGui::TableSetupColumn("Status");
                            ImGui::TableSetupColumn("##play");
                            ImGui::TableHeadersRow();
                            for (std::size_t i = 0; i < soundsB->clips.size(); ++i) {
                                const auto& c = soundsB->clips[i];
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0); ImGui::Text("%.3f", c.time);
                                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(c.type.c_str());
                                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(c.filename.c_str());
                                ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(c.boneName.c_str());
                                ImGui::TableSetColumnIndex(4);
                                bool missing = false;
                                if (s.m_audio) {
                                    const auto& log = s.m_audio->MissingLog();
                                    missing = std::find(log.begin(), log.end(), c.filename) != log.end();
                                }
                                if (missing) ImGui::TextColored(Aoc::Ui::kMissingColor, "MISSING");
                                else         ImGui::TextColored(Aoc::Ui::kOkColor,      "OK");

                                ImGui::TableSetColumnIndex(5);
                                ImGui::PushID(static_cast<int>(i));
                                if (ImGui::SmallButton("Play") && s.m_audio) {
                                    s.m_audio->TriggerByLogicalName(c.filename, c.boneName);
                                }
                                ImGui::PopID();
                            }
                            ImGui::EndTable();
                        }
                    }
                }
            }

            // --- Particle Effects (Phase 3 summary row) ---------------------------
            {
                const auto& ps = s.m_particles;
                const std::size_t emitters = ps.EmitterCount();
                const std::size_t missing  = ps.MissingCount();
                const std::size_t unique   = ps.UniqueFilenames();

                // Count animations (unique animationName values in emitters).
                std::unordered_set<std::string> anims;
                for (const auto& e : ps.Emitters()) anims.insert(e.animationName);

                ImGui::Text("Particle Effects:");
                ImGui::SameLine();
                if (emitters == 0) {
                    ImGui::TextDisabled("none");
                } else if (missing > 0) {
                    ImGui::Text("%zu animation%s, %zu emitter%s (%zu unique .pet, ",
                                anims.size(), anims.size() == 1 ? "" : "s",
                                emitters,     emitters == 1 ? "" : "s",
                                unique);
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%zu missing", missing);
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::Text(")");
                } else {
                    ImGui::Text("%zu animation%s, %zu emitter%s (%zu unique .pet)",
                                anims.size(), anims.size() == 1 ? "" : "s",
                                emitters,     emitters == 1 ? "" : "s",
                                unique);
                }
            }

            // ----- Phase 4 — Transitionable -----
            ImGui::SeparatorText("Transitionable");
            const auto& tr = s.m_transitions;
            if (!tr.IsAvailable()) {
                ImGui::TextDisabled("Transitionable: none");
            } else {
                if (!tr.DiscoveredOtPath().empty()) {
                    ImGui::Text("Source .ot:  %s  [OK]", tr.DiscoveredOtPath().c_str());
                } else {
                    ImGui::TextDisabled("Source .ot:  (none)");
                }
                if (!tr.DiscoveredOtcPath().empty()) {
                    ImGui::Text("Source .otc: %s  [OK]", tr.DiscoveredOtcPath().c_str());
                } else {
                    ImGui::TextDisabled("Source .otc: (none)");
                }
                if (!tr.OverrideOtPath().empty()) {
                    ImGui::Text("Override:    %s", tr.OverrideOtPath().c_str());
                } else {
                    ImGui::TextDisabled("Override:    (none)");
                }
                const auto* tb = tr.OtFile().transitionable.get();
                if (tb) {
                    int handlerMax = -1;
                    for (const auto& h : tb->handlers) if (h.stateIndex > handlerMax) handlerMax = h.stateIndex;
                    if (tb->isBinary) {
                        ImGui::Text("States:      %d (binary form, declared %d)",
                                    tb->numStates, tb->declaredNumStates);
                    } else if (handlerMax + 1 > tb->declaredNumStates) {
                        ImGui::Text("States:      %d (declared %d, +%d from handler indices)",
                                    tb->numStates, tb->declaredNumStates,
                                    handlerMax + 1 - tb->declaredNumStates);
                    } else {
                        ImGui::Text("States:      %d (declared %d)",
                                    tb->numStates, tb->declaredNumStates);
                    }
                    ImGui::Text("Handlers:    %d", static_cast<int>(tb->handlers.size()));
                }
            }

            // --- Animation Events --------------------------------------------------
            // Enumerates:
            //   (a) on_event_* script handlers from AnimationController block
            //       (AnimCtrlBlock::eventHandlers is vector<KeyValue>; key = handler
            //       name stripped of the "on_event_" prefix by the parser, value = script body)
            //   (b) timeline events (HapticEventType, etc.) from the separate
            //       AnimEventsBlock (vector<AnimEventRef>: kind/name/time/summary)
            {
                std::size_t count = 0;
                if (animCtlB) count += animCtlB->eventHandlers.size();
                if (animEvB)  count += animEvB->events.size();

                const std::string hdr = "Animation Events (" + std::to_string(count) + ")###aoc_info_anim_events";
                if (ImGui::CollapsingHeader(hdr.c_str())) {
                    if (count == 0) {
                        ImGui::TextDisabled("- No AnimationController events");
                    } else {
                        if (ImGui::BeginTable("##aoc_anim_events_tbl", 4,
                                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                            ImGui::TableSetupColumn("Kind");
                            ImGui::TableSetupColumn("Name");
                            ImGui::TableSetupColumn("t (s)");
                            ImGui::TableSetupColumn("Summary");
                            ImGui::TableHeadersRow();

                            // (a) on_event_* script handlers (no timeline time)
                            if (animCtlB) {
                                for (const auto& kv : animCtlB->eventHandlers) {
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("on_event");
                                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(kv.key.c_str());
                                    ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("-");
                                    ImGui::TableSetColumnIndex(3);
                                    const std::string& body = kv.value;
                                    const std::string summary = body.size() > 80
                                        ? body.substr(0, 80) + "..."
                                        : body;
                                    ImGui::TextUnformatted(summary.c_str());
                                }
                            }

                            // (b) Timeline events (haptics / other *EventType entries)
                            if (animEvB) {
                                for (const auto& ev : animEvB->events) {
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0);
                                    const char* kindStr = "event";
                                    switch (ev.kind) {
                                        case Parsers::AnimCtrlEventKind::OnEvent:      kindStr = "on_event"; break;
                                        case Parsers::AnimCtrlEventKind::Haptic:       kindStr = "haptic";   break;
                                        case Parsers::AnimCtrlEventKind::GenericEvent: kindStr = "event";    break;
                                    }
                                    ImGui::TextUnformatted(kindStr);
                                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(ev.name.c_str());
                                    ImGui::TableSetColumnIndex(2); ImGui::Text("%.3f", ev.time);
                                    ImGui::TableSetColumnIndex(3);
                                    const std::string summary = ev.summary.size() > 80
                                        ? ev.summary.substr(0, 80) + "..."
                                        : ev.summary;
                                    ImGui::TextUnformatted(summary.c_str());
                                }
                            }

                            ImGui::EndTable();
                        }
                    }
                }
            }

            ImGui::EndTabItem();
        }

        // ==================================================================
        // Attachments sub-tab: Live Attachments listing / Persistence
        // ==================================================================
        if (ImGui::BeginTabItem("Attachments")) {
            // ----- Phase 4 — Attachments -----
            const int nc = s.m_sceneGraph.ChildCount();
            if (nc == 0) {
                ImGui::TextDisabled("Attachments: none");
            } else {
                ImGui::Text("Attachments (%d)", nc);
                ImGui::Indent();
                for (const auto& child : s.m_sceneGraph.Children()) {
                    const bool hasWarn = !child.warnings.empty();
                    ImGui::TextColored(hasWarn ? ImVec4(1.0f, 0.6f, 0.3f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
                                       "  %s -> %s  [%s]",
                                       child.socketName.c_str(),
                                       child.sourcePath.c_str(),
                                       hasWarn ? "WARN" : "OK");

                    // Phase 5.2 — skin-mode badge (consolidated in Aoc::Ui helper).
                    Aoc::Ui::RenderSkinModeBadge(child);

                    // Phase 5.2.1 — transform override readout (consolidated in Aoc::Ui helper).
                    ImGui::Indent();
                    Aoc::Ui::RenderTransformLine(child);
                    ImGui::Unindent();
                }
                ImGui::Unindent();
            }

            // ----- Phase 5.1 — Persistence -----
            ImGui::SeparatorText("Persistence");

            if (!m_state.m_persisted.JsonExistsOnDisk()) {
                ImGui::TextDisabled("(no persistence for this file)");
            } else if (!m_state.m_persisted.IsLoaded()) {
                // JSON found but parse failed — show warnings if any
                ImGui::TextColored(Aoc::Ui::kMissingColor, "JSON found but parse failed");
                if (!m_state.m_persisted.Warnings().empty()) {
                    ImGui::TextUnformatted("Warnings:");
                    for (const auto& w : m_state.m_persisted.Warnings()) {
                        ImGui::BulletText("%s", w.c_str());
                    }
                }
                ImGui::TextDisabled("JSON path: %s", m_state.m_persisted.JsonPath().c_str());
                if (ImGui::Button("Clear Persistence")) {
                    m_state.m_persisted.ClearPersistence(m_state.m_pluginDir, m_state.m_filePath);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Deletes the corrupt JSON file. Current session state remains untouched.");
                }
            } else {
                // Status line summary.
                const int  attachCount = m_state.m_persisted.AttachmentCount();
                const bool hasState    = m_state.m_persisted.HasTransitionState();
                const int  curState    = m_state.m_transitions.CurrentState();
                const std::string& overridePath = m_state.m_transitions.OverrideOtPath();

                char statusBuf[192];
                if (hasState) {
                    std::snprintf(statusBuf, sizeof(statusBuf),
                                  "persisted - %d attachments, state %d, override: %s",
                                  attachCount, curState,
                                  overridePath.empty() ? "(none)" : overridePath.c_str());
                } else {
                    std::snprintf(statusBuf, sizeof(statusBuf), "persisted - %d attachments", attachCount);
                }
                ImGui::Text("Status:  %s", statusBuf);

                // JSON path + Copy button.
                if (ImGui::SmallButton("Copy##persistJsonPath")) {
                    ImGui::SetClipboardText(m_state.m_persisted.JsonPath().c_str());
                }
                ImGui::SameLine();
                ImGui::TextWrapped("JSON: %s", m_state.m_persisted.JsonPath().c_str());

                // Per-entry listing.
                if (!m_state.m_persisted.Entries().empty()) {
                    ImGui::TextUnformatted("Entries:");
                    for (const auto& e : m_state.m_persisted.Entries()) {
                        const char* statusLabel = "[OK]";
                        ImVec4 statusColor = Aoc::Ui::kOkColor;
                        switch (e.status) {
                            case Aoc::AocPersistedAttachments::EntryStatus::MissingSource:
                                statusLabel = "[MISSING source]";
                                statusColor = Aoc::Ui::kMissingColor;
                                break;
                            case Aoc::AocPersistedAttachments::EntryStatus::MissingSocket:
                                statusLabel = "[MISSING socket]";
                                statusColor = Aoc::Ui::kMissingColor;
                                break;
                            case Aoc::AocPersistedAttachments::EntryStatus::ParseFailed:
                                statusLabel = "[PARSE FAILED]";
                                statusColor = Aoc::Ui::kWarningColor;
                                break;
                            default: break;
                        }
                        ImGui::BulletText("%s -> %s  ", e.socket.c_str(), e.sourcePath.c_str());
                        ImGui::SameLine();
                        ImGui::TextColored(statusColor, "%s", statusLabel);

                        // Phase 5.2 — skin-mode badge (only when the persisted entry is
                        // backed by a live AocChildMesh; skip for [MISSING] entries
                        // since skin-mode is irrelevant without a resolved child).
                        if (const Aoc::AocChildMesh* child =
                                m_state.m_sceneGraph.FindChildBySocket(e.socket)) {
                            Aoc::Ui::RenderSkinModeBadge(*child);
                        }
                    }
                }

                // Clear Persistence button.
                if (ImGui::Button("Clear Persistence")) {
                    m_state.m_persisted.ClearPersistence(m_state.m_pluginDir, m_state.m_filePath);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Deletes JSON file for this .aoc. Current session attachments + state remain untouched.");
                }
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

} // namespace GGPKExp
