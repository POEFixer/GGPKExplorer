#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <d3d11.h>
#include <wrl/client.h>
#include "../../core/parsers/SmParser.h"

namespace GGPKExp {

class GGPKManager;

/// Per-material blend mode for the 3D preview.
/// Opaque: standard rendering, alpha output forced to 1.0.
/// AlphaBlend: SrcAlpha / InvSrcAlpha blend, texture alpha respected.
enum class BlendMode : uint8_t {
    Opaque     = 0,
    AlphaBlend = 1,
};

/// Per-material diffuse texture for a submesh
struct MaterialTex {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;       // diffuse/albedo
    std::string matPath;
    std::string ddsPath;
    bool loaded = false;
    BlendMode blendMode = BlendMode::Opaque;
};

/// Manages loading and caching of mesh material textures from GGPK archives.
class MeshMaterial {
public:
    void Initialize(ID3D11Device* device);
    void Clear();

    /// Create the 1x1 white fallback texture
    bool CreateFallbackTexture();

    /// Legacy single-texture path: the first .mat named in the .sm text, its
    /// diffuse .dds replacing the white fallback texture. Used when the
    /// per-submesh materials did not load.
    bool LoadTextureFromSm(const std::vector<uint8_t>& smData, GGPKManager* mgr);

    /// Load all materials referenced by `.sm`, expanding run-length counts into a
    /// per-submesh `m_materialTextures` array of length `submeshCount`.
    /// Extra run-length (DOLm case) is safely truncated; shortage leaves trailing
    /// submeshes with default (unloaded) MaterialTex.
    void LoadAllMaterials(const Parsers::SmData& smData, size_t submeshCount, GGPKManager* mgr);

    // ----- Accessors -----

    /// The legacy texture replaced the white fallback.
    bool HasTexture() const { return m_hasTexture; }
    const std::string& FallbackDdsPath() const { return m_fallbackDdsPath; }
    const std::vector<MaterialTex>& GetMaterials() const { return m_materialTextures; }

    ID3D11ShaderResourceView* GetFallbackSRV() const { return m_texSRV.Get(); }

private:
    ID3D11Device* m_device = nullptr;  // non-owning, lifetime guaranteed by host (ImGui D3D device)

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_texSRV;  // fallback (white, or the legacy texture)
    std::vector<MaterialTex> m_materialTextures;
    bool m_hasTexture = false;
    std::string m_fallbackDdsPath;

    /// .mat -> its diffuse .dds on the GPU (nullptr when any step fails).
    /// outBlend is set once the .mat parses, even when its texture does not load.
    ID3D11ShaderResourceView* LoadTextureFromMat(const std::string& matPath,
                                                  GGPKManager* mgr, std::string& outDdsPath,
                                                  BlendMode& outBlend);
};

} // namespace GGPKExp
