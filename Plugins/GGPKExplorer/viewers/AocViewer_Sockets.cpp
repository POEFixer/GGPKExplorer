#include "AocViewer.h"

#include "../core/GGPKManager.h"      // for FindNodeByPath / ReadFile
#include "../ui/AttachBrowseModal.h"  // for m_browseModal->Open call
#include "aoc/AocUiHelpers.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace GGPKExp {

namespace {

// Case-insensitive suffix check for ".sm" / ".aoc" path filtering in the
// "Attach Selected Mesh" context menu gating.
bool EndsWithCaseInsensitive(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[s.size() - n + i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b) return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Socket tab UI state (file-local, persists across frames for the single viewer
// instance; ImGui's own scroll/selection is handled separately via tab_id).
// -----------------------------------------------------------------------------
struct SocketsTabState {
    char filter[128]  = {};
    int  sortColumn   = 0;     // 0 = Name, 1 = Parent, 6 = Source (see column list)
    bool sortAscend   = true;
    bool showInherit  = true;  // show sockets inherited from parent .aoc files
    bool onlyAttached = false; // T13 — show only sockets with attached meshes
};

SocketsTabState& GetSocketsTabState() {
    // One global per-process state is fine — only one AocViewer is rendered at
    // a time (the ContentPanel swaps viewers on selection change and calls
    // Clear() between loads, so leftover filter text is actually desirable).
    static SocketsTabState s;
    return s;
}

// -----------------------------------------------------------------------------
// A flat view of a socket + back-pointer into the original ClientAnimCtrl. We
// aggregate across every ClientAnimationController block in merged.blocks
// (usually there's just one, but mirroring AocViewer_Info's defensive pattern
// keeps us robust against multi-block files).
// -----------------------------------------------------------------------------
struct SocketView {
    const Parsers::Socket* sock       = nullptr;
    bool                   inherited  = false;  // sourceAocPath != leaf path
};

// Case-insensitive substring match — a naive ASCII fold is fine for filter
// boxes (socket names are conventionally ASCII: "mainhand", "rf_anchor", etc.).
bool ContainsNoCase(const std::string& hay, const char* needle) {
    if (!needle || !needle[0]) return true;
    const std::size_t hLen = hay.size();
    const std::size_t nLen = std::strlen(needle);
    if (nLen > hLen) return false;
    for (std::size_t i = 0; i + nLen <= hLen; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < nLen; ++j) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(hay[i + j])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// Renders "A" if ignoreParentAxis is non-empty, "R" if ignoreParentRot is
// non-empty, dashes otherwise. Separated out so the table row code stays
// readable.
void FormatFlags(const Parsers::Socket& s, char* out, std::size_t outSize) {
    const char axis = s.ignoreParentAxis.empty() ? '-' : 'A';
    const char rot  = s.ignoreParentRot.empty()  ? '-' : 'R';
    std::snprintf(out, outSize, "%c%c", axis, rot);
}

} // namespace

void AocViewer::RenderSocketsTab() {
    auto& tab = GetSocketsTabState();

    // -------------------------------------------------------------------------
    // Aggregate sockets across all ClientAnimationController blocks and tag
    // each with an "inherited" flag derived from the merged document's
    // inheritance chain (index 0 = leaf .aoc path).
    // -------------------------------------------------------------------------
    const auto& chain = m_state.m_parsedDoc.inheritanceChain;
    const std::string leafPath = chain.empty() ? m_state.m_filePath : chain.front();

    std::vector<SocketView> allSockets;
    for (const auto& blk : m_state.m_parsedDoc.merged.blocks) {
        if (!blk.clientAnim) continue;
        allSockets.reserve(allSockets.size() + blk.clientAnim->sockets.size());
        for (const auto& sk : blk.clientAnim->sockets) {
            SocketView sv;
            sv.sock      = &sk;
            sv.inherited = !leafPath.empty()
                           && !sk.sourceAocPath.empty()
                           && sk.sourceAocPath != leafPath;
            allSockets.push_back(sv);
        }
    }

    // -------------------------------------------------------------------------
    // Filter / toggle row
    // -------------------------------------------------------------------------
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("##SocketFilter", "Filter by name...",
                              tab.filter, sizeof(tab.filter));
    ImGui::SameLine();
    ImGui::Checkbox("Show Inherited", &tab.showInherit);
    ImGui::SameLine();
    ImGui::Checkbox("Only Attached", &tab.onlyAttached);
    ImGui::SameLine();
    ImGui::Text("%zu sockets", allSockets.size());

    // -------------------------------------------------------------------------
    // Apply filter + inherited toggle
    // -------------------------------------------------------------------------
    std::vector<SocketView> visible;
    visible.reserve(allSockets.size());
    for (const auto& sv : allSockets) {
        if (!tab.showInherit && sv.inherited) continue;
        if (!ContainsNoCase(sv.sock->name, tab.filter)) continue;
        if (tab.onlyAttached && !m_state.m_sceneGraph.FindChildBySocket(sv.sock->name)) continue;  // T13
        visible.push_back(sv);
    }

    if (allSockets.empty()) {
        ImGui::TextDisabled("(no ClientAnimationController sockets in this file)");
        return;
    }

    // -------------------------------------------------------------------------
    // Table
    // -------------------------------------------------------------------------
    const ImGuiTableFlags tableFlags =
        ImGuiTableFlags_Sortable   | ImGuiTableFlags_SortTristate |
        ImGuiTableFlags_RowBg      | ImGuiTableFlags_Borders      |
        ImGuiTableFlags_ScrollY    | ImGuiTableFlags_Resizable    |
        ImGuiTableFlags_Reorderable;

    if (!ImGui::BeginTable("##Sockets", 8, tableFlags, ImVec2(0, 0))) {
        return;
    }

    // Column 0 acts as the default sort key on first frame.
    ImGui::TableSetupColumn("Name",        ImGuiTableColumnFlags_DefaultSort, 0.0f, 0);
    ImGui::TableSetupColumn("Parent Bone", ImGuiTableColumnFlags_None,        0.0f, 1);
    ImGui::TableSetupColumn("Translation", ImGuiTableColumnFlags_NoSort,      0.0f, 2);
    ImGui::TableSetupColumn("Rotation",    ImGuiTableColumnFlags_NoSort,      0.0f, 3);
    ImGui::TableSetupColumn("Scale",       ImGuiTableColumnFlags_NoSort,      0.0f, 4);
    ImGui::TableSetupColumn("Flags",       ImGuiTableColumnFlags_None,        0.0f, 5);
    ImGui::TableSetupColumn("Source",      ImGuiTableColumnFlags_None,        0.0f, 6);
    ImGui::TableSetupColumn("Attached",    ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort,
                                                                              0.30f, 7);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    // Pick up sort changes from ImGui's own sort specs. Only honour the
    // primary sort column (index 0); multi-key sorts aren't exposed in the UI.
    if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
        if (sortSpecs->SpecsDirty && sortSpecs->SpecsCount > 0) {
            tab.sortColumn = sortSpecs->Specs[0].ColumnIndex;
            tab.sortAscend = (sortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending);
            sortSpecs->SpecsDirty = false;
        }
    }

    // Sort visible[] in place. Translation/Rotation/Scale columns are flagged
    // NoSort so we don't have to invent a sort order for vec3 values.
    auto cmp = [&](const SocketView& a, const SocketView& b) {
        int c = 0;
        switch (tab.sortColumn) {
            case 0: c = a.sock->name.compare(b.sock->name);                     break;
            case 1: c = a.sock->parentBoneName.compare(b.sock->parentBoneName); break;
            case 5: {
                // Flags column: compare as a 2-char synthetic string.
                char fa[4], fb[4];
                FormatFlags(*a.sock, fa, sizeof(fa));
                FormatFlags(*b.sock, fb, sizeof(fb));
                c = std::strcmp(fa, fb);
                break;
            }
            case 6: c = a.sock->sourceAocPath.compare(b.sock->sourceAocPath);   break;
            default: c = a.sock->name.compare(b.sock->name);                    break;
        }
        return tab.sortAscend ? (c < 0) : (c > 0);
    };
    std::sort(visible.begin(), visible.end(), cmp);

    // -------------------------------------------------------------------------
    // Virtualised render: character.aoc exposes 60+ sockets, so we use
    // ImGuiListClipper to keep per-frame work proportional to viewport height
    // rather than socket count.
    // -------------------------------------------------------------------------
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const SocketView& sv = visible[static_cast<std::size_t>(row)];
            const Parsers::Socket& sk = *sv.sock;

            ImGui::TableNextRow();

            // Col 0 — Name (Selectable spans the full row so right-click and
            // context-menu interactions are predictable).
            // AllowOverlap lets nested widgets (e.g. the [T] transform button
            // in col 7) receive clicks instead of being eaten by the Selectable's
            // row-wide hit test — required since the Phase 5.2.1 button addition.
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(row);
            const bool selected = false; // no persistent selection in this tab
            ImGui::Selectable(sk.name.c_str(), selected,
                              ImGuiSelectableFlags_SpanAllColumns |
                              ImGuiSelectableFlags_AllowOverlap);

            // Context menu: right-click anywhere on the row opens it. Using
            // BeginPopupContextItem() keeps the popup scoped to this row's
            // Selectable widget (PushID keeps IDs unique across rows).
            if (ImGui::BeginPopupContextItem("##SockCtx")) {
                if (ImGui::MenuItem("Copy name")) {
                    ImGui::SetClipboardText(sk.name.c_str());
                }

                ImGui::Separator();

                // "Attach Selected Mesh" — reads current ContentPanel selection via
                // the injected callback. Gated to .sm / .aoc files so the user can't
                // try to attach a .dat or .txt by accident. Label shows the selected
                // basename when valid; otherwise prompts for a correct selection.
                AocContentSelection selection;
                if (m_selectionProvider) selection = m_selectionProvider();
                const bool selPresent = !selection.path.empty() && !selection.bytes.empty();
                const bool selIsMeshLike = selPresent &&
                        (EndsWithCaseInsensitive(selection.path, ".sm") ||
                         EndsWithCaseInsensitive(selection.path, ".aoc"));

                std::string attachLabel;
                if (selIsMeshLike) {
                    std::string base = selection.path;
                    const std::size_t slash = base.find_last_of("/\\");
                    if (slash != std::string::npos) base = base.substr(slash + 1);
                    attachLabel = "Attach Selected Mesh (" + base + ")";
                } else {
                    attachLabel = "Attach Selected Mesh (select .sm or .aoc first)";
                }
                if (ImGui::MenuItem(attachLabel.c_str(), nullptr, false, selIsMeshLike)) {
                    DoAttachCommon(sk.name, selection.path, selection.bytes);
                }

                // Phase 5.1.5a — open modal file browser.
                if (ImGui::MenuItem("Browse and Attach...")) {
                    if (m_browseModal) {
                        const std::string socketName = sk.name;  // capture by value
                        m_browseModal->SetParentAocPath(m_state.m_filePath);
                        m_browseModal->Open(socketName,
                            [this](const std::string& sock,
                                   const std::string& path,
                                   const std::vector<uint8_t>& bytes) {
                                this->DoAttachCommon(sock, path, bytes);
                            });
                    }
                }

                // Phase 5.1.5a — submenu of recently attached source paths (global LRU).
                const auto& recentEntries = m_state.m_recent.Entries();
                const bool hasRecent = !recentEntries.empty();
                if (ImGui::BeginMenu("Recent Attachments", hasRecent)) {
                    for (std::size_t ri = 0; ri < recentEntries.size(); ++ri) {
                        const auto& entry = recentEntries[ri];
                        // Basename for display.
                        std::string basename = entry.sourcePath;
                        const std::size_t slash = basename.find_last_of("/\\");
                        if (slash != std::string::npos) basename = basename.substr(slash + 1);

                        ImGui::PushID(static_cast<int>(ri));
                        if (ImGui::MenuItem(basename.c_str())) {
                            // Read bytes from GGPK; if path vanished, show red toast.
                            void* node = m_state.m_mgr ? m_state.m_mgr->FindNodeByPath(entry.sourcePath) : nullptr;
                            if (!node) {
                                Aoc::Ui::PushToast(m_state,
                                                   "File not found: " + entry.sourcePath,
                                                   /*isError=*/true);
                            } else {
                                auto res = m_state.m_mgr->ReadFile(node);
                                if (!res.success) {
                                    Aoc::Ui::PushToast(m_state,
                                                       "Read failed: " + res.error,
                                                       /*isError=*/true);
                                } else {
                                    DoAttachCommon(sk.name, entry.sourcePath, res.value);
                                }
                            }
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", entry.sourcePath.c_str());
                        }
                        ImGui::PopID();
                    }

                    if (hasRecent) {
                        ImGui::Separator();
                        if (ImGui::MenuItem("Clear Recent List")) {
                            m_state.m_recent.Clear();
                            m_state.m_recent.Save(m_state.m_pluginDir);
                        }
                    }
                    ImGui::EndMenu();
                }

                // "Detach Mesh" — only enabled when an attachment exists at this socket.
                const bool hasAttachment =
                        (m_state.m_sceneGraph.FindChildBySocket(sk.name) != nullptr);
                if (ImGui::MenuItem("Detach Mesh", nullptr, false, hasAttachment)) {
                    if (m_state.m_sceneGraph.DetachChild(sk.name)) {
                        // Detach success → green toast.
                        Aoc::Ui::PushToast(m_state,
                                           "Detached from " + sk.name,
                                           /*isError=*/false);
                        // Phase 5.1 — persist attachments on successful detach.
                        m_state.m_persisted.Save(m_state.m_pluginDir,
                                                 m_state.m_filePath,
                                                 m_state.m_sceneGraph,
                                                 m_state.m_transitions);
                    }
                }

                ImGui::EndPopup();
            }

            // Col 1 — Parent bone
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(sk.parentBoneName.c_str());

            // Col 2 — Translation
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.2f %.2f %.2f",
                        sk.translation[0], sk.translation[1], sk.translation[2]);

            // Col 3 — Rotation (Euler XYZ radians per AocTypes.h comment)
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.2f %.2f %.2f",
                        sk.rotation[0], sk.rotation[1], sk.rotation[2]);

