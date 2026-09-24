#include "PsgViewer.h"
#include "../core/StatFormatter.h"
#include <algorithm>
#include <cstdio>
#include <cmath>

namespace GGPKExp {

// Base node RENDER radii at zoom=1.0. Icons rendered as circles.
static constexpr float kBaseRadiusSmall    = 36.0f;
static constexpr float kBaseRadiusNotable  = 54.0f;
static constexpr float kBaseRadiusRoot     = 72.0f;
static constexpr float kBaseRadiusJewel    = 42.0f;

void PsgViewer::RenderVisual() {
    // Drive state machine; render spinner while the worker runs.
    if (PollLoadState()) {
        DrawLoadingSpinner("Loading skill tree...");
        return;
    }
    if (m_loadState == LoadState::Error) {
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Load error: %s", m_error.c_str());
        return;
    }
    if (m_loadState == LoadState::Idle || !m_loaded) {
        ImGui::TextDisabled("No data loaded");
        return;
    }

    // --- Toolbar ---
    if (ImGui::Button("Fit")) {
        m_needsFit = true;
    }

    // Class selector — picks which class's ascendancy subtree to reveal.
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    const char* selectedLabel = "No lines";
    std::string selectedBuf;
    if (m_selectedClassIdx >= 0 && m_selectedClassIdx < static_cast<int>(m_psg.rootNodes.size())) {
        uint32_t h = m_psg.rootNodes[static_cast<size_t>(m_selectedClassIdx)];
        const PassiveSkillInfo* info = GetSkillInfo(h);
        if (info && !info->name.empty()) selectedBuf = info->name;
        else { char b[32]; std::snprintf(b, sizeof(b), "Root %d", m_selectedClassIdx); selectedBuf = b; }
        selectedLabel = selectedBuf.c_str();
    }
    if (ImGui::BeginCombo("##PsgClassFilter", selectedLabel)) {
        if (ImGui::Selectable("No lines", m_selectedClassIdx == -1))
            m_selectedClassIdx = -1;
        for (size_t i = 0; i < m_psg.rootNodes.size(); ++i) {
            uint32_t h = m_psg.rootNodes[i];
            const PassiveSkillInfo* info = GetSkillInfo(h);
            std::string label;
            if (info && !info->name.empty()) label = info->name;
            else { char b[32]; std::snprintf(b, sizeof(b), "Root %zu", i); label = b; }
            bool isSel = (m_selectedClassIdx == static_cast<int>(i));
            if (ImGui::Selectable(label.c_str(), isSel))
                m_selectedClassIdx = static_cast<int>(i);
            if (isSel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Language selector for stat descriptions. Populated from
    // m_availableLangs (English first, rest alphabetical). Hidden if no
    // languages available yet (pre-load or broken CSDs).
    if (!m_availableLangs.empty()) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::BeginCombo("##PsgLangFilter", m_currentLang.c_str())) {
            for (const auto& lang : m_availableLangs) {
                const bool selected = (lang == m_currentLang);
                if (ImGui::Selectable(lang.c_str(), selected)) {
                    if (lang != m_currentLang) {
                        m_currentLang = lang;
                        if (m_settings) {
                            m_settings->statDescLanguage = lang;
                            m_settingsDirty = true;
                        }
                    }
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    // --- Search UI ---
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::InputTextWithHint("##psg_search", "Search nodes (name or stat)...",
                                 m_searchBuf, sizeof(m_searchBuf))) {
        RebuildSearchMatches();
    }

    ImGui::SameLine();
    if (ImGui::Button("<##psg_prev")) NavigatePrev();
    ImGui::SameLine();
    if (ImGui::Button(">##psg_next")) NavigateNext();

    ImGui::SameLine();
    if (m_searchBuf[0] == '\0') {
        ImGui::TextDisabled(" ");
    } else if (m_searchMatches.empty()) {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "no matches");
    } else {
        ImGui::Text("%d/%d", m_searchIndex + 1, static_cast<int>(m_searchMatches.size()));
    }

    ImGui::SameLine();
    ImGui::Text("Zoom: %.2fx", m_zoom);
    ImGui::SameLine();
    ImGui::Text("| Nodes: %u  Edges: %u  Groups: %u",
                m_psg.totalNodes, m_psg.totalEdges,
                static_cast<uint32_t>(m_psg.groups.size()));
    ImGui::SameLine();
    ImGui::TextDisabled("| RMB/LMB drag to pan, Scroll to zoom, Hold Ctrl for details");

    // --- Icon load progress (inline) ---
    if (m_iconLoadState == IconLoadState::Loading) {
        float pct = m_iconsTotalThisLoad > 0
                  ? static_cast<float>(m_iconsLoadedThisLoad) / static_cast<float>(m_iconsTotalThisLoad)
                  : 0.0f;
        char buf[64];
        snprintf(buf, sizeof(buf), "Loading icons %d/%d", m_iconsLoadedThisLoad, m_iconsTotalThisLoad);
        ImGui::SameLine();
        ImGui::ProgressBar(pct, ImVec2(200, 0), buf);
    }

    // --- Canvas ---
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    if (canvasSize.x < 1.0f || canvasSize.y < 1.0f) return;

    ImGui::BeginChild("##PsgCanvas", canvasSize, false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSz = ImGui::GetContentRegionAvail();
    m_canvasLastSize = canvasSz;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Defer the first fit until the canvas has a real size. If the tab just
    // became visible the child can report a near-zero rect on the very first
    // frame; clearing m_needsFit in that state would leave the tree drawn at
    // the wrong zoom indefinitely.
    if (m_needsFit && canvasSz.x > 1.0f && canvasSz.y > 1.0f)
        FitToView(canvasSz);

    dl->AddRectFilled(canvasPos,
                      ImVec2(canvasPos.x + canvasSz.x, canvasPos.y + canvasSz.y),
                      IM_COL32(18, 18, 24, 255));

    ImGui::InvisibleButton("##PsgCanvasBtn", canvasSz,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);
    bool canvasHovered = ImGui::IsItemHovered();

    // --- Zoom ---
    if (canvasHovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            ImVec2 mp = ImGui::GetIO().MousePos;
            float mouseWorldX = (mp.x - canvasPos.x - m_pan.x) / m_zoom;
            float mouseWorldY = (mp.y - canvasPos.y - m_pan.y) / m_zoom;

            float factor = (wheel > 0) ? 1.15f : (1.0f / 1.15f);
            float newZoom = m_zoom * factor;
            newZoom = (std::max)(0.01f, (std::min)(5.0f, newZoom));

            m_pan.x = (mp.x - canvasPos.x) - mouseWorldX * newZoom;
            m_pan.y = (mp.y - canvasPos.y) - mouseWorldY * newZoom;
            m_zoom = newZoom;
        }
    }

    // --- Pan ---
    bool wantPanRMB = canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    bool wantPanMMB = canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
    bool wantPanLMB = canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_hoveredNodeHash == 0;

    if (!m_dragging && (wantPanRMB || wantPanMMB || wantPanLMB)) {
        m_dragging = true;
        m_dragButton = wantPanRMB ? ImGuiMouseButton_Right :
                       wantPanMMB ? ImGuiMouseButton_Middle : ImGuiMouseButton_Left;
        m_dragStart = ImGui::GetIO().MousePos;
        m_panStart = m_pan;
    }
    if (m_dragging) {
        if (ImGui::IsMouseDown(m_dragButton)) {
            ImVec2 delta;
            delta.x = ImGui::GetIO().MousePos.x - m_dragStart.x;
            delta.y = ImGui::GetIO().MousePos.y - m_dragStart.y;
            m_pan.x = m_panStart.x + delta.x;
            m_pan.y = m_panStart.y + delta.y;
        } else {
            m_dragging = false;
        }
    }

    auto isRoot = [this](uint32_t hash) -> bool {
        for (uint32_t rh : m_psg.rootNodes)
            if (rh == hash) return true;
        return false;
    };

    // Ascendancy filter — only the long edges to ascendancy nodes are gated by
    // the class selector. Nodes themselves always render.
    uint32_t selectedClassHash = 0;
    if (m_selectedClassIdx >= 0 && m_selectedClassIdx < static_cast<int>(m_psg.rootNodes.size()))
        selectedClassHash = m_psg.rootNodes[static_cast<size_t>(m_selectedClassIdx)];

    // Returns true if a given node hash is an ascendancy node that doesn't
    // belong to the currently selected class (so edges touching it must be
    // hidden). Non-ascendancy nodes always "belong" in the tree.
    auto isAscOwnedByOther = [&](uint32_t nodeHash) -> bool {
        const PassiveSkillInfo* info = GetSkillInfo(nodeHash);
        if (!info || info->ascendancy < 0) return false;   // not ascendancy
        if (selectedClassHash == 0) return true;            // no class chosen → hide its edges
        auto it = m_ascNodeToClass.find(nodeHash);
        return !(it != m_ascNodeToClass.end() && it->second == selectedClassHash);
    };

    // Edges render only when neither endpoint is an ascendancy node that
    // belongs to a class other than the selected one. Nodes themselves are
    // always visible — only the connecting edges gate on class.
    auto isEdgeShown = [&](uint32_t srcHash, uint32_t dstHash) -> bool {
        return !isAscOwnedByOther(srcHash) && !isAscOwnedByOther(dstHash);
    };

    // Render radius scales linearly with zoom; clamped so nodes stay visible
    // at low zoom and don't overwhelm the canvas at high zoom.
    auto scaleRadius = [&](float baseRadius) -> float {
        float r = baseRadius * m_zoom;
        float minR = 1.0f;
        float maxR = baseRadius * 4.0f;
        return (std::max)(minR, (std::min)(maxR, r));
    };

    // Classify a node into a render category and return its base radius.
    auto baseRadiusFor = [&](const Parsers::PsgNode& node, bool groupIsJewel,
                             const PassiveSkillInfo* skill) -> float {
        if (isRoot(node.hash)) return kBaseRadiusRoot;
        if (skill && skill->isKeystone) return kBaseRadiusRoot;
        if (skill && skill->isNotable) return kBaseRadiusNotable;
        if (skill && skill->isJewelSocket) return kBaseRadiusJewel;
        if (groupIsJewel) return kBaseRadiusJewel;
        if (node.orbit == 0) return kBaseRadiusNotable;
        return kBaseRadiusSmall;
    };

    float edgeThickness         = (std::max)(1.0f, (std::min)(3.0f, m_zoom * 1.2f));
    float edgeThicknessSelected = (std::max)(2.0f, (std::min)(5.0f, m_zoom * 2.0f));

    constexpr ImU32 COL_EDGE          = IM_COL32(150, 150, 160, 180);
    constexpr ImU32 COL_EDGE_SELECTED = IM_COL32(255, 200, 50, 230);
    constexpr ImU32 COL_ROOT          = IM_COL32(255, 200, 50, 255);
    constexpr ImU32 COL_KEYSTONE      = IM_COL32(230, 100, 90, 255);
    constexpr ImU32 COL_NOTABLE       = IM_COL32(80, 140, 255, 255);
    constexpr ImU32 COL_REGULAR       = IM_COL32(220, 220, 230, 255);
    constexpr ImU32 COL_JEWEL         = IM_COL32(80, 220, 220, 255);
    constexpr ImU32 COL_SELECTED_RING = IM_COL32(255, 255, 100, 255);
    constexpr ImU32 COL_HOVERED_RING  = IM_COL32(255, 255, 255, 220);

    // Search-based dim integration. When a search is active, non-matching
    // nodes and edges render at reduced alpha so matches stand out.
    auto isHashMatch = [this](uint32_t hash) -> bool {
        if (m_searchBuf[0] == '\0') return true;     // no search = all visible
        return m_searchMatchSet.find(hash) != m_searchMatchSet.end();
    };
    // Modulate the alpha channel of an ImU32 by a 0..1 multiplier.
    auto dimColor = [](ImU32 col, float alphaMul) -> ImU32 {
        if (alphaMul >= 0.999f) return col;
        int a = static_cast<int>(((col >> IM_COL32_A_SHIFT) & 0xFF) * alphaMul);
        if (a < 0) a = 0; else if (a > 255) a = 255;
        return (col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
    };

    dl->PushClipRect(canvasPos, ImVec2(canvasPos.x + canvasSz.x, canvasPos.y + canvasSz.y), true);

    // --- Draw edges ---
    // PSG edges are stored unidirectionally (A->B exists, B->A does not).
    // Both endpoints must pass the ascendancy filter for the edge to render.
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& group : m_psg.groups) {
            for (const auto& node : group.nodes) {
                bool nodeIsSelected = (node.hash == m_selectedNodeHash && m_selectedNodeHash != 0);
                if (pass == 0 && nodeIsSelected) continue;
                if (pass == 1 && !nodeIsSelected) continue;

                ImVec2 sp = WorldToScreen(node.worldX, node.worldY, canvasPos, canvasSz);

                for (const auto& edge : node.edges) {
                    if (!isEdgeShown(node.hash, edge.targetHash)) continue;

                    const Parsers::PsgNode* target = m_psg.FindNode(edge.targetHash);
                    if (!target) continue;

                    ImVec2 tp = WorldToScreen(target->worldX, target->worldY, canvasPos, canvasSz);

                    ImU32 edgeCol = nodeIsSelected ? COL_EDGE_SELECTED : COL_EDGE;
                    float thickness = nodeIsSelected ? edgeThicknessSelected : edgeThickness;

                    // Dim edges whose endpoints don't both match the search.
                    bool edgeMatches = isHashMatch(node.hash) || isHashMatch(edge.targetHash);
                    float edgeAlpha = (m_searchBuf[0] == '\0') ? 1.0f : (edgeMatches ? 0.7f : 0.1f);
                    dl->AddLine(sp, tp, dimColor(edgeCol, edgeAlpha), thickness);
                }
            }
        }
    }

    // --- Draw nodes + hit test ---
    // closestDistSq starts at +inf so the first node under the cursor wins;
    // later nodes only replace it if they are *closer* to the cursor. The
    // per-node hitSq check alone decides membership — no global minimum
    // radius that would break big-zoom picks.
    float closestDistSq = 1e30f;
    uint32_t closestHash = 0;
    ImVec2 mousePos = ImGui::GetIO().MousePos;

    for (size_t gi = 0; gi < m_psg.groups.size(); ++gi) {
        const auto& group = m_psg.groups[gi];
        for (const auto& node : group.nodes) {
            ImVec2 sp = WorldToScreen(node.worldX, node.worldY, canvasPos, canvasSz);

            const PassiveSkillInfo* skill = GetSkillInfo(node.hash);
            float baseR = baseRadiusFor(node, group.isJewel, skill);
            float radius = scaleRadius(baseR);

            // Off-screen early-out (use a margin slightly larger than the max
            // possible render radius so icons don't pop in/out at the edges).
            float margin = radius + 4.0f;
            if (sp.x < canvasPos.x - margin || sp.x > canvasPos.x + canvasSz.x + margin ||
                sp.y < canvasPos.y - margin || sp.y > canvasPos.y + canvasSz.y + margin) {
                continue;
            }

            bool nodeIsRoot = isRoot(node.hash);
            bool nodeIsJewel = group.isJewel || (skill && skill->isJewelSocket);

            // Dim non-matching nodes when a search is active.
            bool nodeMatches = isHashMatch(node.hash);
            float alphaMul = nodeMatches ? 1.0f : 0.15f;

            // Resolve icon texture (if available).
            ImTextureID tex = ImTextureID{};
            if (skill && !skill->iconPath.empty())
                tex = m_icons.Get(skill->iconPath);

            if (tex != ImTextureID{}) {
                ImVec2 iconMin(sp.x - radius, sp.y - radius);
                ImVec2 iconMax(sp.x + radius, sp.y + radius);
                // Round the image corners all the way to the radius so the
                // icon appears inside a perfect circle.
                dl->AddImageRounded(tex, iconMin, iconMax,
                                    ImVec2(0, 0), ImVec2(1, 1),
                                    dimColor(IM_COL32_WHITE, alphaMul),
                                    radius,
                                    ImDrawFlags_RoundCornersAll);
                // Thin outline circle around the icon.
                ImU32 outlineCol = IM_COL32(40, 40, 50, 220);
                if (nodeIsRoot)                        outlineCol = IM_COL32(255, 200, 50, 255);
                else if (skill && skill->isKeystone)   outlineCol = IM_COL32(230, 100, 90, 255);
                else if (skill && skill->isNotable)    outlineCol = IM_COL32(80, 140, 255, 255);
                else if (nodeIsJewel)                  outlineCol = IM_COL32(80, 220, 220, 255);
                float outlineThick = (std::max)(1.0f, (std::min)(3.0f, radius * 0.08f));
                dl->AddCircle(sp, radius, dimColor(outlineCol, alphaMul), 0, outlineThick);
            } else {
                // Fallback: colored circle/diamond based on type.
                ImU32 fillColor = COL_REGULAR;
                if (nodeIsRoot)                           fillColor = COL_ROOT;
                else if (skill && skill->isKeystone)      fillColor = COL_KEYSTONE;
                else if (skill && skill->isNotable)       fillColor = COL_NOTABLE;
                else if (nodeIsJewel)                     fillColor = COL_JEWEL;
                else if (node.orbit == 0)                 fillColor = COL_NOTABLE;

                fillColor = dimColor(fillColor, alphaMul);

                if (nodeIsJewel && !nodeIsRoot) {
                    float d = radius * 1.4f;
                    ImVec2 pts[4] = {
                        ImVec2(sp.x, sp.y - d),
                        ImVec2(sp.x + d, sp.y),
                        ImVec2(sp.x, sp.y + d),
                        ImVec2(sp.x - d, sp.y)
                    };
                    dl->AddConvexPolyFilled(pts, 4, fillColor);
                } else {
                    dl->AddCircleFilled(sp, radius, fillColor);
                }
            }

            if (node.hash == m_selectedNodeHash && m_selectedNodeHash != 0) {
                // Keep the selection ring fully visible so the current search
                // match pan target stays prominent even when dimmed.
                dl->AddCircle(sp, radius + 4.0f, COL_SELECTED_RING, 0, 2.0f);
            }

            // Hit test against the actual render radius so the tooltip
            // triggers anywhere inside the visible icon, regardless of zoom.
            // A tiny 3px floor keeps sub-pixel nodes at extreme zoom-out
            // still pickable, but otherwise the clickable area matches the
            // drawn circle exactly.
            if (canvasHovered) {
                float dx = sp.x - mousePos.x;
                float dy = sp.y - mousePos.y;
                float distSq = dx * dx + dy * dy;
                float hitR = (std::max)(radius, 3.0f);
                float hitSq = hitR * hitR;
                if (distSq <= hitSq && distSq < closestDistSq) {
                    closestDistSq = distSq;
                    closestHash = node.hash;
                }
            }
        }
    }

    m_hoveredNodeHash = closestHash;

    // Hovered outline
    if (m_hoveredNodeHash != 0 && m_hoveredNodeHash != m_selectedNodeHash) {
        const Parsers::PsgNode* hovNode = m_psg.FindNode(m_hoveredNodeHash);
        if (hovNode) {
            ImVec2 hp = WorldToScreen(hovNode->worldX, hovNode->worldY, canvasPos, canvasSz);
            auto idxIt = m_psg.nodeIndex.find(m_hoveredNodeHash);
            bool hovJewel = (idxIt != m_psg.nodeIndex.end()) && m_psg.groups[idxIt->second.first].isJewel;
            const PassiveSkillInfo* skill = GetSkillInfo(hovNode->hash);
            float hovBase = baseRadiusFor(*hovNode, hovJewel, skill);
            float hovRadius = scaleRadius(hovBase);
            dl->AddCircle(hp, hovRadius + 3.0f, COL_HOVERED_RING, 0, 1.5f);
        }
    }

    dl->PopClipRect();

    // --- Click selection ---
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_hoveredNodeHash != 0) {
        m_selectedNodeHash = m_hoveredNodeHash;
    }
    if (canvasHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && m_hoveredNodeHash == 0 && !m_dragging) {
        m_selectedNodeHash = 0;
    }

    // --- Hover tooltip (game-like style) ---
    if (m_hoveredNodeHash != 0 && canvasHovered) {
        const Parsers::PsgNode* hovNode = m_psg.FindNode(m_hoveredNodeHash);
        if (hovNode) {
            auto it = m_psg.nodeIndex.find(m_hoveredNodeHash);
            size_t groupIdx = (it != m_psg.nodeIndex.end()) ? it->second.first : 0;
            bool ctrlHeld = ImGui::GetIO().KeyCtrl;

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.08f, 0.08f, 0.12f, 0.96f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.35f, 0.5f, 0.8f));

            ImGui::BeginTooltip();

            bool nodeIsRoot = isRoot(hovNode->hash);
            const PassiveSkillInfo* skill = GetSkillInfo(hovNode->hash);

            // Title: node name (yellow)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
            if (skill && !skill->name.empty())
                ImGui::TextUnformatted(skill->name.c_str());
            else if (nodeIsRoot)
                ImGui::Text("Root Node 0x%X", hovNode->hash);
            else
                ImGui::Text("Node 0x%X", hovNode->hash);
            ImGui::PopStyleColor();

            // Formatted stats (blue, game-like) — format on demand per the
            // active language with English fallback per stat.
            if (skill && !skill->statRows.empty()) {
                const ImVec4 statColor(0.55f, 0.75f, 1.0f, 1.0f);
                const Parsers::CsdFormatMap* langMap = nullptr;
                const Parsers::CsdFormatMap* englishMap = nullptr;
                auto itLang = m_statFormatsByLang.find(m_currentLang);
                if (itLang != m_statFormatsByLang.end()) langMap = &itLang->second;
                auto itEn = m_statFormatsByLang.find("English");
                if (itEn != m_statFormatsByLang.end()) englishMap = &itEn->second;

                for (size_t i = 0; i < skill->statRows.size(); ++i) {
                    const int64_t statRow = skill->statRows[i];
                    auto idIt = m_statRowToId.find(statRow);
                    if (idIt == m_statRowToId.end()) continue;
                    const std::string& statId = idIt->second;

                    const std::string* fmtStr = nullptr;
                    if (langMap) {
                        auto it = langMap->find(statId);
                        if (it != langMap->end()) fmtStr = &it->second;
                    }
                    if (!fmtStr && englishMap && m_currentLang != "English") {
                        auto it = englishMap->find(statId);
                        if (it != englishMap->end()) fmtStr = &it->second;
                    }
                    if (!fmtStr) continue;

                    const int32_t value = (i < skill->statValues.size())
                                              ? skill->statValues[i] : 0;
                    const std::string line = FormatStat(*fmtStr, {value});
                    if (!line.empty()) {
                        ImGui::TextColored(statColor, "%s", line.c_str());
                    }
                }
            }

            // Flavour text (gray, after a separator)
            if (skill && !skill->flavourText.empty() && skill->flavourText != "null") {
                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.7f, 1.0f));
                ImGui::TextWrapped("%s", skill->flavourText.c_str());
                ImGui::PopStyleColor();
            }

