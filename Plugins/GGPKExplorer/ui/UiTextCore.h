// Plugins/GGPKExplorer/ui/UiTextCore.h
// The parts of UiText.h that need no ImGui and no PluginSDK.h: numbers for TrF arguments and
// cutting a text by characters. tests/UiLogicTests.cpp checks them on their own.
#pragma once
#include <sal.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace GGPKExp {

// printf for a TrF argument: Num("%d", count). The format is code, never a translation; a
// format the C library refuses (an encoding error) gives "", never an unterminated buffer.
inline std::string Num(_In_z_ _Printf_format_string_ const char* format, ...) {
    char buf[64];
    buf[0] = '\0';
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (n < 0) return std::string();
    return std::string(buf);
}

// The first `maxChars` characters (UTF-8 code points) of `text`, never part of one. *cut (when
// given) tells whether anything was left out. A localized or game text is cut this way, never
// by bytes: a byte count splits a Russian or Thai character.
inline std::string_view Utf8Prefix(std::string_view text, size_t maxChars, bool* cut = nullptr) {
    size_t chars = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const bool starts = (static_cast<unsigned char>(text[i]) & 0xC0) != 0x80;
        if (!starts) continue;
        if (chars == maxChars) {
            if (cut) *cut = true;
            return text.substr(0, i);
        }
        ++chars;
    }
    if (cut) *cut = false;
    return text;
}

// `text` cut to `maxChars` characters, "..." added when anything was left out.
inline std::string ShortenChars(std::string_view text, size_t maxChars) {
    bool cut = false;
    const std::string_view kept = Utf8Prefix(text, maxChars, &cut);
    std::string out(kept);
    if (cut) out += "...";
    return out;
}

} // namespace GGPKExp
