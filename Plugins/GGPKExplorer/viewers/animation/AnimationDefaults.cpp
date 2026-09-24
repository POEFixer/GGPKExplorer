#include "AnimationDefaults.h"

#include <cctype>
#include <cstring>

namespace GGPKExp { namespace Animation {

bool IEquals(const std::string& a, const char* b) {
    const size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

bool IStartsWith(const std::string& a, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (a.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cp = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(ca) != std::tolower(cp)) return false;
    }
    return true;
}

std::size_t PickDefaultAnimIndex(const std::vector<std::string>& names) {
    for (std::size_t i = 0; i < names.size(); ++i)
        if (IEquals(names[i], "idle")) return i;
    for (std::size_t i = 0; i < names.size(); ++i)
        if (IStartsWith(names[i], "idle")) return i;
    return names.empty() ? static_cast<std::size_t>(-1) : 0;
}

}} // namespace GGPKExp::Animation