            // --- Ctrl-held: extended details ---
            if (ctrlHeld) {
                ImGui::Separator();
                ImGui::TextDisabled("-- Ctrl: Details --");

                // Type badge
                const char* nodeType = "Small Passive";
                ImVec4 typeColor(0.7f, 0.8f, 0.9f, 1.0f);
                if (nodeIsRoot) { nodeType = "Class Start"; typeColor = ImVec4(1.0f, 0.7f, 0.2f, 1.0f); }
                else if (skill && skill->isKeystone) { nodeType = "Keystone"; typeColor = ImVec4(1.0f, 0.5f, 0.5f, 1.0f); }
                else if (skill && skill->isNotable) { nodeType = "Notable"; typeColor = ImVec4(0.5f, 0.9f, 0.5f, 1.0f); }
                else if (skill && skill->isJewelSocket) { nodeType = "Jewel Socket"; typeColor = ImVec4(0.3f, 0.9f, 0.9f, 1.0f); }
                else if (skill && skill->isAttribute) { nodeType = "Attribute"; }
                else if (skill && skill->isAscendancyStartingNode) { nodeType = "Ascendancy Start"; }
                ImGui::TextColored(typeColor, "Type: %s", nodeType);

                if (skill && !skill->id.empty())
                    ImGui::TextDisabled("ID: %s", skill->id.c_str());
                if (skill && !skill->iconPath.empty())
                    ImGui::TextDisabled("Icon: %s", skill->iconPath.c_str());

                ImGui::Text("Group: %u  |  Hash: 0x%X", static_cast<uint32_t>(groupIdx), hovNode->hash);
                ImGui::Text("Orbit: %u  |  Index: %u", hovNode->orbit, hovNode->orbitIndex);
                ImGui::Text("Position: (%.0f, %.0f)", hovNode->worldX, hovNode->worldY);

                if (skill) {
                    // Raw stat values + row references
                    if (!skill->statRows.empty()) {
                        ImGui::Separator();
                        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Stats (%u):",
                                           static_cast<uint32_t>(skill->statRows.size()));
                        for (size_t i = 0; i < skill->statRows.size(); ++i) {
                            int32_t val = (i < skill->statValues.size()) ? skill->statValues[i] : 0;
                            ImGui::Text("  [%zu] row=%lld  value=%d", i,
                                        static_cast<long long>(skill->statRows[i]), val);
                        }
                    }

                    ImGui::Separator();
                    ImGui::Text("SkillType: %d", skill->skillType);
                    ImGui::Text("SkillPointsGranted: %d", skill->skillPointsGranted);
                    if (skill->weaponPointsGranted != 0)
                        ImGui::Text("WeaponPointsGranted: %d", skill->weaponPointsGranted);
                    if (skill->ascendancy >= 0)
                        ImGui::Text("Ascendancy: %lld", static_cast<long long>(skill->ascendancy));
                    if (skill->masteryGroup >= 0)
                        ImGui::Text("MasteryGroup: %lld", static_cast<long long>(skill->masteryGroup));
                    if (skill->groupRow >= 0)
                        ImGui::Text("Group: %lld", static_cast<long long>(skill->groupRow));
                    if (skill->keystoneId >= 0)
                        ImGui::Text("KeystoneId: %lld", static_cast<long long>(skill->keystoneId));
                    if (skill->atlasSubTree >= 0)
                        ImGui::Text("AtlasSubTree: %lld", static_cast<long long>(skill->atlasSubTree));

                    // Boolean flags (compact)
                    std::string flags;
                    auto addFlag = [&](bool v, const char* name) {
                        if (!v) return;
                        if (!flags.empty()) flags += ", ";
                        flags += name;
                    };
                    addFlag(skill->isKeystone, "Keystone");
                    addFlag(skill->isNotable, "Notable");
                    addFlag(skill->isJewelSocket, "JewelSocket");
                    addFlag(skill->isAttribute, "Attribute");
                    addFlag(skill->isAscendancyStartingNode, "AscendancyStart");
                    addFlag(skill->isJustIcon, "JustIcon");
                    addFlag(skill->isMultipleChoice, "MultipleChoice");
                    addFlag(skill->isMultipleChoiceOption, "MCOption");
                    addFlag(skill->isExpansion, "Expansion");
                    addFlag(skill->isProxyPassive, "ProxyPassive");
                    addFlag(skill->isAnointmentOnly, "AnointmentOnly");
                    if (!flags.empty())
                        ImGui::TextWrapped("Flags: %s", flags.c_str());

                    if (!skill->characters.empty()) {
                        std::string chars;
                        for (auto c : skill->characters) {
                            if (!chars.empty()) chars += ", ";
                            chars += std::to_string(c);
                        }
                        ImGui::TextWrapped("Characters: %s", chars.c_str());
                    }
                }

