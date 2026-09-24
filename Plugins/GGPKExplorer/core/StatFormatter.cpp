#include "StatFormatter.h"

#include <cstdio>
#include <cstdlib>

namespace GGPKExp {

namespace {

void AppendIntFormatted(std::string& out, int32_t value, const std::string& spec) {
    char buf[32];
    if (spec == "+d") {
        std::snprintf(buf, sizeof(buf), "%+d", value);
    } else if (spec.empty() || spec == "d") {
        std::snprintf(buf, sizeof(buf), "%d", value);
    } else {
        // Unknown spec: fall back to plain decimal rather than leaving the
        // placeholder text in place.
        std::snprintf(buf, sizeof(buf), "%d", value);
    }
    out.append(buf);
}

} // namespace

std::string FormatStat(const std::string& fmt, const std::vector<int32_t>& values) {
    std::string out;
    out.reserve(fmt.size() + 16);

    for (size_t i = 0; i < fmt.size(); ++i) {
        char c = fmt[i];

        if (c == '{') {
            size_t close = fmt.find('}', i);
            if (close == std::string::npos) {
                out.push_back(c);
                continue;
            }
            std::string spec = fmt.substr(i + 1, close - i - 1);
            i = close;

            // Parse: <index>[:<modifier>]
            size_t colon = spec.find(':');
            std::string idxPart = (colon == std::string::npos) ? spec : spec.substr(0, colon);
            std::string modPart = (colon == std::string::npos) ? std::string() : spec.substr(colon + 1);

            int idx = -1;
            try { idx = std::stoi(idxPart); } catch (...) { idx = -1; }
            if (idx < 0 || idx >= static_cast<int>(values.size())) {
                // Out-of-range index: drop the placeholder silently.
                continue;
            }
            AppendIntFormatted(out, values[static_cast<size_t>(idx)], modPart);
            continue;
        }

        if (c == '[') {
            size_t close = fmt.find(']', i);
            if (close == std::string::npos) {
                out.push_back(c);
                continue;
            }
            std::string inner = fmt.substr(i + 1, close - i - 1);
            i = close;

            size_t pipe = inner.find('|');
            if (pipe != std::string::npos) {
                out.append(inner.substr(pipe + 1));
            } else {
                out.append(inner);
            }
            continue;
        }

        out.push_back(c);
    }

    return out;
}

} // namespace GGPKExp
