#include "OtParser.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <regex>
#include <sstream>

namespace GGPKExp { namespace Parsers {

namespace {

// Decode the caller-supplied bytes to UTF-8. Accept UTF-16LE with BOM (FF FE)
// and raw UTF-8 (no BOM or BOM EF BB BF). Anything else passes through as-is
// (bytes interpreted as UTF-8) — .ot files in POE2 are consistently UTF-16LE.
std::string DecodeToUtf8(const std::vector<uint8_t>& bytes) {
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        // UTF-16LE with BOM; minimal ASCII-preserving decoder.
        std::string out;
        out.reserve((bytes.size() - 2) / 2);
        for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
            uint16_t w = static_cast<uint16_t>(bytes[i]) |
                         (static_cast<uint16_t>(bytes[i + 1]) << 8);
            out.push_back(w < 128 ? static_cast<char>(w) : '?');
        }
        return out;
    }
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        return std::string(reinterpret_cast<const char*>(bytes.data() + 3),
                           bytes.size() - 3);
    }
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string TrimAscii(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

bool StartsWith(const std::string& s, const char* prefix) {
    size_t n = std::strlen(prefix);
    return s.size() >= n && std::memcmp(s.data(), prefix, n) == 0;
}

// Extract trailing base-10 integer from `s`. Returns -1 if none present.
int ExtractTrailingInt(const std::string& s) {
    size_t i = s.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(s[i - 1]))) --i;
    if (i == s.size()) return -1;
    int result = 0;
    for (size_t j = i; j < s.size(); ++j) result = result * 10 + (s[j] - '0');
    return result;
}

const std::regex& PlayAnimationRegex() {
    // Accept optional single or double quotes around the clip name so bodies
    // like PlayAnimation('emerge') or PlayAnimation("emerge") match the same
    // as the bare PlayAnimation(emerge).
    static const std::regex r(R"(PlayAnimation\(\s*['"]?([A-Za-z0-9_]+)['"]?\s*\))");
    return r;
}

std::string ExtractFirstPlayAnimation(const std::string& body) {
    std::smatch m;
    if (std::regex_search(body, m, PlayAnimationRegex()) && m.size() >= 2) {
        return m[1].str();
    }
    return {};
}

// Find the end of an identifier starting at p. Returns one past the last char.
size_t IdEnd(const std::string& s, size_t p) {
    while (p < s.size()) {
        char c = s[p];
        if (c != '_' && !std::isalnum(static_cast<unsigned char>(c))) break;
        ++p;
    }
    return p;
}

// Skip whitespace starting at p (space, tab, newline, cr). Returns new position.
size_t SkipWs(const std::string& s, size_t p) {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
    return p;
}

// Read a double-quoted script body starting at *p (pointing at the opening ").
// Returns the body contents without quotes. Advances p past the closing quote.
// Handles `\"` as a literal quote inside the body so escaped quotes in .ot
// handler scripts don't terminate early.
std::string ReadQuotedBody(const std::string& s, size_t& p, bool& unterminated) {
    unterminated = false;
    if (p >= s.size() || s[p] != '"') { unterminated = true; return {}; }
    ++p;
    std::string body;
    while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\' && p + 1 < s.size() && s[p + 1] == '"') {
            // Escaped quote — keep the quote in the body, skip the backslash.
            body.push_back('"');
            p += 2;
            continue;
        }
        body.push_back(s[p++]);
    }
    if (p >= s.size()) { unterminated = true; return body; }
    ++p;
    return body;
}

