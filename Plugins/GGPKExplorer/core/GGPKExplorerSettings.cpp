#include "GGPKExplorerSettings.h"

#include "../lib/json.hpp"

#include <filesystem>
#include <fstream>

namespace GGPKExp {

namespace {
std::filesystem::path SettingsFilePath(const std::filesystem::path& pluginDir) {
    return pluginDir / "config" / "ui.json";
}
} // anonymous

bool LoadUiSettings(const std::filesystem::path& pluginDir, GGPKExplorerUiSettings& out) {
    const auto path = SettingsFilePath(pluginDir);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return false;

    std::ifstream in(path);
    if (!in.good()) return false;

    nlohmann::json j;
    try {
        in >> j;
    } catch (...) {
        return false;
    }

    // Defensive: missing/wrong-type fields fall back to defaults.
    if (j.contains("animPanel") && j["animPanel"].is_object()) {
        const auto& ap = j["animPanel"];
        if (ap.contains("collapsed") && ap["collapsed"].is_boolean())
            out.animPanel.collapsed = ap["collapsed"].get<bool>();
        if (ap.contains("width") && ap["width"].is_number())
            out.animPanel.width = ap["width"].get<float>();
        if (ap.contains("followModel") && ap["followModel"].is_boolean())
            out.animPanel.followModel = ap["followModel"].get<bool>();
    }
    if (j.contains("playbackSpeed") && j["playbackSpeed"].is_number())
        out.playbackSpeed = j["playbackSpeed"].get<float>();
    if (j.contains("statDescLanguage") && j["statDescLanguage"].is_string())
        out.statDescLanguage = j["statDescLanguage"].get<std::string>();
    if (j.contains("meshView") && j["meshView"].is_object()) {
        const auto& mv = j["meshView"];
        if (mv.contains("showGrid") && mv["showGrid"].is_boolean())
            out.meshView.showGrid = mv["showGrid"].get<bool>();
        if (mv.contains("showBones") && mv["showBones"].is_boolean())
            out.meshView.showBones = mv["showBones"].get<bool>();
    }

    // Clamp to sane ranges.
    if (out.animPanel.width < 120.0f)  out.animPanel.width = 120.0f;
    if (out.animPanel.width > 800.0f)  out.animPanel.width = 800.0f;
    if (out.playbackSpeed < 0.5f)      out.playbackSpeed = 0.5f;
    if (out.playbackSpeed > 25.0f)     out.playbackSpeed = 25.0f;

    return true;
}

bool SaveUiSettings(const std::filesystem::path& pluginDir, const GGPKExplorerUiSettings& in) {
    const auto path = SettingsFilePath(pluginDir);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;

    nlohmann::json j;
    j["version"] = 1;
    j["animPanel"] = {
        {"collapsed",   in.animPanel.collapsed},
        {"width",       in.animPanel.width},
        {"followModel", in.animPanel.followModel},
    };
    j["playbackSpeed"] = in.playbackSpeed;
    j["statDescLanguage"] = in.statDescLanguage;
    j["meshView"] = {
        {"showGrid",     in.meshView.showGrid},
        {"showBones",    in.meshView.showBones},
    };

    std::ofstream out(path, std::ios::trunc);
    if (!out.good()) return false;
    out << j.dump(2);
    return out.good();
}

} // namespace GGPKExp
