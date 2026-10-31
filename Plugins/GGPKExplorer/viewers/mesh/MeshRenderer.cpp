#include "MeshRenderer.h"
#include "../MeshShaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace GGPKExp {

// -----------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------

MeshRenderer::~MeshRenderer() {
    ReleaseResources();
}

void MeshRenderer::ReleaseResources() {
    m_vb.Reset();
    m_ib.Reset();
    m_cb.Reset();
    m_boneCB.Reset();
    m_vs.Reset();
    m_psSolid.Reset();
    m_psWire.Reset();
    m_layout.Reset();
    m_rsSolid.Reset();
    m_rsWire.Reset();
    m_dss.Reset();
    m_blendOpaque.Reset();
    m_blendAlpha.Reset();
    m_sampler.Reset();
    m_rtv.Reset();
    m_dsv.Reset();
    m_srv.Reset();
    m_renderTarget.Reset();
    m_depthBuffer.Reset();
    m_context.Reset();
    m_device.Reset();
    m_rtWidth = 0;
    m_rtHeight = 0;
    m_indexCount = 0;
    m_vertexCount = 0;
    m_shadersReady = false;
}

void MeshRenderer::ClearMeshBuffers() {
    m_vb.Reset();
    m_ib.Reset();
    m_vertexCount = 0;
    m_indexCount = 0;
    m_use32BitIndices = false;
}

bool MeshRenderer::Initialize(ID3D11Device* device) {
    if (!device) return false;
    // Store device (AddRef via ComPtr)
    m_device = device;
    device->GetImmediateContext(m_context.ReleaseAndGetAddressOf());
    return CompileShaders();
}

// -----------------------------------------------------------------------
// Shader compilation
// -----------------------------------------------------------------------

