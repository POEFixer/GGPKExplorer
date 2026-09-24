#include "AocTokenizer.h"

#include <windows.h>
#include <cctype>
#include <cstring>
#include <sstream>

namespace GGPKExp { namespace Parsers {

namespace {

// Convert UTF-16LE bytes (after BOM) into UTF-8 std::string.
std::string Utf16LeToUtf8(const uint8_t* data, std::size_t byteCount) {
    if (byteCount == 0) return {};
    const int wideChars = static_cast<int>(byteCount / 2);
    const wchar_t* wide = reinterpret_cast<const wchar_t*>(data);
    int utf8Len = ::WideCharToMultiByte(CP_UTF8, 0, wide, wideChars, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) return {};
    std::string out(static_cast<std::size_t>(utf8Len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide, wideChars, out.data(), utf8Len, nullptr, nullptr);
    return out;
}

// Strip comments starting with // to EOL, and /* ... */ block comments,
// respecting "..." and '...' string contexts.
// Block comments are stripped IN-PLACE (returned string has them removed).
// Note: block comment handling here is intentionally line-local (doesn't span lines);
// multi-line /* */ comments are handled separately (see StripBlockComments).
std::string StripLineComment(const std::string& line) {
    std::string out;
    out.reserve(line.size());
    bool inDouble = false;
    bool inSingle = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\\' && (inDouble || inSingle)) {
            out.push_back(c);
            if (i + 1 < line.size()) { out.push_back(line[i + 1]); ++i; }
            continue;
        }
        if (!inSingle && c == '"') { inDouble = !inDouble; out.push_back(c); continue; }
        if (!inDouble && c == '\'') { inSingle = !inSingle; out.push_back(c); continue; }
        if (!inDouble && !inSingle && c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            // Line comment — rest of line is a comment.
            return out;
        }
        if (!inDouble && !inSingle && c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
            // Inline block comment — find closing */ on same line.
            std::size_t j = i + 2;
            while (j + 1 < line.size() && !(line[j] == '*' && line[j + 1] == '/')) ++j;
            if (j + 1 < line.size()) {
                // Closed on same line — skip past */
                i = j + 1;
                continue;
            } else {
                // Unterminated block comment — rest of line dropped.
                return out;
            }
        }
        out.push_back(c);
    }
    return out;
}

// Trim trailing whitespace (spaces, tabs, CR) from line.
std::string RTrim(const std::string& s) {
    std::size_t end = s.size();
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) --end;
    return s.substr(0, end);
}

// Parse leading indent. Returns (indent_level, rest_of_line_without_indent).
// Accepts mixed tabs + spaces. 4 spaces = 1 indent level (conventional).
// Leftover 1-3 trailing spaces are silently consumed (treated as padding).
std::pair<int, std::string> ParseIndent(const std::string& line) {
    int indent = 0;
    std::size_t i = 0;
    while (i < line.size()) {
        if (line[i] == '\t') { ++indent; ++i; }
        else if (line[i] == ' ') {
            // Count a run of spaces, convert 4-space groups to indent levels.
            std::size_t spaceStart = i;
            while (i < line.size() && line[i] == ' ') ++i;
            std::size_t spaceCount = i - spaceStart;
            indent += static_cast<int>(spaceCount / 4);
            // Remainder (1-3 spaces) is treated as indent padding; no error.
        }
        else break;
    }
    return {indent, line.substr(i)};
}

// Trim leading/trailing spaces from a string.
std::string Trim(const std::string& s) {
    std::size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) ++start;
    std::size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
    return s.substr(start, end - start);
}

