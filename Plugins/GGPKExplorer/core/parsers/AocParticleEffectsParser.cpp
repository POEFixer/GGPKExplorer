#include "AocParticleEffectsParser.h"
#include "AocJsonHelpers.h"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace GGPKExp { namespace Parsers {

namespace {

using Json::SkipJsonWs;
using Json::ReadJsonString;
using Json::SkipJsonValue;
using Json::IsNestedAnimationsFormat;

// Parse one event object into `clip`. Assumes `*p == '{'`.
// Advances p past the closing `}`. Returns true iff a usable clip was produced
// (filename non-empty AND type == "ParticleEffectEventType"). Unknown event
// types increment blockStats.skippedUnknownTypes and return false.
bool ParseOneEvent(const char*& p,
                   const char*  e,
                   const std::string& animationName,
                   ParticleEffectClip& clip,
                   ParticleEffectsBlock& blockStats) {
    if (p >= e || *p != '{') return false;
    ++p;

    clip = ParticleEffectClip{};
    clip.animationName = animationName;

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
            if      (key == "type")       clip.type      = std::move(v);
            else if (key == "filename")   clip.filename  = std::move(v);
            else if (key == "bone_group") clip.boneGroup = std::move(v);
            else if (key == "bone_name")  clip.boneName  = std::move(v);
            // Any other string fields are ignored.
        } else if (p < e && *p == '[') {
            // Array form — `"bone_groups": ["groupA", "groupB"]` — take the
            // first entry and treat it like `"bone_group": "groupA"`. The
            // remaining entries are dropped (Phase 3 stub: one emitter per
            // event, not one per bone_group). Other unknown arrays are
            // skipped entirely.
            if (key == "bone_groups" && clip.boneGroup.empty()) {
                const char* q = p + 1;   // past '['
                while (q < e) {
                    SkipJsonWs(q, e);
                    if (q >= e || *q == ']') break;
                    if (*q == '"' || *q == '\'') {
                        std::string v;
                        ReadJsonString(q, e, v);
                        if (!v.empty() && clip.boneGroup.empty()) {
                            clip.boneGroup = std::move(v);
                        }
                    } else {
                        ++q;
                    }
                }
                SkipJsonValue(p, e);  // advance p past the full array
            } else {
                SkipJsonValue(p, e);
            }
        } else if (p < e && *p == '{') {
            SkipJsonValue(p, e);  // skip unknown nested object
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
    }
    if (p < e) ++p; // past closing '}'

    // Post-conditions: require filename AND recognised type.
    if (clip.filename.empty()) return false;
    if (clip.type != "ParticleEffectEventType") {
        ++blockStats.skippedUnknownTypes;
        return false;
    }
    return true;
}

// Walk the outer nested-format array. For each animation object, extract the
// `events` sub-array slice and parse each event into `out.clips`.
void ParseNestedAnimations(const std::string& jsonBlob,
                           ParticleEffectsBlock& out) {
    const char* p = jsonBlob.c_str();
    const char* e = p + jsonBlob.size();

    SkipJsonWs(p, e);
    if (p < e && *p == '[') ++p;

    while (p < e) {
        SkipJsonWs(p, e);
        if (p >= e || *p == ']') break;
        if (*p != '{') { ++p; continue; }
        ++p;

        std::string animName;
        std::string eventsSlice;

        while (p < e && *p != '}') {
            SkipJsonWs(p, e);
            if (p >= e || *p == '}') break;

            std::string key;
            if (!ReadJsonString(p, e, key)) { if (p < e) ++p; continue; }
            SkipJsonWs(p, e);
            if (p < e && *p == ':') ++p;
            SkipJsonWs(p, e);

            if (key == "name" && p < e && (*p == '"' || *p == '\'')) {
                std::string v;
                ReadJsonString(p, e, v);
                animName = std::move(v);
            } else if (key == "events") {
                eventsSlice = SkipJsonValue(p, e);
            } else {
                SkipJsonValue(p, e);
            }
            SkipJsonWs(p, e);
        }
        if (p < e) ++p;

        if (!eventsSlice.empty() && eventsSlice.front() == '[') {
            const char* q  = eventsSlice.c_str() + 1;            // past '['
            const char* qe = eventsSlice.c_str() + eventsSlice.size();
            while (q < qe) {
                SkipJsonWs(q, qe);
                if (q >= qe || *q == ']') break;
                if (*q != '{') { ++q; continue; }
                ParticleEffectClip clip;
                if (ParseOneEvent(q, qe, animName, clip, out)) {
                    out.clips.push_back(std::move(clip));
                }
                SkipJsonWs(q, qe);
            }
        }
        SkipJsonWs(p, e);
    }
}

// Walk a flat `[ {...}, {...} ]` array (no "animations" wrapper). The entire
// array is attributed to an empty animation name. Used only when the blob is
// not in nested format — rare for ParticleEffects but mirrored from the
// SoundEvents parser for safety.
void ParseFlatAnimations(const std::string& jsonBlob,
                         ParticleEffectsBlock& out) {
    const char* p = jsonBlob.c_str();
    const char* e = p + jsonBlob.size();
    SkipJsonWs(p, e);
    if (p < e && *p == '[') ++p;
    while (p < e) {
        SkipJsonWs(p, e);
        if (p >= e || *p == ']') break;
        if (*p != '{') { ++p; continue; }
        ParticleEffectClip clip;
        if (ParseOneEvent(p, e, /*animName=*/"", clip, out)) {
            out.clips.push_back(std::move(clip));
        }
        SkipJsonWs(p, e);
    }
}

} // anonymous namespace


bool ParseParticleEffects(const AocRawNode&         node,
                          ParticleEffectsBlock&     out,
                          std::vector<std::string>& warnings,
                          std::vector<std::string>& errors) {
    out = {};
    (void)errors;
    (void)warnings;

    // Find the single JSON-blob kv — usual key is "animations"; accept any
    // other child whose value starts with '[' as fallback.
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
        ParseNestedAnimations(*blob, out);
    } else {
        ParseFlatAnimations(*blob, out);
    }

    // Sort clips by (time, animationName) for deterministic iteration.
    std::sort(out.clips.begin(), out.clips.end(),
              [](const ParticleEffectClip& a, const ParticleEffectClip& b) {
                  if (a.time != b.time) return a.time < b.time;
                  return a.animationName < b.animationName;
              });

    return !out.clips.empty();
}

}} // namespace GGPKExp::Parsers
