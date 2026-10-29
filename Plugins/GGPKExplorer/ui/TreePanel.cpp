// Plugins/GGPKExplorer/ui/TreePanel.cpp
#include "TreePanel.h"
#include "UiText.h"      // SizeText, translated labels
#include <imgui.h>
#include <cstdio>
#include <cctype>
#include <algorithm>

// Bookmark icon set — 10 FontAwesome icons
static constexpr int kBookmarkIconCount = 10;
static const char* kBookmarkIcons[kBookmarkIconCount] = {
    "\xef\x80\x85",  // 0: star     U+F005
    "\xef\x80\x84",  // 1: heart    U+F004
    "\xef\x80\xa4",  // 2: flag     U+F024
    "\xef\x83\xa7",  // 3: bolt     U+F0E7
    "\xef\x81\xad",  // 4: fire     U+F06D
    "\xef\x8e\xa5",  // 5: gem      U+F3A5
    "\xef\x84\x91",  // 6: circle   U+F111
    "\xef\x80\xae",  // 7: bookmark U+F02E
    "\xef\x83\xb3",  // 8: bell     U+F0F3
    "\xef\x81\xae",  // 9: eye      U+F06E
};

static const char* GetBookmarkIconStr(int index) {
    if (index >= 0 && index < kBookmarkIconCount)
        return kBookmarkIcons[index];
    return "";
}

// A dimmed note that wraps inside the tree column: the column can be narrow, and a
// translation is often longer than the English text.
static void DisabledWrapped(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

namespace GGPKExp {

// ---------------------------------------------------------------------------
// Per-extension icon + color overrides
// ---------------------------------------------------------------------------

struct ExtensionAppearance {
    const char* icon;       // UTF-8 FontAwesome escape with trailing space, or nullptr
    ImVec4 color;           // alpha == 0 means use default
};

static ExtensionAppearance GetExtensionAppearance(const std::string& name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos) return {nullptr, ImVec4(0,0,0,0)};
    std::string ext = name.substr(dot);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == ".aoc")  return {"\xef\x84\xae ", ImVec4(0.95f, 0.55f, 0.75f, 1.0f)};  // U+F12E puzzle-piece
    if (ext == ".psg")  return {"\xef\x83\xa8 ", ImVec4(0.70f, 0.85f, 0.55f, 1.0f)};  // U+F0E8 sitemap
    if (ext == ".sm")   return {"\xef\x86\xb2 ", ImVec4(0.75f, 0.65f, 0.95f, 1.0f)};  // U+F1B2 cube
    if (ext == ".smd")  return {"\xef\x86\xb2 ", ImVec4(0,0,0,0)};                    // U+F1B2 cube
    if (ext == ".ast")  return {"\xef\x97\x97 ", ImVec4(0,0,0,0)};                    // U+F5D7 bone
    if (ext == ".mtp")  return {"\xef\x89\xb9 ", ImVec4(0,0,0,0)};                    // U+F279 map
    if (ext == ".tdt")  return {"\xef\x80\x8a ", ImVec4(0,0,0,0)};                    // U+F00A th (grid)
    return {nullptr, ImVec4(0,0,0,0)};
}

// ---------------------------------------------------------------------------
// Icon prefix based on file type
// ---------------------------------------------------------------------------

static const char* GetFileTypeIcon(FileType type) {
    switch (type) {
    case FileType::Text:      return "\xef\x85\x9c ";  // fa-file-alt U+F15C
    case FileType::Image:     return "\xef\x87\x85 ";  // fa-file-image U+F1C5
    case FileType::Dat:       return "\xef\x83\x8e ";  // fa-table U+F0CE
    case FileType::Audio:     return "\xef\x87\x87 ";  // fa-file-audio U+F1C7
    case FileType::Video:     return "\xef\x87\x88 ";  // fa-file-video U+F1C8
    case FileType::Shader:    return "\xef\x80\x93 ";  // fa-cog U+F013
    case FileType::Binary:    return "\xef\x85\x9b ";  // fa-file U+F15B
    case FileType::Directory: return "";
    default:                  return "\xef\x85\x9b ";  // fa-file U+F15B
    }
}

// ---------------------------------------------------------------------------
// Selection helpers
// ---------------------------------------------------------------------------

