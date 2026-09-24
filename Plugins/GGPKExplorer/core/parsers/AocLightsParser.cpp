#include "AocLightsParser.h"
#include "AocJsonHelpers.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>

namespace GGPKExp { namespace Parsers {

namespace {

// Parse "a b c" → 3 floats. Missing components → 0. Extra tokens ignored.
// Returns true if at least one component was read.
bool ParseFloat3(const std::string& s, std::array<float, 3>& out) {
    out = { 0.f, 0.f, 0.f };
    std::istringstream iss(s);
    for (int i = 0; i < 3; ++i) {
        if (!(iss >> out[static_cast<std::size_t>(i)])) return i > 0;
    }
    return true;
}

float ParseFloat1(const std::string& s, float def = 0.0f) {
    try { return std::stof(s); } catch (...) { return def; }
}

bool ParseBool(const std::string& s, bool def = false) {
    if (s == "true" || s == "1")  return true;
    if (s == "false" || s == "0") return false;
    return def;
}

LightType MapLightType(const std::string& s) {
    if (s == "spot_light")         return LightType::Spot;
    if (s == "point_light")        return LightType::Point;
    if (s == "directional_light")  return LightType::Directional;
    return LightType::Point;
}

// Read a LightState from the children of a `state = "name" { ... }` node.
LightState ReadLightStateFromChildren(const std::vector<AocRawNode>& body,
                                      const std::string&             name,
                                      LightType                      type) {
    LightState st;
    st.name = name;
    for (const AocRawNode& kv : body) {
        if      (kv.key == "colour")           ParseFloat3(kv.value, st.colour);
        else if (kv.key == "position")         ParseFloat3(kv.value, st.position);
        else if (kv.key == "radius")           st.radius           = ParseFloat1(kv.value, 1.0f);
        else if (kv.key == "penumbra_dist")    st.penumbraDistance = ParseFloat1(kv.value);
        else if (kv.key == "penumbra_radius")  st.penumbraRadius   = ParseFloat1(kv.value);
        else if (type == LightType::Spot) {
            if      (kv.key == "direction")    ParseFloat3(kv.value, st.direction);
            else if (kv.key == "penumbra")     st.penumbraAngleRad = ParseFloat1(kv.value);
            else if (kv.key == "umbra")        st.umbraAngleRad    = ParseFloat1(kv.value);
        }
    }
    return st;
}

// Use shared primitives from AocJsonHelpers.h. Local helpers kept below.
using Json::SkipJsonWs;
using Json::ReadJsonString;
using Json::SkipJsonValue;
using Json::IsNestedAnimationsFormat;

// --- Flat-form event parser ----------------------------------------------
// Consumes a `[ {...}, {...} ]` array of LightStateEvent-style objects.
// Recognises keys "time" (number), "state" / "state_name" (string).
// "type" identifies the event kind and is NOT used as a state reference.
void ParseFlatEvents(const std::string&            jsonBlob,
                     std::vector<LightStateEvent>& out,
                     std::vector<std::string>&     warnings) {
    (void)warnings;
    const char* p = jsonBlob.c_str();
    const char* e = p + jsonBlob.size();

    SkipJsonWs(p, e);
    if (p < e && *p == '[') ++p;
    while (p < e) {
        SkipJsonWs(p, e);
        if (p >= e || *p == ']') break;
        if (*p != '{') { ++p; continue; }
        ++p;

        LightStateEvent ev;
        bool gotState = false;
        bool gotTime  = false;
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
                if (key == "state" || key == "state_name") {
                    ev.stateName = v;
                    gotState = true;
                }
                // "type" and any other string fields are ignored — "type" is the
                // event kind (e.g. "LightStateEventType"), not a state reference.
            } else if (p < e && (*p == '[' || *p == '{')) {
                SkipJsonValue(p, e); // skip unknown nested value
            } else {
                char* endp = nullptr;
                float f = std::strtof(p, &endp);
                if (endp > p) {
                    if (key == "time") { ev.time = f; gotTime = true; }
                    p = endp;
                } else {
                    ++p;
                }
            }
            SkipJsonWs(p, e);
            if (p < e && *p == ',') ++p;
        }
        if (p < e) ++p; // past '}'
        if (gotState && !ev.stateName.empty()) {
            (void)gotTime;   // time is optional (defaults to 0.0)
            out.push_back(ev);
        }
    }
}

// --- Public parser: handles both nested and flat event formats -----------
// Real-world .aoc files nest events inside animation groups:
//   animations = '[{"name":"close","events":[{"type":"LightStateEventType",
//                                             "time":0.0,
//                                             "state_name":"interior_ON"}]}]'
// The legacy/plan-literal form is flat:
//   events = '[{"type":"LightStateEventType","time":0.5,"state":"off"}]'
// Both are accepted. Animation group names are dropped (events are flattened
// into LightDef::events).
void ParseLightEvents(const std::string&            jsonBlob,
                      std::vector<LightStateEvent>& out,
                      std::vector<std::string>&     warnings) {
    if (IsNestedAnimationsFormat(jsonBlob)) {
        // Walk top-level animation group objects, extract each "events": [...]
        // sub-array as a raw slice, and recurse into ParseFlatEvents.
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
                        ParseFlatEvents(inner, out, warnings);
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
    } else {
        ParseFlatEvents(jsonBlob, out, warnings);
    }

    std::sort(out.begin(), out.end(),
              [](const LightStateEvent& a, const LightStateEvent& b) { return a.time < b.time; });
}

// Populate `ld` with one or more sub-properties of a light entry.
// `props` is either:
//   (a) the `children` of a `light = "..."` AocRawNode in real indent-nested
//       .aoc files where casts_shadow/bone_name/state/events appear as deeper
//       indent children of the `light` line, or
//   (b) a single-element vector containing a sibling property (legacy flat
//       form used by plan-generated test data).
// Recognised keys: name, bone_name, casts_shadow, style_speed, default_state,
// state (block), events, animations.
void PopulateLightFromProps(LightDef&                         ld,
                            const std::vector<AocRawNode>&    props,
                            std::vector<std::string>&         warnings) {
    for (const AocRawNode& prop : props) {
        if      (prop.key == "name")          ld.name        = prop.value;
        else if (prop.key == "bone_name")     ld.boneName    = prop.value;
        else if (prop.key == "casts_shadow")  ld.castsShadow = ParseBool(prop.value);
        else if (prop.key == "style_speed")   ld.styleSpeed  = ParseFloat1(prop.value);
        else if (prop.key == "default_state") {
            for (std::size_t i = 0; i < ld.states.size(); ++i) {
                if (ld.states[i].name == prop.value) {
                    ld.defaultStateIndex = static_cast<int>(i);
                    break;
                }
            }
            // If no matching state exists yet the post-pass below clamps invalid indices.
        }
        else if (prop.key == "state") {
            LightState st = ReadLightStateFromChildren(prop.children, prop.value, ld.type);
            ld.states.push_back(std::move(st));
        }
        else if (prop.key == "events" || prop.key == "animations") {
            // Real .aoc files use `animations = '[{"name":...,"events":[...]}]'`
            // (verified in D:/POE2Files/metadata/chests/clamstash/clamstash.aoc).
            // The plan's literal `events = [...]` form is also supported for
            // backward compatibility. ParseLightEvents detects and handles both.
            ParseLightEvents(prop.value, ld.events, warnings);
            // Late-bind stateIndex for each event using the states parsed so far.
            for (LightStateEvent& ev : ld.events) {
                if (ev.stateIndex >= 0) continue;
                for (std::size_t i = 0; i < ld.states.size(); ++i) {
                    if (ld.states[i].name == ev.stateName) {
                        ev.stateIndex = static_cast<int>(i);
                        break;
                    }
                }
            }
        }
    }
}

} // anonymous namespace


