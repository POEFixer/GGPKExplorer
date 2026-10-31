#include "CsdParser.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>

namespace GGPKExp { namespace Parsers {

namespace {

// Decode UTF-16LE bytes to UTF-8. Handles the common BOM (0xFF 0xFE) and
// assumes UTF-16LE when no BOM is present but every second byte is zero for
// a reasonable prefix (typical for ASCII-heavy .csd files).
std::string Utf16LeToUtf8(const uint8_t* data, size_t size) {
    size_t start = 0;
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE)
        start = 2;

    std::string out;
    out.reserve(size / 2);
    for (size_t i = start; i + 1 < size; i += 2) {
        uint32_t cp = static_cast<uint32_t>(data[i]) | (static_cast<uint32_t>(data[i + 1]) << 8);
        // Handle surrogate pair
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < size) {
            uint32_t low = static_cast<uint32_t>(data[i + 2]) | (static_cast<uint32_t>(data[i + 3]) << 8);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// Heuristic: detect UTF-16LE by checking BOM or presence of zero bytes at
// odd positions in the first chunk.
bool LooksLikeUtf16Le(const uint8_t* data, size_t size) {
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) return true;
    // Scan first 64 bytes for the typical UTF-16LE pattern (even-index bytes
    // are ASCII, odd-index bytes are zero).
    size_t check = size < 64 ? size : 64;
    if (check < 4) return false;
    size_t zeroOdd = 0;
    for (size_t i = 1; i < check; i += 2) {
        if (data[i] == 0) ++zeroOdd;
    }
    return zeroOdd * 2 > check / 2;
}

std::string_view Trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

bool StartsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && std::memcmp(s.data(), prefix.data(), prefix.size()) == 0;
}

// Extract the first quoted segment ("...") from `s`. Returns empty on failure.
std::string ExtractQuoted(std::string_view s) {
    size_t a = s.find('"');
    if (a == std::string::npos) return {};
    size_t b = s.find('"', a + 1);
    if (b == std::string::npos) return {};
    return std::string(s.substr(a + 1, b - a - 1));
}

// Split `s` by whitespace into tokens.
std::vector<std::string> SplitWs(std::string_view s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
        if (i > start) out.emplace_back(s.substr(start, i - start));
    }
    return out;
}

// Line-based parse over the decoded UTF-8 text. Grammar:
//   description
//     <N> <stat_id_1> [... <stat_id_N>]      // first line after description
//     <variant_count>                         // starts default English block
//     <condition?> # "<format>"               // capture into out["English"]
//     lang "X"                                // switch target to out["X"]
//     <variant_count>
//     <condition?> # "<format>"               // capture into out["X"]
//     ...                                     // repeat per language
//   description                               // next block
//     ...
// We capture only the FIRST format line per (stat_id, language) pair; this
// intentionally ignores multi-variant conditional formats (pre-existing
// lossy behavior — out of scope for this change).
void ParseDecodedCsdMultiLang(const std::string& text,
                              CsdMultiLangFormatMap& out,
                              std::vector<std::string>& outIncludes) {
    std::istringstream iss(text);
    std::string line;

    enum class Phase { Idle, AfterHeader, AfterVariantCount };
    Phase phase = Phase::Idle;
    std::vector<std::string> pendingIds;
    bool capturedFormat = false;
    std::string activeLang = "English";   // default target before any `lang`

    auto resetDescription = [&]() {
        phase = Phase::Idle;
        pendingIds.clear();
        capturedFormat = false;
        activeLang = "English";
    };

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string_view sv(line);
        std::string_view trimmed = Trim(sv);
        if (trimmed.empty()) continue;

        // Top-level directives terminate any in-progress description.
        if (StartsWith(trimmed, "include ") || StartsWith(trimmed, "include\"")) {
            auto path = ExtractQuoted(trimmed);
            if (!path.empty()) outIncludes.push_back(std::move(path));
            resetDescription();
            continue;
        }
        if (trimmed == "description") {
            resetDescription();
            phase = Phase::AfterHeader;
            continue;
        }
        if (StartsWith(trimmed, "no_description")) {
            resetDescription();
            continue;
        }

        if (phase == Phase::Idle) continue;

        if (StartsWith(trimmed, "lang ")) {
            // Switch active language and reset per-language capture state.
            // Keep pendingIds (same stat_ids apply to every language in the
            // block). Stay in AfterVariantCount phase so the next bare-int
            // line is still treated as the new language's variant count.
            std::string newLang = ExtractQuoted(trimmed);
            if (!newLang.empty()) {
                activeLang = std::move(newLang);
            }
            capturedFormat = false;
            // Phase: if we hadn't yet entered AfterVariantCount (e.g.,
            // malformed file), keep whatever state we were in — the variant
            // count/format handler below only runs in AfterVariantCount.
            continue;
        }

        if (phase == Phase::AfterHeader) {
            auto parts = SplitWs(trimmed);
            if (parts.empty()) continue;
            int count = 0;
            try { count = std::stoi(parts[0]); } catch (...) { resetDescription(); continue; }
            if (count <= 0 || count > 32 || static_cast<int>(parts.size()) < count + 1) {
                resetDescription();
                continue;
            }
            for (int i = 0; i < count; ++i)
                pendingIds.push_back(parts[static_cast<size_t>(i + 1)]);
            phase = Phase::AfterVariantCount;
            continue;
        }

        if (phase == Phase::AfterVariantCount) {
            // Variant count line is a bare integer. Skip and wait for format.
            bool allDigits = !trimmed.empty();
            for (char c : trimmed) if (!std::isdigit(static_cast<unsigned char>(c))) { allDigits = false; break; }
            if (allDigits) continue;

            if (capturedFormat) continue;

            size_t hash = trimmed.find('#');
            if (hash == std::string::npos) continue;
            std::string_view rest = trimmed.substr(hash);
            auto fmt = ExtractQuoted(rest);
            if (fmt.empty()) continue;

            CsdFormatMap& langMap = out[activeLang];
            for (const auto& id : pendingIds) {
                // Later files merged into the same map override earlier ones
                // (operator[] assignment).
                langMap[id] = fmt;
            }
            capturedFormat = true;
        }
    }
}

} // namespace

void ParseCsdMultiLang(const uint8_t* data,
                       size_t size,
                       CsdMultiLangFormatMap& out,
                       std::vector<std::string>& outIncludes) {
    if (!data || size == 0) return;

    std::string text;
    if (LooksLikeUtf16Le(data, size)) {
        text = Utf16LeToUtf8(data, size);
    } else {
        size_t start = 0;
        if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) start = 3;
        text.assign(reinterpret_cast<const char*>(data + start), size - start);
    }
    ParseDecodedCsdMultiLang(text, out, outIncludes);
}

void ParseCsd(const uint8_t* data,
              size_t size,
              CsdFormatMap& out,
              std::vector<std::string>& outIncludes) {
    // Back-compat wrapper: returns only the English default block.
    CsdMultiLangFormatMap multi;
    ParseCsdMultiLang(data, size, multi, outIncludes);
    auto it = multi.find("English");
    if (it == multi.end()) return;
    // Merge into caller's out (preserves accumulate-across-files semantics).
    for (auto& [id, fmt] : it->second) {
        out[id] = std::move(fmt);
    }
}

}} // namespace GGPKExp::Parsers