bool MeshRenderer::CompileShaders() {
    if (!m_device || !m_context) return false;

    HRESULT hr;
    std::string error;

    // --- Vertex shader ---
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* errBlob = nullptr;
    hr = D3DCompile(g_meshVertexShaderSrc,
                    strlen(g_meshVertexShaderSrc),
                    "MeshVS", nullptr, nullptr,
                    "main", "vs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &vsBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) errBlob->Release();
        return false;
    }
    if (errBlob) errBlob->Release();

    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                      vsBlob->GetBufferSize(),
                                      nullptr, m_vs.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        vsBlob->Release();
        return false;
    }

    // --- Input layout (kVertexStride bytes per vertex) ---
    D3D11_INPUT_ELEMENT_DESC layoutDesc[5] = {};
    layoutDesc[0].SemanticName = "POSITION";
    layoutDesc[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    layoutDesc[0].AlignedByteOffset = 0;
    layoutDesc[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;

    layoutDesc[1].SemanticName = "NORMAL";
    layoutDesc[1].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    layoutDesc[1].AlignedByteOffset = 12;
    layoutDesc[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;

    layoutDesc[2].SemanticName = "TEXCOORD";
    layoutDesc[2].Format = DXGI_FORMAT_R32G32_FLOAT;
    layoutDesc[2].AlignedByteOffset = 24;
    layoutDesc[2].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;

    layoutDesc[3].SemanticName = "BLENDINDICES";
    layoutDesc[3].Format = DXGI_FORMAT_R8G8B8A8_UINT;
    layoutDesc[3].AlignedByteOffset = 32;
    layoutDesc[3].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;

    layoutDesc[4].SemanticName = "BLENDWEIGHT";
    layoutDesc[4].Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    layoutDesc[4].AlignedByteOffset = 36;
    layoutDesc[4].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;

    hr = m_device->CreateInputLayout(layoutDesc, 5,
                                     vsBlob->GetBufferPointer(),
                                     vsBlob->GetBufferSize(),
                                     m_layout.ReleaseAndGetAddressOf());
    vsBlob->Release();
    if (FAILED(hr)) return false;

    // --- Solid pixel shader ---
    ID3DBlob* psBlob = nullptr;
    errBlob = nullptr;
    hr = D3DCompile(g_meshPixelShaderSrc,
                    strlen(g_meshPixelShaderSrc),
                    "MeshPS", nullptr, nullptr,
                    "main", "ps_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &psBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) errBlob->Release();
        return false;
    }
    if (errBlob) errBlob->Release();

    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(),
                                     psBlob->GetBufferSize(),
                                     nullptr, m_psSolid.ReleaseAndGetAddressOf());
    psBlob->Release();
    if (FAILED(hr)) return false;

    // --- Wireframe pixel shader ---
    psBlob = nullptr;
    errBlob = nullptr;
    hr = D3DCompile(g_wirePixelShaderSrc,
                    strlen(g_wirePixelShaderSrc),
                    "WirePS", nullptr, nullptr,
                    "main", "ps_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &psBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) errBlob->Release();
        return false;
    }
    if (errBlob) errBlob->Release();

    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(),
                                     psBlob->GetBufferSize(),
                                     nullptr, m_psWire.ReleaseAndGetAddressOf());
    psBlob->Release();
    if (FAILED(hr)) return false;

    // --- Constant buffer ---
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(CBData);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_device->CreateBuffer(&cbDesc, nullptr, m_cb.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    // --- Bone matrix constant buffer (kMaxSkinBones * 4x4 float, currently 256 * 64 = 16384 bytes) ---
    {
        D3D11_BUFFER_DESC boneDesc = {};
        boneDesc.ByteWidth = kMaxSkinBones * 64;
        boneDesc.Usage = D3D11_USAGE_DYNAMIC;
        boneDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        boneDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = m_device->CreateBuffer(&boneDesc, nullptr, m_boneCB.ReleaseAndGetAddressOf());
        if (FAILED(hr)) return false;
    }

    // --- Rasterizer states ---
    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.FrontCounterClockwise = FALSE;
    rsDesc.DepthClipEnable = TRUE;
    hr = m_device->CreateRasterizerState(&rsDesc, m_rsSolid.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    rsDesc.FillMode = D3D11_FILL_WIREFRAME;
    rsDesc.CullMode = D3D11_CULL_NONE;
    hr = m_device->CreateRasterizerState(&rsDesc, m_rsWire.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    // --- Depth stencil state ---
    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc = D3D11_COMPARISON_LESS;
    hr = m_device->CreateDepthStencilState(&dsDesc, m_dss.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    // --- Alpha-blend state (used for transparent materials: ghosts, effects, glows) ---
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend  = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp   = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = m_device->CreateBlendState(&blendDesc, m_blendAlpha.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    // --- Opaque state (BlendEnable=FALSE, simple replace) ---
    D3D11_BLEND_DESC opaqueDesc = {};
    opaqueDesc.RenderTarget[0].BlendEnable    = FALSE;
    opaqueDesc.RenderTarget[0].SrcBlend       = D3D11_BLEND_ONE;
    opaqueDesc.RenderTarget[0].DestBlend      = D3D11_BLEND_ZERO;
    opaqueDesc.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
    opaqueDesc.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
    opaqueDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    opaqueDesc.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    opaqueDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = m_device->CreateBlendState(&opaqueDesc, m_blendOpaque.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    // --- Texture sampler ---
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = m_device->CreateSamplerState(&sampDesc, m_sampler.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    m_shadersReady = true;
    return true;
}

// -----------------------------------------------------------------------
// Render target management
// -----------------------------------------------------------------------

bool MeshRenderer::CreateRenderTarget(int w, int h) {
    if (w <= 0 || h <= 0 || !m_device) return false;

    m_rtv.Reset();
    m_dsv.Reset();
    m_srv.Reset();
    m_renderTarget.Reset();
    m_depthBuffer.Reset();

    HRESULT hr;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(w);
    texDesc.Height = static_cast<UINT>(h);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    hr = m_device->CreateTexture2D(&texDesc, nullptr, m_renderTarget.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    hr = m_device->CreateRenderTargetView(m_renderTarget.Get(), nullptr, m_rtv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    hr = m_device->CreateShaderResourceView(m_renderTarget.Get(), nullptr, m_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = static_cast<UINT>(w);
    depthDesc.Height = static_cast<UINT>(h);
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = m_device->CreateTexture2D(&depthDesc, nullptr, m_depthBuffer.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    hr = m_device->CreateDepthStencilView(m_depthBuffer.Get(), nullptr, m_dsv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    m_rtWidth = w;
    m_rtHeight = h;
    return true;
}

// -----------------------------------------------------------------------
// Mesh buffer creation
// -----------------------------------------------------------------------

static float HalfToFloat(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;

    if (exp == 0) {
        if (mant == 0) return sign ? -0.0f : 0.0f;
        float f = static_cast<float>(mant) / 1024.0f;
        f *= (1.0f / 16384.0f);
        return sign ? -f : f;
    }
    if (exp == 31) {
        if (mant == 0) return sign ? -INFINITY : INFINITY;
        return NAN;
    }

    uint32_t f32 = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    float result;
    std::memcpy(&result, &f32, 4);
    return result;
}

bool MeshRenderer::CreateMeshBuffers(const Parsers::SmdMesh& mesh, std::string& error) {
    if (!m_device || mesh.vertices.empty()) return false;

    // Release old buffers
    m_vb.Reset();
    m_ib.Reset();

    // Build interleaved vertex data: kVertexStride (40) bytes per vertex
    std::vector<float> vertexData;
    vertexData.reserve(mesh.vertices.size() * (kVertexStride / sizeof(float)));
    constexpr float kMaxCoord = 1e6f;
    for (const auto& v : mesh.vertices) {
        float px = v.x, py = v.y, pz = v.z;
        if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz) ||
            std::fabs(px) > kMaxCoord || std::fabs(py) > kMaxCoord || std::fabs(pz) > kMaxCoord) {
            px = py = pz = std::numeric_limits<float>::quiet_NaN();
        }
        vertexData.push_back(px);
        vertexData.push_back(py);
        vertexData.push_back(pz);

        float nx = (v.nx / 127.5f) - 1.0f;
        float ny = (v.ny / 127.5f) - 1.0f;
        float nz = (v.nz / 127.5f) - 1.0f;
        float nLen = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nLen > 0.0001f) { nx /= nLen; ny /= nLen; nz /= nLen; }
        else { nx = 0.0f; ny = 1.0f; nz = 0.0f; }
        vertexData.push_back(nx);
        vertexData.push_back(ny);
        vertexData.push_back(nz);

        vertexData.push_back(HalfToFloat(v.uvRaw[0]));
        vertexData.push_back(HalfToFloat(v.uvRaw[1]));

        uint32_t boneIdx = v.boneData[0] | (v.boneData[1] << 8) | (v.boneData[2] << 16) | (v.boneData[3] << 24);
        float biFloat; std::memcpy(&biFloat, &boneIdx, 4);
        vertexData.push_back(biFloat);

        uint32_t boneWt = v.boneData[4] | (v.boneData[5] << 8) | (v.boneData[6] << 16) | (v.boneData[7] << 24);
        float bwFloat; std::memcpy(&bwFloat, &boneWt, 4);
        vertexData.push_back(bwFloat);
    }

    m_vertexCount = static_cast<uint32_t>(mesh.vertices.size());

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = static_cast<UINT>(vertexData.size() * sizeof(float));
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = vertexData.data();

    HRESULT hr = m_device->CreateBuffer(&vbDesc, &vbData, m_vb.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        error = "Failed to create vertex buffer";
        return false;
    }

    if (!mesh.indices.empty()) {
        m_indexCount = static_cast<uint32_t>(mesh.indices.size());
        m_use32BitIndices = (mesh.vertexCount > 0xFFFF);

        D3D11_BUFFER_DESC ibDesc = {};
        ibDesc.Usage = D3D11_USAGE_DEFAULT;
        ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

        D3D11_SUBRESOURCE_DATA ibData = {};

        if (m_use32BitIndices) {
            ibDesc.ByteWidth = static_cast<UINT>(mesh.indices.size() * sizeof(uint32_t));
            ibData.pSysMem = mesh.indices.data();
        } else {
            std::vector<uint16_t> idx16(mesh.indices.size());
            for (size_t i = 0; i < mesh.indices.size(); i++)
                idx16[i] = static_cast<uint16_t>(mesh.indices[i]);
            ibDesc.ByteWidth = static_cast<UINT>(idx16.size() * sizeof(uint16_t));
            ibData.pSysMem = idx16.data();
            hr = m_device->CreateBuffer(&ibDesc, &ibData, m_ib.ReleaseAndGetAddressOf());
            if (FAILED(hr)) {
                error = "Failed to create index buffer";
                return false;
            }
            return true;
        }

        hr = m_device->CreateBuffer(&ibDesc, &ibData, m_ib.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            error = "Failed to create index buffer";
            return false;
        }
    } else {
        m_indexCount = 0;
    }

    return true;
}

// -----------------------------------------------------------------------
// Scene rendering (offscreen)
// -----------------------------------------------------------------------

void MeshRenderer::RenderScene(int w, int h,
                                MeshCamera& camera,
                                MeshMaterial& material,
                                const Parsers::SmdMesh& mesh,
                                int renderMode,
                                const float* skinningMatrices,
                                std::size_t boneCount) {
    if (!m_context || !m_shadersReady) return;

    // Recreate render target if size changed
    if (w != m_rtWidth || h != m_rtHeight) {
        if (!CreateRenderTarget(w, h)) return;
    }
    if (!m_rtv || !m_dsv) return;

    // Skeleton-only scenes (no mesh geometry) still need the RT cleared so the
    // overlay drawlist (bones, grid) doesn't render on top of stale pixels.
    const bool hasMesh = (m_indexCount > 0) && m_vb && m_ib;
    if (!hasMesh) {
        ID3D11RenderTargetView* rtv = m_rtv.Get();
        m_context->OMSetRenderTargets(1, &rtv, m_dsv.Get());
        float clearColor[4] = {kBgColor[0], kBgColor[1], kBgColor[2], 1.0f};
        m_context->ClearRenderTargetView(m_rtv.Get(), clearColor);
        m_context->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        D3D11_VIEWPORT vp0 = {};
        vp0.Width = static_cast<float>(w);
        vp0.Height = static_cast<float>(h);
        vp0.MinDepth = 0.0f;
        vp0.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &vp0);
        return;
    }

    // Save existing D3D state
    ID3D11RenderTargetView* oldRtv = nullptr;
    ID3D11DepthStencilView* oldDsv = nullptr;
    D3D11_VIEWPORT oldVp = {};
    UINT numVps = 1;
    m_context->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    m_context->RSGetViewports(&numVps, &oldVp);

    // Set our render target
    ID3D11RenderTargetView* rtv = m_rtv.Get();
    m_context->OMSetRenderTargets(1, &rtv, m_dsv.Get());

    // Clear
    float clearColor[4] = {kBgColor[0], kBgColor[1], kBgColor[2], 1.0f};
    m_context->ClearRenderTargetView(m_rtv.Get(), clearColor);
    m_context->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    // Viewport
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(w);
    vp.Height = static_cast<float>(h);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);

    // Camera matrices
    float view[16], proj[16], model[16], mv[16], mvp[16];
    MeshCamera::MatIdentity(model);
    camera.BuildViewMatrix(view);
    float aspect = static_cast<float>(w) / static_cast<float>(h);
    camera.BuildProjectionMatrix(proj, aspect);

    MeshCamera::MatMul(mv, model, view);
    MeshCamera::MatMul(mvp, mv, proj);

    float mvpT[16], worldT[16];
    MeshCamera::MatTranspose(mvpT, mvp);
    MeshCamera::MatTranspose(worldT, model);

    // The one directional light (its travel direction, normalized).
    float lx = 0.4f, ly = -0.6f, lz = 0.5f;
    float ll = std::sqrt(lx * lx + ly * ly + lz * lz);
    lx /= ll; ly /= ll; lz /= ll;

    // Build CB struct on the stack — drawSubmesh uploads it per submesh with
    // Opaque overridden by the material blend mode.
    CBData cbData = {};
    std::memcpy(cbData.MVP, mvpT, sizeof(float) * 16);
    std::memcpy(cbData.World, worldT, sizeof(float) * 16);
    cbData.BaseColor[0] = 0.78f;
    cbData.BaseColor[1] = 0.80f;
    cbData.BaseColor[2] = 0.84f;
    cbData.BaseColor[3] = 1.0f;
    cbData.LightDir[0] = lx;
    cbData.LightDir[1] = ly;
    cbData.LightDir[2] = lz;
    cbData.LightDir[3] = 0.0f;
    cbData.Opaque = 1.0f;  // default; overridden per submesh in drawSubmesh
    // Skin only when an animated palette was actually provided.
    cbData.SkinEnabled = (skinningMatrices != nullptr && boneCount > 0) ? 1.0f : 0.0f;

    // Set input assembler
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetInputLayout(m_layout.Get());

    UINT stride = kVertexStride;
    UINT offset = 0;
    ID3D11Buffer* vbPtr = m_vb.Get();
    m_context->IASetVertexBuffers(0, 1, &vbPtr, &stride, &offset);
    m_context->IASetIndexBuffer(m_ib.Get(), m_use32BitIndices ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);

    // Set shaders and constant buffers
    m_context->VSSetShader(m_vs.Get(), nullptr, 0);
    ID3D11Buffer* cbPtr = m_cb.Get();
    m_context->VSSetConstantBuffers(0, 1, &cbPtr);
    m_context->PSSetConstantBuffers(0, 1, &cbPtr);

    // Bone constant buffer upload.
    //
    // Project-wide HLSL convention is column-major (default; MVP/World are
    // row-major on CPU and transposed on upload — see the camera matrices above).
    // BuildSkinningPalette emits matrices in the same row-vector / row-major
    // convention as MVP/World, so we transpose each bone matrix on upload to
    // stay consistent with the shader's column-major interpretation.
    if (m_boneCB) {
        D3D11_MAPPED_SUBRESOURCE boneMap = {};
        if (SUCCEEDED(m_context->Map(m_boneCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &boneMap))) {
            float* boneData = static_cast<float*>(boneMap.pData);

            if (skinningMatrices != nullptr && boneCount > 0) {
                // Animated path: transpose and copy palette, fill remainder with identity.
                const std::size_t n = (boneCount < static_cast<std::size_t>(kMaxSkinBones))
                                        ? boneCount
                                        : static_cast<std::size_t>(kMaxSkinBones);
                for (std::size_t i = 0; i < n; ++i) {
                    const float* src = skinningMatrices + i * 16;
                    float* dst = boneData + i * 16;
                    // Transpose src (row-major) into dst (column-major storage).
                    for (int r = 0; r < 4; ++r) {
                        for (int c = 0; c < 4; ++c) {
                            dst[c * 4 + r] = src[r * 4 + c];
                        }
                    }
                }

                // Fill any unused slots with identity (defensive — shader bounds-checks
                // by weight > 0.001 anyway, but identity avoids garbage matmuls).
                for (std::size_t i = n; i < static_cast<std::size_t>(kMaxSkinBones); ++i) {
                    std::memset(boneData + i * 16, 0, 64);
                    boneData[i * 16 + 0]  = 1.0f;
                    boneData[i * 16 + 5]  = 1.0f;
                    boneData[i * 16 + 10] = 1.0f;
                    boneData[i * 16 + 15] = 1.0f;
                }
            } else {
                // Identity fallback (no skinning data provided).
                std::memset(boneData, 0, kMaxSkinBones * 64);
                for (int i = 0; i < kMaxSkinBones; ++i) {
                    boneData[i * 16 + 0]  = 1.0f;
                    boneData[i * 16 + 5]  = 1.0f;
                    boneData[i * 16 + 10] = 1.0f;
                    boneData[i * 16 + 15] = 1.0f;
                }
            }

            m_context->Unmap(m_boneCB.Get(), 0);
        }
        ID3D11Buffer* bonePtr = m_boneCB.Get();
        m_context->VSSetConstantBuffers(1, 1, &bonePtr);
    }

    // Bind the texture sampler
    if (m_sampler) {
        ID3D11SamplerState* samp = m_sampler.Get();
        m_context->PSSetSamplers(0, 1, &samp);
    }

    m_context->OMSetDepthStencilState(m_dss.Get(), 0);

    // Draw helper lambda — uploads CB + selects blend state per submesh.
    auto drawSubmesh = [&](uint32_t idxStart, uint32_t idxCount,
                           ID3D11ShaderResourceView* diffuse,
                           BlendMode blend) {
        // Per-submesh: copy cached cbData with Opaque overridden for this blend mode.
        {
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            HRESULT hrMap = m_context->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (SUCCEEDED(hrMap)) {
                CBData* cb = static_cast<CBData*>(mapped.pData);
                *cb = cbData;  // copy cached struct (all fixed fields)
                cb->Opaque = (blend == BlendMode::Opaque) ? 1.0f : 0.0f;
                m_context->Unmap(m_cb.Get(), 0);
            }
        }

        // Select blend state per-submesh.
        ID3D11BlendState* bs = (blend == BlendMode::Opaque)
            ? m_blendOpaque.Get() : m_blendAlpha.Get();
        float blendFactor[4] = {0, 0, 0, 0};
        m_context->OMSetBlendState(bs, blendFactor, 0xFFFFFFFF);

        ID3D11ShaderResourceView* bindDiffuse = diffuse ? diffuse : material.GetFallbackSRV();
        if (bindDiffuse) m_context->PSSetShaderResources(0, 1, &bindDiffuse);

        if (renderMode == 0 || renderMode == 2) {
            m_context->RSSetState(m_rsSolid.Get());
            m_context->PSSetShader(m_psSolid.Get(), nullptr, 0);
            m_context->DrawIndexed(idxCount, idxStart, 0);
        }
        if (renderMode == 1 || renderMode == 2) {
            m_context->RSSetState(m_rsWire.Get());
            m_context->PSSetShader(m_psWire.Get(), nullptr, 0);
            m_context->DrawIndexed(idxCount, idxStart, 0);
        }
    };

    // Draw per-submesh with material textures, or all at once
    auto& materials = material.GetMaterials();
    if (!mesh.submeshRanges.empty() && !materials.empty()) {
        for (size_t s = 0; s < mesh.submeshRanges.size(); ++s) {
            auto& range = mesh.submeshRanges[s];
            ID3D11ShaderResourceView* diffuse = nullptr;
            BlendMode blend = BlendMode::Opaque;
            if (s < materials.size()) {
                if (materials[s].loaded) diffuse = materials[s].srv.Get();
                blend = materials[s].blendMode;
            }
            drawSubmesh(range.indexOffset, range.indexCount, diffuse, blend);
        }
    } else {
        drawSubmesh(0, m_indexCount, nullptr, BlendMode::Opaque);
    }

    // Restore previous D3D state
    m_context->OMSetRenderTargets(1, &oldRtv, oldDsv);
    m_context->RSSetViewports(1, &oldVp);
    if (oldRtv) oldRtv->Release();
    if (oldDsv) oldDsv->Release();
}

} // namespace GGPKExp