// The resolver passes a synthetic AocRawNode whose `children` hold the Lights
// block's top-level properties (one AocRawNode per line inside `Lights { ... }`).
// Each property is itself an AocRawNode: `key` + `value`, with nested body lines
// stored under `children`. So `state = "on" { colour = ... }` arrives as a single
// node with key="state", value="on", children=[{colour,...}, ...].
//
// Real .aoc files use indent-nested format where all light sub-properties
// (casts_shadow, bone_name, state, events, etc.) appear as CHILDREN of the
// `light = "..."` AocRawNode (verified in
// D:/POE2Files/metadata/chests/clamstash/clamstash.aoc). The plan-literal
// form where these are siblings at the Lights block level is also supported
// as a fallback for backward compatibility with synthetic test data.
bool ParseLights(const AocRawNode&            node,
                 LightsBlock&                 out,
                 std::vector<std::string>&    warnings,
                 std::vector<std::string>&    errors) {
    (void)errors;
    out = {};

    const std::vector<AocRawNode>& props = node.children;
    if (props.empty()) return false;

    LightDef*   current          = nullptr;
    bool        currentSuppressed = false; // true if we're inside a skipped directional_light entry

    for (const AocRawNode& prop : props) {
        if (prop.key == "light") {
            const LightType t = MapLightType(prop.value);
            if (t == LightType::Directional) {
                warnings.push_back("Lights: skipping unsupported directional_light ('" +
                                   prop.value + "')");
                out.skippedDirectional++;
                current = nullptr;
                currentSuppressed = true;
                continue;
            }
            out.lights.emplace_back();
            current = &out.lights.back();
            current->type = t;
            currentSuppressed = false;

            // Real .aoc files: all sub-properties are nested under the `light`
            // node as its children. Populate from those children.
            if (!prop.children.empty()) {
                PopulateLightFromProps(*current, prop.children, warnings);
            }
            continue;
        }

        if (currentSuppressed) continue; // skip properties of a directional_light until next `light`
        if (!current) {
            // Property appearing before any `light = ...` header.
            warnings.push_back("Lights: stray property '" + prop.key + "' before first light");
            continue;
        }

        // Backward-compat path: plan-generated flat form where properties are
        // siblings of `light` at the Lights block level.
        PopulateLightFromProps(*current, std::vector<AocRawNode>{prop}, warnings);
    }

    // Post-process: guarantee >=1 state per light and valid defaultStateIndex.
    for (LightDef& ld : out.lights) {
        if (ld.states.empty()) {
            LightState dflt;
            dflt.name = "default";
            ld.states.push_back(std::move(dflt));
            warnings.push_back("Lights: '" + ld.name + "' had no state blocks, synthesised 'default'");
        }
        if (ld.defaultStateIndex < 0 ||
            ld.defaultStateIndex >= static_cast<int>(ld.states.size())) {
            ld.defaultStateIndex = 0;
        }
        // Resolve any not-yet-bound event stateIndexes (events may appear before their state).
        for (LightStateEvent& ev : ld.events) {
            if (ev.stateIndex >= 0) continue;
            for (std::size_t i = 0; i < ld.states.size(); ++i) {
                if (ld.states[i].name == ev.stateName) {
                    ev.stateIndex = static_cast<int>(i);
                    break;
                }
            }
        }
    }

    return !out.lights.empty() || out.skippedDirectional > 0;
}

}} // namespace GGPKExp::Parsers