                if (!hovNode->edges.empty()) {
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Connected (%u):",
                                       static_cast<uint32_t>(hovNode->edges.size()));
                    size_t edgeLimit = (std::min)(hovNode->edges.size(), size_t{12});
                    for (size_t i = 0; i < edgeLimit; ++i) {
                        uint32_t targetHash = hovNode->edges[i].targetHash;
                        const PassiveSkillInfo* targetSkill = GetSkillInfo(targetHash);
                        if (targetSkill && !targetSkill->name.empty())
                            ImGui::Text("  -> %s", targetSkill->name.c_str());
                        else
                            ImGui::Text("  -> 0x%X", targetHash);
                    }
                    if (hovNode->edges.size() > 12)
                        ImGui::TextDisabled("  ... +%u more",
                                            static_cast<uint32_t>(hovNode->edges.size() - 12));
                }
            } else {
                // Hint to user: Ctrl reveals more info
                if (skill) {
                    ImGui::Separator();
                    ImGui::TextDisabled("Hold Ctrl for details");
                }
            }

            if (!m_skillInfoLoaded) {
                ImGui::Separator();
                ImGui::TextDisabled("Skill names not loaded (no GGPK context)");
            }

            ImGui::EndTooltip();

            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
        }
    }

    ImGui::EndChild();
}

} // namespace GGPKExp
