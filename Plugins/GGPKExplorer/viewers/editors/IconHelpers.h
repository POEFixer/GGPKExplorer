#pragma once
// -----------------------------------------------------------------------------
// Phase 7 / T22 — FA6 icon label helper.
//
// Centralises the "icon glyph prefix + label" string construction so every
// DatViewer button/menu item uses a single path. When the host atlas does not
// have FontAwesome6 merged (probed once in GGPKExplorer::RenderMainContent),
// IconLabel() degrades to the plain label.
//
// Usage:
//   auto lbl = IconLabel(fa6Available, kIconSave, "Save Changes");
//   if (ImGui::Button(lbl.c_str())) { ... }
//
// NOTE: IconLabel returns std::string by value — store it in a local before
// calling .c_str() so the buffer outlives the ImGui call.
// -----------------------------------------------------------------------------
#include "../../../../POEFixer/imgui/IconsFontAwesome6.h"
#include <string>

namespace GGPKExp {

// Prepends icon glyph + space to the label when FA6 is available; otherwise
// returns just the label (no icon, no leading space).
inline std::string IconLabel(bool fa6Available, const char* icon, const char* label) {
    if (!label) label = "";
    if (fa6Available && icon && icon[0])
        return std::string(icon) + " " + label;
    return std::string(label);
}

// Commonly used icon constants. Prefer classic (pre-FA6) codepoints guaranteed
// to be present in any FA6 Free Solid font build. FA6-new codepoints (e.g.
// file-arrow-up/down 0xf574/0xf56d, magnifying-glass-location 0xf689) may be
// absent from older fa-solid-900.ttf builds even when FA_PEN (the probe) exists.
constexpr const char* kIconImport    = ICON_FA_UPLOAD;                     // U+f093 (classic)
constexpr const char* kIconExport    = ICON_FA_DOWNLOAD;                   // U+f019 (classic)
constexpr const char* kIconSave      = ICON_FA_FLOPPY_DISK;                // U+f0c7
constexpr const char* kIconEdit      = ICON_FA_PEN;                        // U+f304
constexpr const char* kIconCopy      = ICON_FA_COPY;                       // U+f0c5
constexpr const char* kIconPaste     = ICON_FA_PASTE;                      // U+f0ea
constexpr const char* kIconClear     = ICON_FA_ERASER;                     // U+f12d
constexpr const char* kIconNavigate  = ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE; // U+f08e
constexpr const char* kIconRevert    = ICON_FA_ROTATE_LEFT;                // U+f2ea
constexpr const char* kIconRowJson   = ICON_FA_CODE;                       // U+f121
constexpr const char* kIconFindRef   = ICON_FA_MAGNIFYING_GLASS;           // U+f002 (safer than _LOCATION variant)
constexpr const char* kIconClose     = ICON_FA_XMARK;                      // U+f00d
constexpr const char* kIconExpand    = ICON_FA_CARET_DOWN;                 // U+f0d7
constexpr const char* kIconCollapse  = ICON_FA_CARET_RIGHT;                // U+f0da
constexpr const char* kIconBrowse    = ICON_FA_FOLDER_OPEN;                // U+f07c
constexpr const char* kIconAdd       = ICON_FA_PLUS;                       // U+002b
constexpr const char* kIconDelete    = ICON_FA_TRASH;                      // U+f1f8
constexpr const char* kIconWarn      = ICON_FA_TRIANGLE_EXCLAMATION;       // U+f071
constexpr const char* kIconOk        = ICON_FA_CHECK;                      // U+f00c
constexpr const char* kIconSearch    = ICON_FA_MAGNIFYING_GLASS;           // U+f002

} // namespace GGPKExp
