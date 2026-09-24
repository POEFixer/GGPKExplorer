#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <d3d11.h>
#include <wrl/client.h>
#include "../../core/parsers/SmdParser.h"
#include "MeshCamera.h"
#include "MeshMaterial.h"

namespace GGPKExp {

// GPU-side light descriptor. 64 bytes (4 x float4) — keeps CBData 16-byte aligned.
// Encoding:
//   posRadius.xyz  : world-space position
//   posRadius.w    : radius (linear falloff end)
//   dirCone.xyz    : world-space direction (unit; zero for point lights)
//   dirCone.w      : cos(penumbraHalfAngle) — <0 flags "point light" (no cone test)
//   colorInner.xyz : HDR colour
//   colorInner.w   : cos(umbraHalfAngle) — fully-lit cone edge; unused for point
//   params.x       : penumbra_dist  (inner radius at which falloff begins)
//   params.y       : penumbra_radius (outer radius at which falloff ends; can exceed radius)
//   params.z       : castsShadow flag (1.f for dominant caster, else 0.f)
//   params.w       : reserved
struct ShaderLight {
    std::array<float, 4> posRadius  { 0, 0, 0, 0 };
    std::array<float, 4> dirCone    { 0, 0, -1, -1 };
    std::array<float, 4> colorInner { 0, 0, 0, 0 };
    std::array<float, 4> params     { 0, 0, 0, 0 };
};

static constexpr int kMaxShaderLights = 8;

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
    /// by MVP/World) and `SkinEnabled` is forced true for this draw call.
    /// When `skinningMatrices == nullptr` or `boneCount == 0`, the bone
    /// constant buffer is filled with identity and the legacy `hasSkinning`
    /// flag governs shader-side skinning.
    void RenderScene(int w, int h,
                     MeshCamera& camera,
                     MeshMaterial& material,
                     const Parsers::SmdMesh& mesh,
                     int renderMode,
                     bool hasSkinning,
                     const float* skinningMatrices = nullptr,
                     std::size_t boneCount = 0);

    /// Phase 4/5.2 — additive child-mesh render pass.
    ///
    /// Draws a child mesh into the SAME offscreen RT populated by the most
    /// recent RenderScene() call, WITHOUT clearing. Intended to be called
    /// zero or more times after RenderScene() each frame.
    ///
    /// Two modes (selected by the trailing two parameters):
    ///
    ///   RIGID (skinningMatrices == nullptr || boneCount == 0):
    ///     Identity bone palette, SkinEnabled = 0.0, MVP uses `childWorld`.
    ///     Child vertices transformed rigidly by the socket world matrix.
    ///     This is the Phase 4 default and preserves binary compat.
    ///
    ///   SKINNED (skinningMatrices != nullptr && boneCount > 0):
    ///     Uploads `boneCount` matrices (transposed like RenderScene), SkinEnabled
    ///     = 1.0, MVP uses identity × view × proj. The palette is expected
    ///     to already place vertices in parent-world space (caller composes
    ///     `palette[i] = child.bindInverse[i] × parentBone.worldTransform`),
    ///     so `childWorld` is ignored for MVP in this path — but still used
    ///     at the call site for the unmatched-bone fallback (see
    ///     AocViewer_3DPreview.cpp).
    ///
    /// renderMode: 0 = solid, 1 = wireframe, 2 = both (matches RenderScene).
    /// Returns false if the RT / shaders are not ready (non-fatal).
    bool RenderAdditionalChildMesh(MeshCamera& camera,
                                   MeshMaterial& childMaterial,
                                   const Parsers::SmdMesh& childMesh,
                                   const float childWorld[16],
                                   int renderMode,
                                   const float* skinningMatrices = nullptr,
                                   std::size_t boneCount = 0);

    /// Get the SRV for the rendered texture (to display via ImGui::Image)
    ID3D11ShaderResourceView* GetRenderTargetSRV() const { return m_srv.Get(); }

    /// Overwrite the multi-light block before the next RenderScene call. Extra slots
    /// are zero-filled by the callee; lightCount is clamped to kMaxShaderLights.
    void UpdateLights(const ShaderLight* lights, int count);
    void UpdateSunDir(const std::array<float, 3>& dir);

    /// Release GPU mesh buffers and reset vertex/index counts so the next
    /// RenderScene() treats this viewer as "no mesh". Safe to call when
    /// buffers are already empty. Used by MeshViewer::Clear() to prevent
    /// stale buffers from being drawn with a freshly-loaded skeleton.
    void ClearMeshBuffers();

    // ----- Accessors -----
    uint32_t GetIndexCount() const { return m_indexCount; }
    uint32_t GetVertexCount() const { return m_vertexCount; }
    bool Use32BitIndices() const { return m_use32BitIndices; }
    int GetRenderTargetWidth() const { return m_rtWidth; }
    int GetRenderTargetHeight() const { return m_rtHeight; }
    bool AreShadersReady() const { return m_shadersReady; }
    ID3D11DeviceContext* GetContext() const { return m_context.Get(); }

    float m_bgColor[3] = {0.12f, 0.13f, 0.18f};

private:
    // Constant buffer layout (must be 16-byte aligned).
    // Phase 2 multi-light layout — mirror MeshShaders.h cbuffer Constants.
    //
    // NOTE: SkinEnabled re-uses the first pad slot after lightCount so the VS
    // can still read it; the VS cbuffer declares that slot as `float SkinEnabled`.
    struct alignas(16) CBData {
        float MVP[16];
        float World[16];
        float BaseColor[4];
        float CameraPosOpaque[4];  // xyz = cam pos, w = opaque flag (1.0 opaque, 0.0 texture alpha)
        float LightVP[16];

        // Phase 2 multi-light block (8 * 64 = 512 bytes)
        ShaderLight lights[kMaxShaderLights];
        int   lightCount;
        float SkinEnabled;  // 1.0 = apply skinning, 0.0 = pass-through (re-used pad slot)
        int   _pad1;
        int   _pad2;

        // Sun fallback (replaces old LightDir slot). Used when no Lights block,
        // or as supplementary hemisphere-ambient direction.
        float SunDir[4];  // xyz = dir, w unused
    };

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

    // Shadow map
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_shadowTex;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_shadowDSV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_shadowSRV;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_shadowSampler;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_shadowVS;
    static constexpr int kShadowMapSize = 2048;

    uint32_t m_indexCount = 0;
    uint32_t m_vertexCount = 0;
    bool m_use32BitIndices = false;
    bool m_shadersReady = false;

    // CPU-side shadow of the multi-light block. UpdateLights()/UpdateSunDir()
    // write here; RenderScene() copies these fields into the actual CBData
    // upload on each draw. Everything else (MVP, World, BaseColor, cameras,
    // LightVP, SkinEnabled, opaque flag) is computed per-frame inside
    // RenderScene() and left untouched here.
    CBData m_cbState{};
};

} // namespace GGPKExp
