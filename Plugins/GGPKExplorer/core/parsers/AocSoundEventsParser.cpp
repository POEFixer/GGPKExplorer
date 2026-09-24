#include "AocSoundEventsParser.h"
#include "AocJsonHelpers.h"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace GGPKExp { namespace Parsers {

namespace {

// Use shared primitives from AocJsonHelpers.h.
using Json::SkipJsonWs;
using Json::ReadJsonString;
using Json::SkipJsonValue;
using Json::IsNestedAnimationsFormat;

// --- Flat-form clip parser ------------------------------------------------
// Consumes a `[ {...}, {...} ]` array of SoundEvent-style objects.
// Recognises keys "time" (number), "type" (string), "filename" (string),
// "bone_name" (string, optional). Anything else is skipped.
void ParseFlatClips(const std::string&           jsonBlob,
                    std::vector<SoundEventClip>& out) {
    const char* p = jsonBlob.c_str();
    const char* e = p + jsonBlob.size();

    SkipJsonWs(p, e);
    if (p < e && *p == '[') ++p;
    while (p < e) {
        SkipJsonWs(p, e);
        if (p >= e || *p == ']') break;
        if (*p != '{') { ++p; continue; }
        ++p;

        SoundEventClip clip;
        while (p < e && *p != '}') {
            SkipJsonWs(p, e);
            if (p >= e || *p == '}') break;

            std::string key;
            if (!ReadJsonString(p, e, key)) { if (p < e) ++p; continue; }
            SkipJsonWs(p, e);
            if (p < e && *p == ':') ++p;
            SkipJsonWs(p, e);

            if (p < e && (*p == '"' || *p == '\'')) {
                std::string v;
                ReadJsonString(p, e, v);
                if      (key == "type")      clip.type     = std::move(v);
                else if (key == "filename")  clip.filename = std::move(v);
                else if (key == "bone_name") clip.boneName = std::move(v);
                // Any other string fields (e.g. unknown keys) are ignored.
            } else if (p < e && (*p == '[' || *p == '{')) {
                SkipJsonValue(p, e); // skip unknown nested value
            } else {
                char* endp = nullptr;
                float f = std::strtof(p, &endp);
                if (endp > p) {
                    if (key == "time") clip.time = f;
                    p = endp;
                } else {
                    ++p;
                }
            }
            SkipJsonWs(p, e);
            if (p < e && *p == ',') ++p;
        }
        if (p < e) ++p; // past '}'
        if (!clip.filename.empty()) out.push_back(std::move(clip));
        SkipJsonWs(p, e);
        if (p < e && *p == ',') ++p;
    }
}

// --- Nested-form clip parser ----------------------------------------------
// Walk top-level animation group objects, extract each "events": [...] sub-array
// as a raw slice, and recurse into ParseFlatClips. Animation group names are
// dropped — clips are flattened into a single list.
void ParseNestedClips(const std::string&           jsonBlob,
                      std::vector<SoundEventClip>& out) {
    const char* p = jsonBlob.c_str();
    const char* e = p + jsonBlob.size();

    SkipJsonWs(p, e);
    if (p < e && *p == '[') ++p;
    while (p < e) {
        SkipJsonWs(p, e);
        if (p >= e || *p == ']') break;
        if (*p != '{') { ++p; continue; }
        ++p;

        while (p < e && *p != '}') {
            SkipJsonWs(p, e);
            if (p >= e || *p == '}') break;

            std::string key;
            if (!ReadJsonString(p, e, key)) { if (p < e) ++p; continue; }
            SkipJsonWs(p, e);
            if (p < e && *p == ':') ++p;
            SkipJsonWs(p, e);

            if (key == "events") {
                const std::string inner = SkipJsonValue(p, e);
                if (!inner.empty() && inner.front() == '[') {
                    ParseFlatClips(inner, out);
                }
            } else {
                SkipJsonValue(p, e); // skip "name", or any other field
            }

            SkipJsonWs(p, e);
            if (p < e && *p == ',') ++p;
        }
        if (p < e) ++p; // past '}'
        SkipJsonWs(p, e);
        if (p < e && *p == ',') ++p;
    }
}

} // anonymous namespace


// The resolver passes a synthetic AocRawNode whose `children` hold the
// SoundEvents block's top-level properties (one AocRawNode per line inside
// `SoundEvents { ... }`). Real-world .aoc files store all clips in a single
// JSON-blob kv whose key is usually "animations" (verified in
// D:/POE2Files/metadata/terrain/woods/wideriver/winter/lunarisshrine_gate.aoc).
bool ParseSoundEvents(const AocRawNode&         node,
                      SoundEventsBlock&         out,
                      std::vector<std::string>& warnings,
                      std::vector<std::string>& errors) {
    out = {};
    (void)errors;
    (void)warnings;

    // Look for the single json-blob kv — usual key is "animations"; also accept
    // "events" for flat form, then any child whose value starts with '[' as a
    // last-resort fallback.
    const std::string* blob = nullptr;
    for (const AocRawNode& child : node.children) {
        if (child.key == "animations" || child.key == "events") {
            blob = &child.value;
            break;
        }
    }
    if (!blob) {
        for (const AocRawNode& child : node.children) {
            if (!child.value.empty() && child.value.front() == '[') {
                blob = &child.value;
                break;
            }
        }
    }
    if (!blob) return false;

    if (IsNestedAnimationsFormat(*blob)) {
        ParseNestedClips(*blob, out.clips);
    } else {
        ParseFlatClips(*blob, out.clips);
    }

    std::sort(out.clips.begin(), out.clips.end(),
              [](const SoundEventClip& a, const SoundEventClip& b) { return a.time < b.time; });

    return !out.clips.empty();
}

}} // namespace GGPKExp::Parsers
