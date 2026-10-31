// Plugins/GGPKExplorer/core/SearchQuery.h
// One search's query, prepared ONCE per search (never per file, line or cell): the expanded
// query and replacement, the lowered needle and the compiled regex. Shared by FileSearcher,
// ContentSearcher and DatSearcher; header-only (tests/run_tests.bat compiles DatSearcher.cpp
// with nothing but DatParser and DatSchema).
#pragma once
#include "SearchTypes.h"
#include <cctype>
#include <optional>
#include <regex>
#include <string>

namespace GGPKExp {

// std::tolower per byte (the "C" locale the process runs in: A-Z), as the searchers lowered
// every text before.
inline std::string SearchToLower(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (unsigned char c : s) r += static_cast<char>(std::tolower(c));
    return r;
}

// The "Extended" escapes: \n \r \t \0 \xHH \\ (any other backslash stays literal). A bad \x
// escape throws (std::stoi), as it always did; SearchQuery catches it.
inline std::string ExpandExtendedEscapes(const std::string& pattern) {
    std::string result;
    result.reserve(pattern.size());
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '\\' && i + 1 < pattern.size()) {
            char next = pattern[i + 1];
            switch (next) {
            case 'n': result += '\n'; i++; break;
            case 'r': result += '\r'; i++; break;
            case 't': result += '\t'; i++; break;
            case '0': result += '\0'; i++; break;
            case 'x':
                if (i + 3 < pattern.size()) {
                    result += static_cast<char>(std::stoi(pattern.substr(i + 2, 2), nullptr, 16));
                    i += 3;
                } else {
                    result += pattern[i];
                }
                break;
            case '\\': result += '\\'; i++; break;
            default: result += pattern[i]; break;
            }
        } else {
            result += pattern[i];
        }
    }
    return result;
}

// Whole-word boundaries around [pos, pos + length) of `haystack` (ASCII letters and digits).
inline bool IsWholeWordAt(const std::string& haystack, size_t pos, size_t length) {
    const bool startOk = (pos == 0 || !std::isalnum(static_cast<unsigned char>(haystack[pos - 1])));
    const bool endOk = (pos + length >= haystack.size() ||
                        !std::isalnum(static_cast<unsigned char>(haystack[pos + length])));
    return startOk && endOk;
}

/// Never throws on construction: a regex that does not compile, or a bad \x escape in the
/// query, makes a query that matches nothing (Valid() false; the searchers always treated
/// such a pattern as "no match"). A bad escape in the replacement only invalidates the
/// replacement (ReplacementValid()). The const members are safe from several threads at once.
class SearchQuery {
public:
    SearchQuery() = default;

    explicit SearchQuery(const SearchOptions& opts) : m_opts(opts) {
        try {
            m_query = opts.mode == SearchMode::Extended ? ExpandExtendedEscapes(opts.query) : opts.query;
            m_needle = opts.matchCase ? m_query : SearchToLower(m_query);
            if (opts.mode == SearchMode::Regex) {
                auto flags = std::regex::ECMAScript | std::regex::optimize;
                if (!opts.matchCase) flags |= std::regex::icase;
                m_regex.emplace(m_query, flags);
            }
            m_valid = true;
        } catch (...) {
            m_valid = false;
            m_regex.reset();
        }
        try {
            m_replacement = opts.mode == SearchMode::Extended ? ExpandExtendedEscapes(opts.replaceWith)
                                                              : opts.replaceWith;
            m_replacementValid = true;
        } catch (...) {
            m_replacementValid = false;
        }
    }

    bool Valid() const { return m_valid; }
    bool ReplacementValid() const { return m_valid && m_replacementValid; }
    const SearchOptions& Options() const { return m_opts; }
    bool IsRegex() const { return m_opts.mode == SearchMode::Regex; }
    bool MatchCase() const { return m_opts.matchCase; }
    bool WholeWord() const { return m_opts.matchWholeWord; }
    const std::regex& Regex() const { return *m_regex; }      // IsRegex() && Valid() only
    const std::string& Query() const { return m_query; }       // expanded (Extended)
    const std::string& Needle() const { return m_needle; }     // expanded, lowered unless MatchCase
    const std::string& Replacement() const { return m_replacement; }

    /// `value` (a name, a path, a DAT cell) holds a match.
    bool Matches(const std::string& value) const {
        if (!m_valid) return false;
        if (IsRegex()) {
            try {
                return std::regex_search(value, *m_regex);
            } catch (...) {
                return false;
            }
        }
        std::string lowered;
        if (!m_opts.matchCase) lowered = SearchToLower(value);
        const std::string& haystack = m_opts.matchCase ? value : lowered;
        if (m_opts.matchWholeWord) {
            size_t pos = 0;
            while ((pos = haystack.find(m_needle, pos)) != std::string::npos) {
                if (IsWholeWordAt(haystack, pos, m_needle.size())) return true;
                pos++;
            }
            return false;
        }
        return haystack.find(m_needle) != std::string::npos;
    }

private:
    SearchOptions m_opts;
    std::string m_query;
    std::string m_needle;
    std::string m_replacement;
    std::optional<std::regex> m_regex;
    bool m_valid = false;
    bool m_replacementValid = false;
};

} // namespace GGPKExp
