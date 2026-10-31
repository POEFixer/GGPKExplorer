#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <d3d11.h>
#include <wrl/client.h>
#include "../../core/parsers/SmdParser.h"
#include "MeshCamera.h"
#include "MeshMaterial.h"

namespace GGPKExp {

/// Manages all D3D11 GPU resources and rendering for the mesh viewer.
class MeshRenderer {
public:
    ~MeshRenderer();

    bool Initialize(ID3D11Device* device);
    void ReleaseResources();

    /// Create vertex and index buffers from parsed mesh data.
    /// Returns false on failure (sets error string).
    bool CreateMeshBuffers(const Parsers::SmdMesh& mesh, std::string& error);

    /// Render the scene to an offscreen render target.
    ///
    /// `skinningMatrices` (optional): pointer to a row-major palette of
    /// `boneCount` 4x4 matrices (16 floats each), matching the row-vector
    /// convention `palette[i] = bindInverse[i] * worldTransform[i]` produced
    /// by `BuildSkinningPalette`. When provided, matrices are transposed on
    /// upload (to match the project-wide column-major HLSL convention used
    /// by MVP/World) and the vertices are skinned. Without a palette the
    /// mesh draws unskinned.
    ///
    /// renderMode: 0 = solid, 1 = wireframe, 2 = both.
    void RenderScene(int w, int h,
                     MeshCamera& camera,
                     MeshMaterial& material,
                     const Parsers::SmdMesh& mesh,
                     int renderMode,
                     const float* skinningMatrices = nullptr,
                     std::size_t boneCount = 0);

    /// Get the SRV for the rendered texture (to display via ImGui::Image)
    ID3D11ShaderResourceView* GetRenderTargetSRV() const { return m_srv.Get(); }

    /// Release GPU mesh buffers and reset vertex/index counts so the next
    /// RenderScene() treats this viewer as "no mesh". Safe to call when
    /// buffers are already empty. Used by MeshViewer::Clear() to prevent
    /// stale buffers from being drawn with a freshly-loaded skeleton.
    void ClearMeshBuffers();

    // ----- Accessors -----
    uint32_t GetIndexCount() const { return m_indexCount; }
    uint32_t GetVertexCount() const { return m_vertexCount; }
    bool AreShadersReady() const { return m_shadersReady; }

private:
    // Constant buffer layout (16-byte aligned) — mirror of the cbuffer
    // Constants in MeshShaders.h.
    struct alignas(16) CBData {
        float MVP[16];
        float World[16];
        float BaseColor[4];
        float LightDir[4];    // xyz = direction the light travels
        float Opaque;         // 1.0 opaque, 0.0 texture alpha
        float SkinEnabled;    // 1.0 = apply skinning, 0.0 = pass-through
        float _pad[2];
    };
    static_assert(sizeof(CBData) == 176, "CBData must match the HLSL cbuffer Constants");

    // Interleaved vertex: position, normal, uv, bone indices, bone weights.
    static constexpr unsigned kVertexStride = 40;

    bool CompileShaders();
    bool CreateRenderTarget(int w, int h);

    // Device & context
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;

    // Render target
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_dsv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_renderTarget;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthBuffer;
    int m_rtWidth = 0, m_rtHeight = 0;

    // Shaders
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_psSolid;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_psWire;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_layout;

    // Mesh buffers
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_ib;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_boneCB;

    // Pipeline states
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rsSolid;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rsWire;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_dss;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendOpaque;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendAlpha;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;

    uint32_t m_indexCount = 0;
    uint32_t m_vertexCount = 0;
    bool m_use32BitIndices = false;
    bool m_shadersReady = false;

    static constexpr float kBgColor[3] = {0.12f, 0.13f, 0.18f};
};

} // namespace GGPKExp
