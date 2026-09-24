#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <imgui.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace GGPKExp {

class GGPKManager;

// Eagerly loads every unique DDS icon referenced by the PSG viewer and hands
// out ImGui texture IDs by path. The cache owns the SRVs; clear() / destructor
// releases them.
class PsgIconCache {
public:
    void Initialize(ID3D11Device* device, GGPKManager* manager);

    // Load the given GGPK paths into GPU textures. Duplicates are skipped
    // automatically. Missing or broken files are silently dropped.
    void PreloadAll(const std::vector<std::string>& ggpkPaths);

    // Returns an ImTextureID suitable for ImGui::Image / AddImage, or 0 if the
    // icon was not loaded.
    ImTextureID Get(const std::string& ggpkPath) const;

    // Texture dimensions as they were decoded; both zero when unknown.
    void GetSize(const std::string& ggpkPath, int& outW, int& outH) const;

    void Clear();

    size_t LoadedCount() const { return m_textures.size(); }

    // Loads a single icon from the given GGPK path. Duplicates skipped.
    // Exposed (not private) so callers can rate-limit icon loading by
    // pumping one-at-a-time from the render loop.
    bool LoadOne(const std::string& ggpkPath);

    // Attempts to load one icon; catches SEH exceptions (NVIDIA driver crashes).
    // Returns false on failure (not loaded, or crash caught). Safe to call repeatedly.
    bool LoadOneSafe(const std::string& ggpkPath);

    // Returns total loaded so far; used for progress display
    size_t Size() const { return m_textures.size(); }

private:
    struct Entry {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        int width = 0;
        int height = 0;
    };

    ID3D11Device* m_device = nullptr;
    GGPKManager* m_manager = nullptr;
    // Keyed by lowercase path for case-insensitive lookup.
    std::unordered_map<std::string, Entry> m_textures;

    static std::string NormalizeKey(const std::string& path);
};

} // namespace GGPKExp
