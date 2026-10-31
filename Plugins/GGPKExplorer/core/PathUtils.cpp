#include "PathUtils.h"
#include <algorithm>
#include <cctype>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace GGPKExp::PathUtils {

std::string ToLower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::filesystem::path FromUtf8(const std::string& utf8) {
    return std::filesystem::path(Utf8ToWide(utf8));
}

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::vector<std::string_view> Split(std::string_view path) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();
        if (end > start)
            parts.push_back(path.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

bool Equals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

bool StartsWith(const std::string& path, const std::string& prefix) {
    if (prefix.size() > path.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(path[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

} // namespace GGPKExp::PathUtils