// Parse a key=value line. Handles:
//   key = "value"     (double-quoted)
//   key = 'value'     (single-quoted, possibly pre-merged multi-line)
//   key = bareword    (unquoted)
//   key = N           (number)
//   key = true/false  (bool)
// Returns (key, value). Returns empty key on malformed line.
std::pair<std::string, std::string> ParseKeyValue(const std::string& body) {
    auto eq = body.find('=');
    if (eq == std::string::npos) return {{}, {}};

    std::string key = Trim(body.substr(0, eq));
    std::string valueRaw = Trim(body.substr(eq + 1));

    // Strip surrounding quotes if paired.
    if (valueRaw.size() >= 2) {
        if (valueRaw.front() == '"' && valueRaw.back() == '"') {
            return {key, valueRaw.substr(1, valueRaw.size() - 2)};
        }
        if (valueRaw.front() == '\'' && valueRaw.back() == '\'') {
            return {key, valueRaw.substr(1, valueRaw.size() - 2)};
        }
    }
    return {key, valueRaw};
}

} // anonymous namespace

Result<std::vector<AocLine>> TokenizeAoc(const uint8_t* bytes, std::size_t length) {
    if (length < 2 || bytes[0] != 0xFF || bytes[1] != 0xFE) {
        return Result<std::vector<AocLine>>::Fail("Expected UTF-16LE BOM (FF FE) at start of .aoc file");
    }
    if ((length - 2) % 2 != 0) {
        return Result<std::vector<AocLine>>::Fail("UTF-16LE payload has odd byte count (truncated file?)");
    }

    std::string utf8 = Utf16LeToUtf8(bytes + 2, length - 2);

    // Strip multi-line /* ... */ block comments BEFORE line splitting.
    // Preserves newlines inside block comments to keep line numbers stable.
    // Respects "..." and '...' string contexts.
    {
        std::string out;
        out.reserve(utf8.size());
        bool inDouble = false, inSingle = false;
        for (std::size_t i = 0; i < utf8.size(); ++i) {
            char c = utf8[i];
            if (c == '\\' && (inDouble || inSingle)) {
                out.push_back(c);
                if (i + 1 < utf8.size()) { out.push_back(utf8[i + 1]); ++i; }
                continue;
            }
            if (!inSingle && c == '"')  { inDouble = !inDouble; out.push_back(c); continue; }
            if (!inDouble && c == '\'') { inSingle = !inSingle; out.push_back(c); continue; }
            if (!inDouble && !inSingle && c == '/' && i + 1 < utf8.size() && utf8[i + 1] == '*') {
                // Skip until matching */, preserving newlines for line-number stability.
                std::size_t j = i + 2;
                while (j + 1 < utf8.size()) {
                    if (utf8[j] == '*' && utf8[j + 1] == '/') break;
                    if (utf8[j] == '\n') out.push_back('\n');
                    ++j;
                }
                if (j + 1 < utf8.size()) i = j + 1;
                else                     i = utf8.size();  // unterminated — drop rest
                continue;
            }
            out.push_back(c);
        }
        utf8 = std::move(out);
    }

    // Split by '\n'. We'll merge multi-line '...' strings after initial split.
    std::vector<std::string> rawLines;
    {
        std::string cur;
        for (char c : utf8) {
            if (c == '\n') { rawLines.push_back(cur); cur.clear(); }
            else if (c == '\r') { /* skip CR */ }
            else cur.push_back(c);
        }
        if (!cur.empty()) rawLines.push_back(cur);
    }

    // Pre-split: if a line (after leading whitespace) starts with `{` and has more
    // content after, split off the `{` so it's alone on its line. This way the
    // value-continuation merge (which balances braces) cannot mistake a block-open
    // brace for a value brace.
    // Same for trailing `}` followed by comment-free residue — rare, so not handled.
    {
        std::vector<std::string> split;
        split.reserve(rawLines.size());
        for (std::string& ln : rawLines) {
            // Find start of non-whitespace
            std::size_t p = 0;
            while (p < ln.size() && (ln[p] == ' ' || ln[p] == '\t')) ++p;
            if (p < ln.size() && ln[p] == '{') {
                // Check if there's more content after the `{` (excluding trailing whitespace).
                std::size_t q = p + 1;
                while (q < ln.size() && (ln[q] == ' ' || ln[q] == '\t' || ln[q] == '\r')) ++q;
                if (q < ln.size()) {
                    // More content — split.
                    std::string head = ln.substr(0, p + 1);  // leading whitespace + "{"
                    std::string tail = ln.substr(p + 1);     // rest, preserving original tabs/spaces
                    split.push_back(std::move(head));
                    split.push_back(std::move(tail));
                    continue;
                }
            }
            split.push_back(std::move(ln));
        }
        rawLines = std::move(split);
    }

    // Pre-strip line comments per rawLine (for balance counting only — original is kept).
    // This prevents apostrophes / double-quotes / braces inside `// ...` comments from
    // triggering spurious multi-line merges.
    std::vector<std::string> strippedLines;
    strippedLines.reserve(rawLines.size());
    for (const auto& ln : rawLines) strippedLines.push_back(StripLineComment(ln));

    // Merge lines whose value opens a '...' string and doesn't close it.
    // Heuristic: count unescaped single-quotes in a line; if odd, merge with next lines until count even.
    //
    // Additionally merge key = value continuations where the value is on the next line
    // (e.g. `RenderPasses =` alone, followed by `'{...}'` on the next line).
    std::vector<std::string> merged;
    std::vector<std::size_t> mergedToRaw;  // 1-based source line for each merged entry
    {
        auto countUnescapedSingleQuotes = [](const std::string& s) {
            int n = 0;
            bool inDouble = false;
            for (std::size_t k = 0; k < s.size(); ++k) {
                char c = s[k];
                if (c == '\\' && k + 1 < s.size()) { ++k; continue; }
                if (c == '"') inDouble = !inDouble;
                else if (!inDouble && c == '\'') ++n;
            }
            return n;
        };

        // Count unescaped double-quotes, respecting single-quote context (inside '...' they are literal).
        auto countUnescapedDoubleQuotes = [](const std::string& s) {
            int n = 0;
            bool inSingle = false;
            for (std::size_t k = 0; k < s.size(); ++k) {
                char c = s[k];
                if (c == '\\' && k + 1 < s.size()) { ++k; continue; }
                if (c == '\'') inSingle = !inSingle;
                else if (!inSingle && c == '"') ++n;
            }
            return n;
        };

        // Check if a line (after stripping comments and trimming trailing whitespace) ends with '='
        // with no value after it. Such lines are continuation markers awaiting the value on a
        // subsequent line. A key=value line has a '=' followed by at least one non-whitespace char.
        auto endsWithEqualsAwaitingValue = [](const std::string& s) {
            // Walk from the end, skipping whitespace, looking for '='.
            // If '=' found AND nothing after it but whitespace → continuation.
            const std::string stripped = [&]() {
                // Strip trailing whitespace and CR
                std::size_t e = s.size();
                while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
                return s.substr(0, e);
            }();
            if (stripped.empty()) return false;
            if (stripped.back() != '=') return false;
            // Must be a real key=value anchor: there must be a '=' only at the end with a key before it.
            // Ensure there's non-whitespace before '=' (i.e. a key).
            for (std::size_t k = 0; k + 1 < stripped.size(); ++k) {
                char c = stripped[k];
                if (c != ' ' && c != '\t') return true;  // found a key char
            }
            return false;
        };

        // Check if a line contains `= {` (key equals open-brace) with nothing between
        // but whitespace, AND the brace is not closed on the same line. This is a
        // multi-line value using curly-brace delimiters (`key = {\n ... \n}`).
        // Used as a secondary trigger for isValueContinuation.
        auto hasOpenBraceValueTrigger = [](const std::string& s) {
            // Find '=' outside strings.
            bool inDouble = false, inSingle = false;
            std::size_t eqPos = std::string::npos;
            for (std::size_t k = 0; k < s.size(); ++k) {
                char c = s[k];
                if (c == '\\' && k + 1 < s.size()) { ++k; continue; }
                if (!inSingle && c == '"') inDouble = !inDouble;
                else if (!inDouble && c == '\'') inSingle = !inSingle;
                else if (!inDouble && !inSingle && c == '=') { eqPos = k; break; }
            }
            if (eqPos == std::string::npos) return false;
            // Skip whitespace after `=`.
            std::size_t k = eqPos + 1;
            while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
            if (k >= s.size() || s[k] != '{') return false;
            // Count brace depth from `{` onward (outside strings).
            int depth = 0;
            inDouble = inSingle = false;
            for (std::size_t j = k; j < s.size(); ++j) {
                char c = s[j];
                if (c == '\\' && j + 1 < s.size()) { ++j; continue; }
                if (!inSingle && c == '"') inDouble = !inDouble;
                else if (!inDouble && c == '\'') inSingle = !inSingle;
                else if (!inDouble && !inSingle) {
                    if (c == '{') ++depth;
                    else if (c == '}') --depth;
                }
            }
            return depth > 0;  // unbalanced { — needs continuation
        };

        // Count brace depth outside of "..." and '...' strings.
        auto braceDepth = [](const std::string& s) {
            int depth = 0;
            bool inDouble = false, inSingle = false;
            for (std::size_t k = 0; k < s.size(); ++k) {
                char c = s[k];
                if (c == '\\' && k + 1 < s.size()) { ++k; continue; }
                if (!inSingle && c == '"') inDouble = !inDouble;
                else if (!inDouble && c == '\'') inSingle = !inSingle;
                else if (!inDouble && !inSingle) {
                    if (c == '{') ++depth;
                    else if (c == '}') --depth;
                }
            }
            return depth;
        };

        std::size_t i = 0;
        while (i < rawLines.size()) {
            std::string acc = rawLines[i];
            std::size_t startLine = i + 1;  // 1-based
            bool isValueContinuation = false;  // we're merging a key=value spanning multiple lines

            // First, if this line ends with '=' and there's no value yet, merge with the
            // next non-empty line to pull the value up. If that value line begins with '{',
            // the value is a curly-brace-delimited block (merge until braces balanced).
            if (endsWithEqualsAwaitingValue(StripLineComment(acc))) {
                std::size_t j = i + 1;
                // skip empty lines
                while (j < rawLines.size()) {
                    std::string trial = rawLines[j];
                    bool allWs = true;
                    for (char c : trial) if (c != ' ' && c != '\t' && c != '\r') { allWs = false; break; }
                    if (allWs) { ++j; continue; }
                    break;
                }
                if (j < rawLines.size()) {
                    // merge everything from i+1..j onto acc
                    for (std::size_t k = i + 1; k <= j; ++k) {
                        acc.push_back('\n');
                        acc.append(rawLines[k]);
                    }
                    i = j;
                    isValueContinuation = true;
                }
            }

            // Alternative trigger: `key = {` on the same line, where `{` is unbalanced
            // and the value spans multiple lines. Activate brace-balancing merge.
            if (!isValueContinuation && hasOpenBraceValueTrigger(StripLineComment(acc))) {
                isValueContinuation = true;
            }

            // Merge lines until single-quote and double-quote pairs are balanced.
            // Also merge while brace depth > 0 if this line is a value continuation
            // (`key = \n { ...nested... }`). Non-continuation lines starting with `{`
            // are block openers and must NOT pull the block body into one line.
            //
            // NOTE: balance is measured on comment-stripped lines (strippedLines[])
            // so that apostrophes inside `// ...` do not cause spurious merges.
            // We build accStripped incrementally by indices into strippedLines.
            std::string accStripped = strippedLines[startLine - 1];
            for (std::size_t k = startLine; k <= i; ++k) {
                accStripped.push_back('\n');
                accStripped.append(strippedLines[k]);
            }
            while (i + 1 < rawLines.size()) {
                bool unbalancedQuotes =
                    (countUnescapedSingleQuotes(accStripped) % 2) == 1 ||
                    (countUnescapedDoubleQuotes(accStripped) % 2) == 1;
                bool unbalancedBraces = isValueContinuation && braceDepth(accStripped) > 0;
                if (!unbalancedQuotes && !unbalancedBraces) break;
                ++i;
                acc.push_back('\n');
                acc.append(rawLines[i]);
                accStripped.push_back('\n');
                accStripped.append(strippedLines[i]);
            }
            merged.push_back(acc);
            mergedToRaw.push_back(startLine);
            ++i;
        }
    }

    std::vector<AocLine> out;
    out.reserve(merged.size());

    for (std::size_t idx = 0; idx < merged.size(); ++idx) {
        const std::string& stripped = RTrim(StripLineComment(merged[idx]));

        AocLine line;
        line.lineNumber = mergedToRaw[idx];
        line.raw = stripped;

        if (stripped.empty()) {
            line.kind = AocLine::Kind::Empty;
            out.push_back(std::move(line));
            continue;
        }

        auto [indent, body] = ParseIndent(stripped);
        if (indent < 0) {
            std::ostringstream os;
            os << "Mixed indentation (space where tab expected) at line " << line.lineNumber;
            return Result<std::vector<AocLine>>::Fail(os.str());
        }
        line.indent = indent;
        std::string trimmed = Trim(body);
        if (trimmed.empty()) {
            line.kind = AocLine::Kind::Empty;
            out.push_back(std::move(line));
            continue;
        }

        // Single-char '{' or '}' lines
        if (trimmed == "{") {
            line.kind = AocLine::Kind::BraceOpen;
            out.push_back(std::move(line));
            continue;
        }
        if (trimmed == "}") {
            line.kind = AocLine::Kind::BlockClose;
            out.push_back(std::move(line));
            continue;
        }

        // Compound start `{ <rest>` — emit BraceOpen and continue processing <rest>.
        // This handles `{\tkey = value` and `{\t}` where the opening brace and other
        // content (property or closing brace) are on one line.
        if (!trimmed.empty() && trimmed.front() == '{') {
            AocLine braceOpen;
            braceOpen.lineNumber = line.lineNumber;
            braceOpen.indent = line.indent;
            braceOpen.raw = "{";
            braceOpen.kind = AocLine::Kind::BraceOpen;
            out.push_back(std::move(braceOpen));

            // Strip leading '{' and leading whitespace from trimmed; treat as a fresh line at same indent.
            trimmed = Trim(trimmed.substr(1));
            if (trimmed.empty()) continue;
            // Re-check single-char `}` tokens after the strip (handles inline `{ }` split across logic).
            if (trimmed == "}") {
                AocLine braceClose;
                braceClose.lineNumber = line.lineNumber;
                braceClose.indent = line.indent;
                braceClose.raw = "}";
                braceClose.kind = AocLine::Kind::BlockClose;
                out.push_back(std::move(braceClose));
                continue;
            }
            line.raw = trimmed;
            // Fall through to the rest of the switch: the line is now the remainder.
        }

        // Top-level header lines use space-separator (no '='):
        //   version 2
        //   extends "Metadata/Parent"
        if (indent == 0) {
            auto sepAt = trimmed.find_first_of(" \t");
            if (sepAt != std::string::npos) {
                std::string head = trimmed.substr(0, sepAt);
                if (head == "version" || head == "extends") {
                    std::string rest = Trim(trimmed.substr(sepAt + 1));
                    // Strip surrounding quotes if paired.
                    if (rest.size() >= 2 &&
                        ((rest.front() == '"' && rest.back() == '"') ||
                         (rest.front() == '\'' && rest.back() == '\''))) {
                        rest = rest.substr(1, rest.size() - 2);
                    }
                    line.key = std::move(head);
                    line.value = std::move(rest);
                    line.kind = AocLine::Kind::HeaderKV;
                    out.push_back(std::move(line));
                    continue;
                }
            }
            // Bareword top-level modifier: `abstract` (no arguments). Treated as HeaderKV with empty value.
            if (trimmed == "abstract") {
                line.key = trimmed;
                line.value.clear();
                line.kind = AocLine::Kind::HeaderKV;
                out.push_back(std::move(line));
                continue;
            }
        }

        // key = value?
        if (trimmed.find('=') != std::string::npos) {
            auto [k, v] = ParseKeyValue(trimmed);
            if (k.empty()) {
                std::ostringstream os;
                os << "Malformed key=value at line " << line.lineNumber << ": " << stripped;
                return Result<std::vector<AocLine>>::Fail(os.str());
            }
            line.key = std::move(k);
            line.value = std::move(v);
            // Header-level KV when indent == 0 and key is "version" or "extends"
            line.kind = (indent == 0 && (line.key == "version" || line.key == "extends"))
                      ? AocLine::Kind::HeaderKV
                      : AocLine::Kind::Property;
            out.push_back(std::move(line));
            continue;
        }

        // Inline block forms:
        //   `BlockName { }` / `BlockName {}` — empty inline block
        //   `BlockName {`                    — block open on same line as name
        // Emit synthetic tokens accordingly.
        {
            std::string t = trimmed;
            // Pattern A: `BlockName { }` or `BlockName {}`
            if (!t.empty() && t.back() == '}') {
                std::size_t close = t.size() - 1;
                std::size_t k = close;
                while (k > 0 && (t[k - 1] == ' ' || t[k - 1] == '\t')) --k;
                if (k > 0 && t[k - 1] == '{') {
                    std::size_t open = k - 1;
                    std::string name = Trim(t.substr(0, open));
                    if (!name.empty() && name.find('=') == std::string::npos) {
                        line.key = name;
                        line.kind = AocLine::Kind::BlockOpen;
                        out.push_back(line);

                        AocLine braceOpen;
                        braceOpen.lineNumber = line.lineNumber;
                        braceOpen.indent = line.indent;
                        braceOpen.raw = "{";
                        braceOpen.kind = AocLine::Kind::BraceOpen;
                        out.push_back(std::move(braceOpen));

                        AocLine braceClose;
                        braceClose.lineNumber = line.lineNumber;
                        braceClose.indent = line.indent;
                        braceClose.raw = "}";
                        braceClose.kind = AocLine::Kind::BlockClose;
                        out.push_back(std::move(braceClose));
                        continue;
                    }
                }
            }
            // Pattern B: `BlockName {` (no closing brace on same line).
            if (!t.empty() && t.back() == '{') {
                std::string name = Trim(t.substr(0, t.size() - 1));
                if (!name.empty() && name.find('=') == std::string::npos) {
                    line.key = name;
                    line.kind = AocLine::Kind::BlockOpen;
                    out.push_back(line);

                    AocLine braceOpen;
                    braceOpen.lineNumber = line.lineNumber;
                    braceOpen.indent = line.indent;
                    braceOpen.raw = "{";
                    braceOpen.kind = AocLine::Kind::BraceOpen;
                    out.push_back(std::move(braceOpen));
                    continue;
                }
            }
        }

        // Orphan quoted string with no associated key=value. This occurs in real files when
        // a key is hidden inside a line comment (`//...key =`) and the subsequent line begins
        // with `"..."` or `'...'`. The string is orphan content — treat as Empty (skip).
        if (!trimmed.empty() && (trimmed.front() == '"' || trimmed.front() == '\'')) {
            line.kind = AocLine::Kind::Empty;
            out.push_back(std::move(line));
            continue;
        }

        // Otherwise: bare identifier — block opener (name of a top-level block).
        line.key = trimmed;
        line.kind = AocLine::Kind::BlockOpen;
        out.push_back(std::move(line));
    }

    return Result<std::vector<AocLine>>::Ok(std::move(out));
}

}} // namespace GGPKExp::Parsers