// Parse Transitionable block body between blockStart (char after `{`) and
// blockEnd (position of `}`).
void ParseTransitionableBody(const std::string& text,
                              size_t blockStart,
                              size_t blockEnd,
                              OtTransitionable& out,
                              std::vector<std::string>& warnings,
                              const std::string& sourcePath) {
    size_t p = blockStart;
    while (p < blockEnd) {
        p = SkipWs(text, p);
        if (p >= blockEnd) break;
        if (text[p] != '_' && !std::isalpha(static_cast<unsigned char>(text[p]))) {
            ++p;
            continue;
        }
        size_t idStart = p;
        size_t idEndPos = IdEnd(text, p);
        std::string key(text.data() + idStart, idEndPos - idStart);
        p = idEndPos;
        p = SkipWs(text, p);
        if (p >= blockEnd || text[p] != '=') {
            while (p < blockEnd && text[p] != '\n') ++p;
            continue;
        }
        ++p;
        p = SkipWs(text, p);

        if (key == "num_states") {
            int val = 0;
            bool hasDigit = false;
            while (p < blockEnd && std::isdigit(static_cast<unsigned char>(text[p]))) {
                val = val * 10 + (text[p] - '0');
                hasDigit = true;
                ++p;
            }
            if (hasDigit) out.declaredNumStates = val;
            else warnings.push_back("Transitionable.num_states not numeric in " + sourcePath);
            while (p < blockEnd && text[p] != '\n') ++p;
            continue;
        }

        if (StartsWith(key, "create_state_") || StartsWith(key, "on_transition_to_")) {
            int idx = ExtractTrailingInt(key);
            if (idx < 0) {
                warnings.push_back("Transitionable handler '" + key + "' has no trailing index in " + sourcePath);
                while (p < blockEnd && text[p] != '\n') ++p;
                continue;
            }
            if (p >= blockEnd || text[p] != '"') {
                warnings.push_back("Transitionable handler '" + key + "' missing quoted body in " + sourcePath);
                while (p < blockEnd && text[p] != '\n') ++p;
                continue;
            }
            bool unterminated = false;
            std::string body = ReadQuotedBody(text, p, unterminated);
            if (unterminated) {
                warnings.push_back("Transitionable handler '" + key + "' has unterminated quote in " + sourcePath);
            }
            OtStateHandler h;
            h.key           = key;
            h.body          = std::move(body);
            h.stateIndex    = idx;
            h.isTransition  = StartsWith(key, "on_transition_to_");
            h.extractedClip = ExtractFirstPlayAnimation(h.body);
            out.handlers.push_back(std::move(h));
            continue;
        }

        if (key == "create_untransitioned" || key == "create_transitioned" ||
            key == "on_untransition"       || key == "on_transition") {
            if (p >= blockEnd || text[p] != '"') {
                warnings.push_back("Transitionable handler '" + key + "' missing quoted body in " + sourcePath);
                while (p < blockEnd && text[p] != '\n') ++p;
                continue;
            }
            bool unterminated = false;
            std::string body = ReadQuotedBody(text, p, unterminated);
            if (unterminated) {
                warnings.push_back("Transitionable handler '" + key + "' has unterminated quote in " + sourcePath);
            }
            OtStateHandler h;
            h.key           = key;
            h.body          = std::move(body);
            // Mapping (suffix-less binary form):
            //   create_untransitioned / on_untransition -> state 0
            //   create_transitioned   / on_transition   -> state 1
            h.stateIndex    = (key == "create_transitioned" || key == "on_transition") ? 1 : 0;
            h.isTransition  = (key == "on_transition" || key == "on_untransition");
            h.extractedClip = ExtractFirstPlayAnimation(h.body);
            out.handlers.push_back(std::move(h));
            out.isBinary    = true;
            continue;
        }

        // Unknown key — silently skip to end of value
        if (p < blockEnd && text[p] == '"') {
            bool unterminated = false;
            (void)ReadQuotedBody(text, p, unterminated);
        } else {
            while (p < blockEnd && text[p] != '\n') ++p;
        }
    }
}

// Locate first `Transitionable` block. Returns true and sets openPos (pos of '{')
// and closePos (pos of matching '}').
bool FindTransitionableBlock(const std::string& text,
                              size_t& openPos,
                              size_t& closePos) {
    static const std::string key = "Transitionable";
    size_t p = 0;
    while (p < text.size()) {
        size_t hit = text.find(key, p);
        if (hit == std::string::npos) return false;
        bool lineStart = (hit == 0);
        if (!lineStart) {
            size_t q = hit;
            while (q > 0 && (text[q - 1] == ' ' || text[q - 1] == '\t')) --q;
            if (q > 0 && text[q - 1] == '\n') lineStart = true;
            if (q == 0) lineStart = true;
        }
        size_t afterKey = hit + key.size();
        size_t t = SkipWs(text, afterKey);
        if (lineStart && t < text.size() && text[t] == '{') {
            openPos = t;
            int depth = 1;
            size_t r = t + 1;
            while (r < text.size() && depth > 0) {
                if (text[r] == '{') ++depth;
                else if (text[r] == '}') --depth;
                ++r;
            }
            if (depth != 0) return false;
            closePos = r - 1;
            return true;
        }
        p = hit + 1;
    }
    return false;
}

} // anonymous namespace