            // Col 4 — Scale
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.2f %.2f %.2f",
                        sk.scale[0], sk.scale[1], sk.scale[2]);

            // Col 5 — Flags ("A" = ignoreParentAxis set, "R" = ignoreParentRot set)
            ImGui::TableSetColumnIndex(5);
            char flagBuf[4];
            FormatFlags(sk, flagBuf, sizeof(flagBuf));
            ImGui::TextUnformatted(flagBuf);

            // Col 6 — Source (greyed if inherited so it reads as secondary info)
            ImGui::TableSetColumnIndex(6);
            if (sv.inherited) {
                ImGui::TextDisabled("%s", sk.sourceAocPath.c_str());
            } else {
                ImGui::TextUnformatted(sk.sourceAocPath.c_str());
            }

            // Col 7 — Attached (basename of attached mesh; orange when the
            // child reports warnings; tooltip shows full GGPK path on hover).
            ImGui::TableSetColumnIndex(7);
            Aoc::AocChildMesh* attached =
                    m_state.m_sceneGraph.FindChildBySocket(sk.name);
            if (attached) {
                std::string base = attached->sourcePath;
                const std::size_t slash = base.find_last_of("/\\");
                if (slash != std::string::npos) base = base.substr(slash + 1);
                const ImVec4 col = !attached->warnings.empty()
                        ? ImVec4(1.0f, 0.6f, 0.3f, 1.0f)
                        : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                ImGui::TextColored(col, "%s", base.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(attached->sourcePath.c_str());
                    ImGui::EndTooltip();
                }

                // Phase 5.2 — skin mode badge (SameLine'd after the source path
                // so it renders on the same row as the basename above).
                Aoc::Ui::RenderSkinModeBadge(*attached);

                // Phase 5.2.1 — [T] button + [modified] badge + popup.
                ImGui::SameLine();
                if (ImGui::SmallButton("T")) {
                    ImGui::OpenPopup("##TOverride");
                }
                if (ImGui::IsItemHovered() && !ImGui::IsPopupOpen("##TOverride")) {
                    ImGui::SetTooltip("Transform override (translate / rotate / scale)");
                }

                const bool transformModified = !Aoc::IsTransformIdentity(*attached);
                if (transformModified) {
                    ImGui::SameLine();
                    ImGui::TextColored(Aoc::Ui::kWarningColor, "[modified]");
                }

                (void)Aoc::Ui::RenderTransformOverridePopup(*attached);

                // T8 / B1 debounce: track popup open/close per-row. On open->close transition,
                // if the row's pendingTransformSave flag is set, flush one Save and reset.
                const bool popupOpenNow = ImGui::IsPopupOpen("##TOverride");
                const bool wasOpenThisRow = (m_state.m_transformPopupOpenRow == sk.name);
                if (popupOpenNow && !wasOpenThisRow) {
                    // Our popup just opened on this row (previous row's popup, if any, closed).
                    m_state.m_transformPopupOpenRow = sk.name;
                } else if (!popupOpenNow && wasOpenThisRow) {
                    // Popup closed this frame. Flush if edits happened.
                    if (attached->pendingTransformSave) {
                        m_state.m_persisted.Save(m_state.m_pluginDir,
                                                 m_state.m_filePath,
                                                 m_state.m_sceneGraph,
                                                 m_state.m_transitions);
                        attached->pendingTransformSave = false;
                    }
                    m_state.m_transformPopupOpenRow.clear();
                }
            } else {
                ImGui::TextDisabled("(none)");
            }

            ImGui::PopID();
        }
    }

    ImGui::EndTable();
}

void AocViewer::DoAttachCommon(const std::string& socket,
                               const std::string& sourcePath,
                               const std::vector<uint8_t>& sourceBytes) {
    std::string err;
    const bool ok = m_state.m_sceneGraph.AttachChild(
        socket, sourcePath, sourceBytes, m_state.m_mgr, err);

    Aoc::Ui::PushToast(m_state,
                       ok ? ("Attached " + sourcePath + " -> " + socket)
                          : ("Attach failed: " + err),
                       /*isError=*/!ok);

    if (ok) {
        m_state.m_recent.Record(sourcePath);
        m_state.m_recent.Save(m_state.m_pluginDir);

        m_state.m_persisted.Save(m_state.m_pluginDir,
                                 m_state.m_filePath,
                                 m_state.m_sceneGraph,
                                 m_state.m_transitions);
    }
}

} // namespace GGPKExp
