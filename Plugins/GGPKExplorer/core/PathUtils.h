#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

namespace GGPKExp::PathUtils {

std::vector<std::string_view> Split(std::string_view path);
bool Equals(const std::string& a, const std::string& b);
bool StartsWith(const std::string& path, const std::string& prefix);
std::string ToLower(const std::string& str);

// Build a filesystem path from a UTF-8 string (Unicode-correct on Windows;
// std::filesystem::path(std::string) would otherwise use the ANSI codepage).
std::filesystem::path FromUtf8(const std::string& utf8);

// UTF-16 <-> UTF-8 (CP_UTF8, never the ANSI code page): Win32 dialogs and paths.
std::string WideToUtf8(std::wstring_view wide);
std::wstring Utf8ToWide(std::string_view utf8);

} // namespace GGPKExp::PathUtils