bool ParseOtFile(const std::vector<uint8_t>& bytes,
                 const std::string& sourcePath,
                 OtFile& out) {
    out = OtFile{};
    out.sourcePath = sourcePath;
    if (bytes.empty()) {
        out.warnings.push_back("OtParser: empty bytes for " + sourcePath);
        return false;
    }
    std::string text = DecodeToUtf8(bytes);
    if (text.empty()) {
        out.warnings.push_back("OtParser: decoded text empty for " + sourcePath);
        return false;
    }

    // Top-of-file: version N, extends "..."
    {
        std::istringstream lineStream(text);
        std::string line;
        for (int i = 0; i < 6 && std::getline(lineStream, line); ++i) {
            std::string t = TrimAscii(line);
            if (t.empty()) continue;
            if (StartsWith(t, "version ")) {
                out.version = ExtractTrailingInt(t);
            } else if (StartsWith(t, "extends ")) {
                size_t q1 = t.find('"');
                size_t q2 = (q1 == std::string::npos) ? q1 : t.find('"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
                    out.extendsPath = t.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }
    }

    size_t openPos = 0, closePos = 0;
    if (!FindTransitionableBlock(text, openPos, closePos)) {
        // Phase 4 M4 — diagnostic hint: if the leaf had `extends "..."` but no
        // Transitionable block was found locally, the block likely lives in the
        // parent .ot (which Phase 4 does NOT walk — resolver lives at the .aoc
        // level only). Emit a warning so the Diagnostic tab explains why the
        // sidepanel shows "(no Transitionable block resolved)".
        if (!out.extendsPath.empty()) {
            out.warnings.push_back(
                "OtParser: no Transitionable block in leaf; extends chain not walked in Phase 4 ('" +
                out.extendsPath + "')");
        }
        return true;
    }

    auto tr = std::make_unique<OtTransitionable>();
    ParseTransitionableBody(text, openPos + 1, closePos, *tr, out.warnings, sourcePath);

    std::string lowPath = sourcePath;
    std::transform(lowPath.begin(), lowPath.end(), lowPath.begin(),
                    [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (lowPath.size() >= 4 && lowPath.compare(lowPath.size() - 4, 4, ".otc") == 0) {
        tr->sourceOtcPath = sourcePath;
    } else {
        tr->sourceOtPath = sourcePath;
    }

    int maxIdx = -1;
    for (const auto& h : tr->handlers) if (h.stateIndex > maxIdx) maxIdx = h.stateIndex;
    tr->numStates = std::max(tr->declaredNumStates, maxIdx + 1);
    if (tr->isBinary && tr->numStates < 2) tr->numStates = 2;

    out.transitionable = std::move(tr);
    return true;
}

void MergeOtFiles(const OtFile& ot, const OtFile& otc, OtFile& merged) {
    merged = OtFile{};
    if (!otc.sourcePath.empty())     merged.sourcePath = otc.sourcePath;
    else if (!ot.sourcePath.empty()) merged.sourcePath = ot.sourcePath;
    merged.version     = ot.version     != 0 ? ot.version     : otc.version;
    merged.extendsPath = !ot.extendsPath.empty() ? ot.extendsPath : otc.extendsPath;
    for (const auto& w : ot.warnings)  merged.warnings.push_back(w);
    for (const auto& w : otc.warnings) merged.warnings.push_back(w);

    const bool hasOt  = ot.transitionable  != nullptr;
    const bool hasOtc = otc.transitionable != nullptr;
    if (!hasOt && !hasOtc) return;

    auto tr = std::make_unique<OtTransitionable>();
    if (hasOt) {
        tr->declaredNumStates = std::max(tr->declaredNumStates, ot.transitionable->declaredNumStates);
        tr->isBinary          = tr->isBinary || ot.transitionable->isBinary;
        if (!ot.transitionable->sourceOtPath.empty())
            tr->sourceOtPath = ot.transitionable->sourceOtPath;
        if (!ot.transitionable->sourceOtcPath.empty())
            tr->sourceOtcPath = ot.transitionable->sourceOtcPath;
        for (const auto& h : ot.transitionable->handlers) tr->handlers.push_back(h);
    }
    if (hasOtc) {
        tr->declaredNumStates = std::max(tr->declaredNumStates, otc.transitionable->declaredNumStates);
        tr->isBinary          = tr->isBinary || otc.transitionable->isBinary;
        if (!otc.transitionable->sourceOtPath.empty())
            tr->sourceOtPath = otc.transitionable->sourceOtPath;
        if (!otc.transitionable->sourceOtcPath.empty())
            tr->sourceOtcPath = otc.transitionable->sourceOtcPath;
        for (const auto& h : otc.transitionable->handlers) {
            auto it = std::find_if(tr->handlers.begin(), tr->handlers.end(),
                                    [&](const OtStateHandler& x){ return x.key == h.key; });
            if (it != tr->handlers.end()) tr->handlers.erase(it);
            tr->handlers.push_back(h);
        }
    }
    int maxIdx = -1;
    for (const auto& h : tr->handlers) if (h.stateIndex > maxIdx) maxIdx = h.stateIndex;
    tr->numStates = std::max(tr->declaredNumStates, maxIdx + 1);
    if (tr->isBinary && tr->numStates < 2) tr->numStates = 2;
    merged.transitionable = std::move(tr);
}

}} // namespace GGPKExp::Parsers