void TreePanel::ClearSelection() {
    m_selectedIndices.clear();
    m_selectedPath.clear();
}

std::vector<std::string> TreePanel::GetSelectedPaths() const {
    std::vector<std::string> paths;
    if (!m_tree) return paths;
    const auto& flatList = m_tree->GetFlatList();
    for (int idx : m_selectedIndices) {
        if (idx >= 0 && idx < static_cast<int>(flatList.size()))
            paths.push_back(flatList[static_cast<uint32_t>(idx)].fullPath);
    }
    return paths;
}

std::vector<std::string> TreePanel::GetSelectedDirPaths() const {
    std::vector<std::string> paths;
    if (!m_tree) return paths;
    const auto& flatList = m_tree->GetFlatList();
    for (int idx : m_selectedIndices) {
        if (idx >= 0 && idx < static_cast<int>(flatList.size())) {
            const auto& entry = flatList[static_cast<uint32_t>(idx)];
            if (entry.isDirectory)
                paths.push_back(entry.fullPath);
        }
    }
    return paths;
}

bool TreePanel::HasAnyDirSelected() const {
    if (!m_tree) return false;
    const auto& flatList = m_tree->GetFlatList();
    for (int idx : m_selectedIndices) {
        if (idx >= 0 && idx < static_cast<int>(flatList.size()) &&
            flatList[static_cast<uint32_t>(idx)].isDirectory)
            return true;
    }
    return false;
}

bool TreePanel::HasAnyFileSelected() const {
    if (!m_tree) return false;
    const auto& flatList = m_tree->GetFlatList();
    for (int idx : m_selectedIndices) {
        if (idx >= 0 && idx < static_cast<int>(flatList.size()) &&
            !flatList[static_cast<uint32_t>(idx)].isDirectory)
            return true;
    }
    return false;
}

void TreePanel::SetSelectedPath(const std::string& path) {
    m_selectedPath = path;
    m_selectedIndices.clear();
    if (m_tree) {
        uint32_t idx = m_tree->FindByPath(path);
        if (idx != UINT32_MAX) {
            m_selectedIndices.insert(static_cast<int>(idx));
            m_lastClickedIndex = static_cast<int>(idx);
        }
    }
}

