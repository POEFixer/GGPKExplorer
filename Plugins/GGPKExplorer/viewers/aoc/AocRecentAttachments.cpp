#include "AocRecentAttachments.h"

#include "../../lib/json.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace GGPKExp {
namespace Aoc {

bool AocRecentAttachments::Load(const std::string& pluginDir) {
    m_entries.clear();

    const std::string jsonPath = DeriveJsonPath(pluginDir);
    if (jsonPath.empty()) return true;

    const std::filesystem::path p(jsonPath);
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return true;

    std::ifstream in(p);
    if (!in.good()) return false;

    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception&) {
        return false;
    }

    // Schema version tolerance (v1 only for now).
    if (j.contains("version") && j["version"].is_number_integer()) {
        const int v = j["version"].get<int>();
        if (v > 1) {
            // Best-effort parse, but ignore unknown fields.
        }
    }

    if (j.contains("entries") && j["entries"].is_array()) {
        for (const auto& e : j["entries"]) {
            if (!e.is_object()) continue;
            Entry entry;
            if (e.contains("sourcePath") && e["sourcePath"].is_string()) {
                entry.sourcePath = ToLowerPath(e["sourcePath"].get<std::string>());
            }
            if (e.contains("lastUsedUnix") && e["lastUsedUnix"].is_number_integer()) {
                entry.lastUsedUnix = e["lastUsedUnix"].get<std::int64_t>();
            }
            if (entry.sourcePath.empty()) continue;
            m_entries.push_back(std::move(entry));
        }
    }

    if (static_cast<int>(m_entries.size()) > kMaxEntries) {
        m_entries.resize(kMaxEntries);
    }
    return true;
}

void AocRecentAttachments::Save(const std::string& pluginDir) const {
    const std::string jsonPath = DeriveJsonPath(pluginDir);
    if (jsonPath.empty()) return;

    nlohmann::json j;
    j["version"]    = 1;
    j["maxEntries"] = kMaxEntries;

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : m_entries) {
        nlohmann::json obj;
        obj["sourcePath"]   = e.sourcePath;
        obj["lastUsedUnix"] = static_cast<std::int64_t>(e.lastUsedUnix);
        arr.push_back(obj);
    }
    j["entries"] = arr;

    const std::filesystem::path p(jsonPath);
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    if (ec) return;

    std::ofstream out(p, std::ios::trunc);
    if (!out.good()) return;
    out << j.dump(2);
}

void AocRecentAttachments::Record(const std::string& sourcePath) {
    if (sourcePath.empty()) return;
    const std::string key = ToLowerPath(sourcePath);

    auto it = std::find_if(m_entries.begin(), m_entries.end(),
        [&](const Entry& e) { return e.sourcePath == key; });
    if (it != m_entries.end()) {
        m_entries.erase(it);
    }

    Entry fresh;
    fresh.sourcePath   = key;
    fresh.lastUsedUnix = std::time(nullptr);
    m_entries.insert(m_entries.begin(), std::move(fresh));

    if (static_cast<int>(m_entries.size()) > kMaxEntries) {
        m_entries.resize(kMaxEntries);
    }
}

void AocRecentAttachments::Remove(const std::string& sourcePath) {
    const std::string key = ToLowerPath(sourcePath);
    m_entries.erase(
        std::remove_if(m_entries.begin(), m_entries.end(),
            [&](const Entry& e) { return e.sourcePath == key; }),
        m_entries.end());
}

void AocRecentAttachments::Clear() {
    m_entries.clear();
}

std::string AocRecentAttachments::DeriveJsonPath(const std::string& pluginDir) {
    if (pluginDir.empty()) return {};
    std::filesystem::path p(pluginDir);
    p /= "user_data";
    p /= "recent_attachments.json";
    return p.string();
}

std::string AocRecentAttachments::ToLowerPath(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace Aoc
} // namespace GGPKExp
