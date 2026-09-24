#pragma once
#include <string>

namespace GGPKExp { namespace Parsers { namespace Json {

// Low-level JSON scanners shared across AocLightsParser,
// AocSoundEventsParser, and AocParticleEffectsParser.
//
// These scanners are intentionally permissive: they accept single or double
// quotes, trailing commas, unknown keys, and malformed entries.

// Skip whitespace AND commas (comma is a separator in the lenient .aoc JSON
// dialect and is eaten here to simplify per-field loops).
inline void SkipJsonWs(const char*& p, const char* e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')) ++p;
}

// Read a `"..."` or `'...'` string. Returns true if a string was consumed.
inline bool ReadJsonString(const char*& p, const char* e, std::string& val) {
    SkipJsonWs(p, e);
    if (p >= e || (*p != '"' && *p != '\'')) return false;
    const char quote = *p++;
    val.clear();
    while (p < e && *p != quote) {
        if (*p == '\\' && p + 1 < e) { val.push_back(p[1]); p += 2; continue; }
        val.push_back(*p++);
    }
    if (p < e) ++p;
    return true;
}

// Skip a balanced JSON value starting at *p. Handles strings, arrays, objects,
// and primitives. Returns the slice of input that was consumed (may be empty
// if nothing parseable was found).
inline std::string SkipJsonValue(const char*& p, const char* e) {
    SkipJsonWs(p, e);
    const char* start = p;
    if (p >= e) return {};

    if (*p == '"' || *p == '\'') {
        std::string dummy;
        ReadJsonString(p, e, dummy);
        return std::string(start, p);
    }
    if (*p == '[' || *p == '{') {
        const char open  = *p;
        const char close = (open == '[') ? ']' : '}';
        int depth = 0;
        while (p < e) {
            if (*p == '"' || *p == '\'') {
                std::string dummy;
                ReadJsonString(p, e, dummy);
                continue;
            }
            if (*p == open)  { ++depth; ++p; continue; }
            if (*p == close) { --depth; ++p; if (depth == 0) break; continue; }
            ++p;
        }
        return std::string(start, p);
    }
    // Primitive: number/bool/null — read until delimiter.
    while (p < e && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        ++p;
    }
    return std::string(start, p);
}

// Detect whether `jsonBlob` is the nested "animations" format:
//   [ { "name": "...", "events": [ ... ] }, ... ]
// A blob is "nested" if its first object contains both a "name" key and an
// "events" key. Otherwise treat as the flat form.
inline bool IsNestedAnimationsFormat(const std::string& jsonBlob) {
    const char* p = jsonBlob.c_str();
    const char* e = p + jsonBlob.size();

    SkipJsonWs(p, e);
    if (p >= e || *p != '[') return false;
    ++p;
    SkipJsonWs(p, e);
    if (p >= e || *p != '{') return false;
    ++p;

    bool hasName = false, hasEvents = false;
    while (p < e && *p != '}') {
        SkipJsonWs(p, e);
        if (p >= e || *p == '}') break;

        std::string key;
        if (!ReadJsonString(p, e, key)) { if (p < e) ++p; continue; }
        SkipJsonWs(p, e);
        if (p < e && *p == ':') ++p;
        SkipJsonWs(p, e);
        SkipJsonValue(p, e);

        if (key == "name")   hasName   = true;
        if (key == "events") hasEvents = true;
        if (hasName && hasEvents) return true;

        SkipJsonWs(p, e);
        if (p < e && *p == ',') ++p;
    }
    return hasName && hasEvents;
}

}}} // namespace GGPKExp::Parsers::Json