void TreePanel::RevealSelected() {
    if (!m_tree || m_selectedPath.empty()) return;
    uint32_t idx = m_tree->FindByPath(m_selectedPath);
    if (idx == UINT32_MAX) return;
    m_pendingRevealRow = static_cast<int>(idx);
    m_highlightRow     = static_cast<int>(idx);
    m_highlightEndTime = ImGui::GetTime() + 1.5;  // 1.5s visible fade
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void TreePanel::Render(float width) {
    if (!m_tree) {
        ImGui::BeginChild("##GGPKTree", ImVec2(width, 0), ImGuiChildFlags_Borders);
        DisabledWrapped(I18n::Tr("tree_no_data"));
        ImGui::EndChild();
        return;
    }

    const auto& flatList = m_tree->GetFlatList();
    int itemCount = static_cast<int>(flatList.size());

    ImGui::BeginChild("##GGPKTree", ImVec2(width, 0), ImGuiChildFlags_Borders);

    if (itemCount == 0) {
        DisabledWrapped(I18n::Tr("tree_empty"));
        ImGui::EndChild();
        return;
    }

    ImGuiListClipper clipper;
    clipper.Begin(itemCount);

    // Force the clipper to render our reveal target even when it's offscreen,
    // otherwise SetScrollHereY has nothing to anchor to.
    if (m_pendingRevealRow >= 0 && m_pendingRevealRow < itemCount) {
        clipper.IncludeItemByIndex(m_pendingRevealRow);
    }

    bool openContextMenu = false;
    auto& io = ImGui::GetIO();

    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            uint32_t idx = static_cast<uint32_t>(row);
            const auto& entry = flatList[idx];

            ImGui::PushID(row);

            if (row == m_pendingRevealRow) {
                ImGui::SetScrollHereY(0.5f);
                m_pendingRevealRow = -1;  // one-shot
            }

            bool isSelected = m_selectedIndices.count(row) > 0;

            // Full-width Selectable for hit testing (covers the entire row).
            float indent = entry.depth * 16.0f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);

            // Build display text for the Selectable label
            char label[512];
            const char* bmPrefix = "";
            char bmBuf[16] = {};
            if (entry.isBookmarked && entry.bookmarkIcon >= 0) {
                snprintf(bmBuf, sizeof(bmBuf), "%s ", GetBookmarkIconStr(entry.bookmarkIcon));
                bmPrefix = bmBuf;
            }

            ExtensionAppearance extApp{nullptr, ImVec4(0,0,0,0)};
            if (!entry.isDirectory)
                extApp = GetExtensionAppearance(entry.name);

            if (entry.isDirectory) {
                const char* icon = entry.isExpanded ? "\xef\x81\xbc " : "\xef\x81\xbb ";
                if (entry.childCount > 0)
                    snprintf(label, sizeof(label), "%s%s%s (%u)", bmPrefix, icon, entry.name.c_str(), entry.childCount);
                else
                    snprintf(label, sizeof(label), "%s%s%s", bmPrefix, icon, entry.name.c_str());
            } else {
                const char* icon = extApp.icon ? extApp.icon : GetFileTypeIcon(entry.type);
                if (entry.fileSize >= 0) {
                    const std::string sizeText = SizeText(entry.fileSize);
                    snprintf(label, sizeof(label), "%s%s%s (%s)", bmPrefix, icon, entry.name.c_str(), sizeText.c_str());
                } else {
                    snprintf(label, sizeof(label), "%s%s%s", bmPrefix, icon, entry.name.c_str());
                }
            }

            // Color the text: directories blue-ish, selected items yellow,
            // per-extension overrides for certain file types.
            ImVec4 textColor;
            if (isSelected) {
                textColor = ImVec4(1.0f, 1.0f, 0.4f, 1.0f);
            } else if (entry.isDirectory) {
                textColor = ImVec4(0.7f, 0.85f, 1.0f, 1.0f);
            } else if (extApp.color.w > 0.0f) {
                textColor = extApp.color;
            } else {
                textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, textColor);

            bool clicked = ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_None);
            ImGui::PopStyleColor();

            // ----- Flash highlight after RevealSelected() -----
            if (row == m_highlightRow && ImGui::GetTime() < m_highlightEndTime) {
                double remaining = m_highlightEndTime - ImGui::GetTime();
                float  alpha = static_cast<float>(remaining / 1.5) * 0.55f;  // fade 0.55 -> 0
                if (alpha < 0.0f) alpha = 0.0f;
                ImVec2 rmin = ImGui::GetItemRectMin();
                ImVec2 rmax = ImGui::GetItemRectMax();
                ImU32  col  = ImGui::GetColorU32(ImVec4(1.0f, 0.75f, 0.15f, alpha));
                ImGui::GetWindowDrawList()->AddRectFilled(rmin, rmax, col, 2.0f);
            }

            // ----- Click handling: multi-select logic -----
            if (clicked) {
                bool ctrl = io.KeyCtrl;
                bool shift = io.KeyShift;

                if (ctrl && shift) {
                    // Ctrl+Shift: ADD range to selection (don't clear)
                    if (m_lastClickedIndex >= 0) {
                        int lo = std::min(m_lastClickedIndex, row);
                        int hi = std::max(m_lastClickedIndex, row);
                        for (int i = lo; i <= hi; ++i)
                            m_selectedIndices.insert(i);
                    } else {
                        m_selectedIndices.insert(row);
                    }
                    // Don't update m_lastClickedIndex for Ctrl+Shift
                } else if (shift) {
                    // Shift: SELECT range from anchor to clicked (clear first)
                    m_selectedIndices.clear();
                    int anchor = (m_lastClickedIndex >= 0) ? m_lastClickedIndex : row;
                    int lo = std::min(anchor, row);
                    int hi = std::max(anchor, row);
                    for (int i = lo; i <= hi; ++i)
                        m_selectedIndices.insert(i);
                    // Don't update m_lastClickedIndex for Shift
                } else if (ctrl) {
                    // Ctrl: TOGGLE single item (don't expand folders)
                    if (m_selectedIndices.count(row))
                        m_selectedIndices.erase(row);
                    else
                        m_selectedIndices.insert(row);
                    m_lastClickedIndex = row;
                } else {
                    // Plain click: clear all, select one, expand/collapse dirs
                    m_selectedIndices.clear();
                    m_selectedIndices.insert(row);
                    m_lastClickedIndex = row;

                    if (entry.isDirectory) {
                        m_tree->ToggleExpand(idx);
                    }
                }

                // Update primary selected path (last clicked for callbacks)
                m_selectedPath = entry.fullPath;
                if (m_onSelect) {
                    m_onSelect(entry);
                }
            }

            // ----- Right click -----
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                // If right-clicked item is NOT in selection, reset to just this item
                if (m_selectedIndices.count(row) == 0) {
                    m_selectedIndices.clear();
                    m_selectedIndices.insert(row);
                    m_lastClickedIndex = row;
                    m_selectedPath = entry.fullPath;
                    if (m_onSelect) {
                        m_onSelect(entry);
                    }
                }
                m_contextMenuIndex = idx;
                openContextMenu = true;
            }

            ImGui::PopID();
        }
    }

    clipper.End();

    // Open popup OUTSIDE PushID so ID stack matches BeginPopup in RenderContextMenu
    if (openContextMenu) {
        ImGui::OpenPopup("##GGPKTreeCtx");
    }

    // Context menu popup
    RenderContextMenu();

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// RenderContextMenu
// ---------------------------------------------------------------------------

