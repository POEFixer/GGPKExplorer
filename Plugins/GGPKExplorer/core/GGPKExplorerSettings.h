#pragma once

#include <string>
#include <filesystem>

namespace GGPKExp {

// Panel state that persists between sessions.
// Owned by the plugin, used by the model view (MeshViewer).
struct AnimationPanelState {
    bool  collapsed   = false;
    float width       = 280.0f;   // expanded-state width in pixels
    bool  followModel = false;    // camera orbit pivot tracks the model's root bone
};

// 3D view toggles shared by every MeshViewer session. Survives model
// switches (user's choice of Grid/Bones is remembered).
struct MeshViewSettings {
    bool showGrid     = false;
    bool showBones    = false;
};

struct GGPKExplorerUiSettings {
    AnimationPanelState animPanel{};
    float               playbackSpeed = 10.0f;   // backend; label "1.0x" × 10
    MeshViewSettings    meshView{};
    // Language selector for CSD stat descriptions in the PSG viewer.
    // "English" when the .csd file's default (untagged) block should be used.
    // Any lang name as stored in .csd ("French", "Russian", "Traditional
    // Chinese", etc.) — the PSG viewer falls back to English if the selected
    // language is missing a particular stat_id.
    std::string         statDescLanguage = "English";
};

// Base directory for the settings file. Pass the plugin directory (from
// IPlugin::SetPluginDirectory). File path = `{pluginDir}/config/ui.json`.
//
// Returns true if file existed AND parsed successfully; false otherwise.
// On false, `out` keeps default-constructed values.
// pluginDir comes from Plugin::DirectoryPath() — already converted UTF-8 → wide.
// Passing the UTF-8 std::string from Directory() through fs::path on Windows
// would corrupt non-ASCII chars (system ANSI codepage interpretation).
bool LoadUiSettings(const std::filesystem::path& pluginDir, GGPKExplorerUiSettings& out);

// Writes settings to disk. Creates `{pluginDir}/config/` directory if missing.
// Returns true on success.
bool SaveUiSettings(const std::filesystem::path& pluginDir, const GGPKExplorerUiSettings& in);

} // namespace GGPKExp
