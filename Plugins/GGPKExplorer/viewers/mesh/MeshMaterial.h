#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <d3d11.h>
#include <wrl/client.h>
#include "../../core/parsers/SmParser.h"

namespace GGPKExp {

class GGPKManager;
class VirtualTree;

/// Per-material blend mode for the 3D preview.
/// Opaque: standard rendering, alpha output forced to 1.0.
/// AlphaBlend: SrcAlpha / InvSrcAlpha blend, texture alpha respected.
enum class BlendMode : uint8_t {
    Opaque     = 0,
    AlphaBlend = 1,
};

/// Per-material texture data for a submesh
struct MaterialTex {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;       // diffuse/albedo
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> normalSrv; // normal map
    std::string matPath;
    std::string ddsPath;
    std::string normalDdsPath;
    bool loaded = false;
    BlendMode blendMode = BlendMode::Opaque;
    bool normalLoaded = false;
};

/// Structured texture pipeline info for the Info tab
struct TexLoadInfo {
    std::string matPath;
    bool matFound = false;
    size_t matSize = 0;
    std::string matJsonError;
    std::vector<std::string> allDdsPaths;
    std::string selectedDdsPath;
    bool ddsFound = false;
    size_t ddsSize = 0;
    bool gpuLoaded = false;
    std::string gpuError;
};

/// Manages loading and caching of mesh material textures from GGPK archives.
class MeshMaterial {
public:
    void Initialize(ID3D11Device* device);
    void Clear();

    /// Create the 1x1 white fallback texture and 1x1 flat normal fallback
    bool CreateFallbackTextures();

    /// Load texture from .sm -> .mat -> .dds chain (legacy single-texture path)
    bool LoadTextureFromSm(const std::vector<uint8_t>& smData,
                           GGPKManager* mgr, VirtualTree* tree);

    /// Load all materials referenced by `.sm`, expanding run-length counts into a
    /// per-submesh `m_materialTextures` array of length `submeshCount`.
    /// Extra run-length (DOLm case) is safely truncated; shortage leaves trailing
    /// submeshes with default (unloaded) MaterialTex.
    void LoadAllMaterials(const Parsers::SmData& smData, size_t submeshCount, GGPKManager* mgr);

    // ----- Accessors -----

    bool HasTexture() const { return m_hasTexture; }
    const std::vector<MaterialTex>& GetMaterials() const { return m_materialTextures; }

    ID3D11ShaderResourceView* GetFallbackSRV() const { return m_texSRV.Get(); }
    ID3D11ShaderResourceView* GetFlatNormalSRV() const { return m_flatNormalSRV.Get(); }

    const TexLoadInfo& GetTexInfo() const { return m_texInfo; }
    const std::string& GetTexLog() const { return m_texLog; }

private:
    ID3D11Device* m_device = nullptr;  // non-owning, lifetime guaranteed by host (ImGui D3D device)

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_texSRV;       // fallback white texture
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_flatNormalSRV; // flat normal fallback
    std::vector<MaterialTex> m_materialTextures;
    bool m_hasTexture = false;

    TexLoadInfo m_texInfo;
    std::string m_texLog;

    bool CreateWhiteTexture();
    bool CreateFlatNormalTexture();
    ID3D11ShaderResourceView* LoadTextureFromMat(const std::string& matPath,
                                                  GGPKManager* mgr, TexLoadInfo& info,
                                                  BlendMode& outBlend);
};

} // namespace GGPKExp