void TreePanel::RenderContextMenu() {
    if (!ImGui::BeginPopup("##GGPKTreeCtx"))
        return;

    if (!m_tree || m_contextMenuIndex >= static_cast<uint32_t>(m_tree->GetFlatList().size())) {
        ImGui::EndPopup();
        return;
    }

    bool hasDirs = HasAnyDirSelected();
    bool hasFiles = HasAnyFileSelected();
    bool multiSel = HasMultipleSelected();

    const auto& entry = m_tree->GetFlatList()[m_contextMenuIndex];

    // --- Search item (always first) ---
    if (hasDirs || (hasDirs && hasFiles)) {
        // Folders selected (or mixed)
        if (ImGui::MenuItem(WithIcon("\xef\x80\x82", I18n::TrId("tree_search_in_selected")).c_str())) {
            wantsSearch = true;
            wantsSearchFilesTab = false;
            ImGui::CloseCurrentPopup();
        }
    } else if (hasFiles && !hasDirs) {
        // Files only
        if (ImGui::MenuItem(WithIcon("\xef\x80\x82", I18n::TrId("tree_find_replace_in_selected")).c_str())) {
            wantsSearch = true;
            wantsSearchFilesTab = true;
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::Separator();

    // --- Directory-specific items (only when single dir selected) ---
    if (entry.isDirectory && !multiSel) {
        if (ImGui::MenuItem(WithIcon("\xef\x95\xae", I18n::TrId("tree_extract_directory")).c_str())) {
            wantsExtractDir = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(WithIcon("\xef\x81\xbc", I18n::TrId("tree_expand_all")).c_str())) {
            wantsExpandAll = true;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::MenuItem(WithIcon("\xef\x81\xbb", I18n::TrId("tree_collapse_all")).c_str())) {
            wantsCollapseAll = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
    }

    // --- File-specific items (only when single file selected) ---
    if (!entry.isDirectory && !multiSel) {
        if (ImGui::MenuItem(WithIcon("\xef\x95\xae", I18n::TrId("tree_extract_file")).c_str())) {
            wantsExtractFile = true;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::MenuItem(WithIcon("\xef\x95\xaf", I18n::TrId("tree_replace_file")).c_str())) {
            wantsReplaceFile = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
    }

    // --- Multi-select extract (2+ selected, files and/or directories) ---
    if (multiSel && (hasFiles || hasDirs)) {
        if (ImGui::MenuItem(WithIcon("\xef\x95\xae", I18n::TrId("tree_extract_selected")).c_str())) {
            wantsExtractSelectedFiles = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
    }

    // --- Save As (convert): .datc64 -> JSON/CSV, .dds -> DDS/PNG ---
    {
        bool sawDir = false, sawDatc64 = false, sawDds = false;
        const auto& flatList = m_tree->GetFlatList();
        auto endsWithCI = [](const std::string& s, const char* suf) {
            size_t n = 0; while (suf[n]) ++n;
            if (s.size() < n) return false;
            for (size_t i = 0; i < n; ++i) {
                char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[s.size() - n + i])));
                if (a != suf[i]) return false;
            }
            return true;
        };
        for (int idx : m_selectedIndices) {
            if (idx < 0 || idx >= static_cast<int>(flatList.size())) continue;
            const auto& e = flatList[static_cast<uint32_t>(idx)];
            if (e.isDirectory) sawDir = true;
            else if (endsWithCI(e.name, ".datc64")) sawDatc64 = true;
            else if (endsWithCI(e.name, ".dds")) sawDds = true;
        }
        const bool showJsonCsv = sawDir || sawDatc64;
        const bool showDdsPng  = sawDir || sawDds;
        if (showJsonCsv || showDdsPng) {
            if (ImGui::BeginMenu(WithIcon("\xef\x83\x87", I18n::TrId("tree_save_as")).c_str())) {  // FA floppy-disk (U+F0C7)
                if (showJsonCsv) {
                    if (ImGui::MenuItem("JSON")) { wantsSaveAs = true; saveAsFormat = SaveFormat::Json; ImGui::CloseCurrentPopup(); }
                    if (ImGui::MenuItem("CSV"))  { wantsSaveAs = true; saveAsFormat = SaveFormat::Csv;  ImGui::CloseCurrentPopup(); }
                }
                if (showDdsPng) {
                    if (ImGui::MenuItem("DDS"))  { wantsSaveAs = true; saveAsFormat = SaveFormat::Dds;  ImGui::CloseCurrentPopup(); }
                    if (ImGui::MenuItem("PNG"))  { wantsSaveAs = true; saveAsFormat = SaveFormat::Png;  ImGui::CloseCurrentPopup(); }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
        }
    }

    // --- Copy items (single selection only) ---
    if (!multiSel) {
        if (ImGui::MenuItem(WithIcon("\xef\x83\x85", I18n::TrId("tree_copy_path")).c_str())) {
            wantsCopyPath = true;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::MenuItem(WithIcon("\xef\x83\x85", I18n::TrId("tree_copy_name")).c_str())) {
            wantsCopyName = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
    }

    // Check if ANY selected item is bookmarked (for showing Remove option)
    bool anyBookmarked = false;
    bool anyNotBookmarked = false;
    {
        const auto& flatList = m_tree->GetFlatList();
        for (int idx : m_selectedIndices) {
            if (idx >= 0 && idx < static_cast<int>(flatList.size())) {
                if (flatList[static_cast<uint32_t>(idx)].isBookmarked)
                    anyBookmarked = true;
                else
                    anyNotBookmarked = true;
            }
        }
    }

    if (anyNotBookmarked || !anyBookmarked) {
        if (ImGui::BeginMenu(WithIcon("\xef\x80\x85", I18n::TrId("tree_add_bookmark")).c_str())) {
            // 5x2 icon grid with vertically centered icons
            const ImVec2 btnSize(24, 24);
            for (int i = 0; i < kBookmarkIconCount; ++i) {
                ImGui::PushID(i);
                ImVec2 pos = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##bm", false, ImGuiSelectableFlags_None, btnSize)) {
                    bookmarkIconChoice = i;
                    wantsAddBookmark = true;
                    ImGui::CloseCurrentPopup();
                }
                // Draw icon centered over the selectable
                ImVec2 textSize = ImGui::CalcTextSize(kBookmarkIcons[i]);
                float offsetX = (btnSize.x - textSize.x) * 0.5f;
                float offsetY = (btnSize.y - textSize.y) * 0.5f;
                ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + offsetX, pos.y + offsetY),
                    ImGui::GetColorU32(ImGuiCol_Text), kBookmarkIcons[i]);
                ImGui::PopID();
                if ((i % 5) != 4) ImGui::SameLine();
            }
            ImGui::EndMenu();
        }
    }

    if (anyBookmarked) {
        if (ImGui::MenuItem(WithIcon("\xef\x80\x85", I18n::TrId("tree_remove_bookmark")).c_str())) {
            wantsRemoveBookmark = true;
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::EndPopup();
}

} // namespace GGPKExp
