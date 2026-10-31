// Plugins/GGPKExplorer/ui/UiText.h
// The explorer's chrome (GGPKExplorer.cpp and ui/) follows the program language (PluginI18n.h,
// included by PluginSDK.h): every text a user reads there is a key of lang/en.json (lang/ru.json,
// ... next to the DLL), drawn through I18n::Tr / TrF / TrId. The viewers (viewers/), the parsers
// (core/) and the file log stay English.
//
//   * Widgets whose label is their ImGui id take TrId ("<text>###<key>"); a label that needs its
//     own id appends it: LabelId(I18n::Tr("search_match_case"), "ff_match_case").
//   * A text is never a printf format: numbers go through Num into named TrF arguments.
//   * Nothing translated is stored: a state shown later (a status label, an error) keeps a LocText
//     (a key and its arguments) and turns it into text when drawn (TextOf), on the drawing thread.
//   * Layout is code: no pixel width sized for the English text (ButtonWidth, em-based sizes).
#pragma once
#include "../../../POEFixer/plugin_sdk/PluginSDK.h"
#include <imgui.h>

#include "UiTextCore.h"   // Num, Utf8Prefix, ShortenChars

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GGPKExp {

namespace I18n = PluginSDK::I18n;

// A text kept for later: a key (written I18n::Key("...")) and its {name} arguments.
struct LocText {
    const char* key = nullptr;
    std::vector<std::pair<std::string, std::string>> args;

    bool Empty() const { return !key || !*key; }
};

// The text of a LocText in the program language, its {name} placeholders filled.
inline std::string TextOf(const LocText& text) {
    std::string out;
    if (text.Empty()) return out;
    for (const char* p = I18n::Tr(text.key); *p;) {
        if (*p == '{') {
            if (const char* close = std::strchr(p + 1, '}')) {
                const std::string_view name(p + 1, static_cast<size_t>(close - p - 1));
                const std::string* value = nullptr;
                for (const auto& arg : text.args)
                    if (arg.first == name) {
                        value = &arg.second;
                        break;
                    }
                if (value) {
                    out += *value;
                    p = close + 1;
                    continue;
                }
            }
        }
        out.push_back(*p++);
    }
    return out;
}

// "<text>###<id>": a translated label whose ImGui id is `id` in every language.
inline std::string LabelId(const char* text, const char* id) {
    return std::string(text) + "###" + id;
}

// "<icon>  <label>": a menu label led by a FontAwesome glyph (the label may carry its ###id).
inline std::string WithIcon(const char* icon, const char* label) {
    return std::string(icon) + "  " + label;
}

// A byte count with its unit in the program language ("12.3 KB").
inline std::string SizeText(int64_t bytes) {
    if (bytes < 0) return "?";
    if (bytes < 1024)
        return I18n::TrF("size_value", { { "value", Num("%lld", static_cast<long long>(bytes)) },
                                         { "unit", I18n::Tr("unit_bytes") } });
    if (bytes < 1024 * 1024)
        return I18n::TrF("size_value", { { "value", Num("%.1f", static_cast<double>(bytes) / 1024.0) },
                                         { "unit", I18n::Tr("unit_kb") } });
    if (bytes < 1024LL * 1024 * 1024)
        return I18n::TrF("size_value", { { "value", Num("%.1f", static_cast<double>(bytes) / (1024.0 * 1024.0)) },
                                         { "unit", I18n::Tr("unit_mb") } });
    return I18n::TrF("size_value", { { "value", Num("%.2f", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) },
                                     { "unit", I18n::Tr("unit_gb") } });
}

// What a view that reads the DAT schema shows until one is loaded: why the last load failed
// (`loadError`, as the parsers word it), else that it waits for the game data the program may
// still be downloading. Wrapped at the window's edge.
inline void SchemaNotReadyLine(const std::string& loadError) {
    ImGui::PushTextWrapPos(0.0f);
    if (loadError.empty()) {
        ImGui::TextDisabled("%s", I18n::Tr("schema_waiting"));
    } else {
        const std::string text = I18n::TrF("schema_failed", { { "reason", loadError } });
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", text.c_str());   // as a file's load error
    }
    ImGui::PopTextWrapPos();
}

// A button width that fits its (translated) label and is at least `minEm` font sizes wide.
inline float ButtonWidth(const char* label, float minEm) {
    const float fit = ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float least = ImGui::GetFontSize() * minEm;
    return fit > least ? fit : least;
}

// UTF-8 to UTF-16 for the Win32 dialogs.
inline std::wstring Wide(const char* utf8) {
    if (!utf8 || !*utf8) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (needed <= 1) return {};
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), needed);
    return out;
}

// A Win32 file-dialog filter ("<label> (<pattern>)\0<pattern>\0...\0\0") with translated labels.
class DialogFilter {
public:
    DialogFilter& Add(const std::string& label, const std::wstring& pattern) {
        m_text += Wide(label.c_str());
        m_text += L" (";
        m_text += pattern;
        m_text += L')';
        m_text.push_back(L'\0');
        m_text += pattern;
        m_text.push_back(L'\0');
        return *this;
    }
    // Double-NUL terminated: the last entry's NUL plus the string's own terminator.
    const wchar_t* Get() const { return m_text.c_str(); }

private:
    std::wstring m_text;
};

} // namespace GGPKExp
