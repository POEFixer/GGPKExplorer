#include "MeshMaterial.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <DDSTextureLoader11.h>
#include <json.hpp>

#include "../../core/GGPKManager.h"

namespace GGPKExp {

// -----------------------------------------------------------------------
// Initialize / Clear
// -----------------------------------------------------------------------

void MeshMaterial::Initialize(ID3D11Device* device) {
    m_device = device;
}

void MeshMaterial::Clear() {
    m_texSRV.Reset();
    m_materialTextures.clear();
    m_hasTexture = false;
    m_fallbackDdsPath.clear();
}

// -----------------------------------------------------------------------
// Fallback texture (1x1 white)
// -----------------------------------------------------------------------

bool MeshMaterial::CreateFallbackTexture() {
    if (!m_device) return false;

    uint32_t whitePixel = 0xFFFFFFFF;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = &whitePixel;
    initData.SysMemPitch = 4;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = m_device->CreateTexture2D(&desc, &initData, &tex);
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    hr = m_device->CreateShaderResourceView(tex, &srvDesc, m_texSRV.ReleaseAndGetAddressOf());
    tex->Release();

    return SUCCEEDED(hr);
}

// -----------------------------------------------------------------------
// Load texture from .sm -> .mat -> .dds chain (legacy single-texture)
// -----------------------------------------------------------------------

bool MeshMaterial::LoadTextureFromSm(const std::vector<uint8_t>& smData, GGPKManager* mgr) {
    if (!mgr || !m_device || smData.size() < 4) return false;

    // Parse .sm as UTF-16LE text
    std::string smText;
    {
        size_t start = 0;
        if (smData.size() >= 2 && smData[0] == 0xFF && smData[1] == 0xFE) start = 2;
        for (size_t i = start; i + 1 < smData.size(); i += 2) {
            uint16_t ch;
            std::memcpy(&ch, smData.data() + i, 2);
            if (ch < 128) smText += static_cast<char>(ch);
            else smText += '?';
        }
    }

    // Find .mat file path in the .sm text
    std::string matPath;
    size_t pos = 0;
    while (pos < smText.size()) {
        auto nl = smText.find('\n', pos);
        std::string line = (nl != std::string::npos) ? smText.substr(pos, nl - pos) : smText.substr(pos);
        pos = (nl != std::string::npos) ? nl + 1 : smText.size();

        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();

        std::string lineLower = line;
        std::transform(lineLower.begin(), lineLower.end(), lineLower.begin(), ::tolower);
        auto matPos = lineLower.find(".mat");
        if (matPos != std::string::npos) {
            auto q1 = line.rfind('"', matPos);
            auto q2 = line.find('"', matPos);
            if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
                matPath = line.substr(q1 + 1, q2 - q1 - 1);
            }
            if (!matPath.empty()) break;
        }
    }
    if (matPath.empty()) return false;

    std::string ddsPath;
    BlendMode blend = BlendMode::Opaque;   // the legacy texture is always drawn as before
    ID3D11ShaderResourceView* newSrv = LoadTextureFromMat(matPath, mgr, ddsPath, blend);
    if (!newSrv) return false;

    // Replace fallback white texture with the real one
    m_texSRV.Attach(newSrv);
    m_hasTexture = true;
    m_fallbackDdsPath = ddsPath;
    return true;
}

// -----------------------------------------------------------------------
// Multi-material loading
// -----------------------------------------------------------------------

void MeshMaterial::LoadAllMaterials(const Parsers::SmData& smData, size_t submeshCount, GGPKManager* mgr) {
    if (!mgr || !m_device || smData.materialPaths.empty() || submeshCount == 0) {
        m_materialTextures.clear();
        return;
    }

    // Phase 1: load each unique .mat entry once (so repeated run-length entries
    // don't re-read the same .mat/.dds from GGPK).
    std::vector<MaterialTex> loaded(smData.materialPaths.size());
    for (size_t i = 0; i < smData.materialPaths.size(); ++i) {
        auto& mt = loaded[i];
        mt.matPath = smData.materialPaths[i].path;

        std::string ddsPath;
        BlendMode blend = BlendMode::Opaque;
        ID3D11ShaderResourceView* srv = LoadTextureFromMat(mt.matPath, mgr, ddsPath, blend);
        if (srv) mt.srv.Attach(srv);
        mt.loaded = (mt.srv != nullptr);
        if (mt.loaded) mt.ddsPath = ddsPath;
        mt.blendMode = blend;  // captured regardless of SRV success so MeshRenderer can still
                               // select blend state for fallback-textured submeshes.
    }

    // Phase 2: expand by run-length to fill m_materialTextures[submeshCount].
    // Each entry applies to its submeshCount consecutive submeshes, capped at
    // remaining. If total run-length < submeshCount (malformed or shortage),
    // remaining submeshes get default-constructed (unloaded) MaterialTex.
    m_materialTextures.clear();
    m_materialTextures.reserve(submeshCount);
    for (size_t i = 0; i < smData.materialPaths.size() && m_materialTextures.size() < submeshCount; ++i) {
        uint32_t count = smData.materialPaths[i].submeshCount;
        if (count == 0) count = 1;  // defensive: treat zero as "apply once"
        size_t remaining = submeshCount - m_materialTextures.size();
        size_t take = (std::min)(static_cast<size_t>(count), remaining);
        for (size_t j = 0; j < take; ++j) {
            m_materialTextures.push_back(loaded[i]);  // MaterialTex is copyable via ComPtr AddRef
        }
    }
    // Pad with empty MaterialTex if sm provided fewer materials than submesh count.
    while (m_materialTextures.size() < submeshCount) {
        m_materialTextures.emplace_back();
    }
}

ID3D11ShaderResourceView* MeshMaterial::LoadTextureFromMat(
    const std::string& matPath, GGPKManager* mgr, std::string& outDdsPath,
    BlendMode& outBlend) {

    outBlend = BlendMode::Opaque;  // default: all early-exit paths inherit Opaque

    void* matNode = mgr->FindNodeByPath(matPath);
    if (!matNode) return nullptr;

    auto matRes2 = mgr->ReadFile(matNode);
    if (!matRes2 || matRes2.value.empty()) return nullptr;
    std::vector<uint8_t> matData = std::move(matRes2.value);

    std::string matText;
    {
        size_t start = 0;
        if (matData.size() >= 2 && matData[0] == 0xFF && matData[1] == 0xFE) start = 2;
        for (size_t i = start; i + 1 < matData.size(); i += 2) {
            uint16_t ch;
            std::memcpy(&ch, matData.data() + i, 2);
            if (ch < 128) matText += static_cast<char>(ch);
        }
    }
    if (matText.empty()) return nullptr;

    std::string ddsPath;
    std::string fallbackDds;

    try {
        auto matJson = nlohmann::json::parse(matText);

        // Fix #3: blend-mode detection. Priority:
        //   1) defaultgraph.overriden_blend_mode (string, authoritative but rare)
        //   2) graphinstances[].parent keyword match
        //   3) path heuristic fallback (effects/ghost/glow/transparentobjects)
        auto detectBlendMode = [&]() {
            // 1) explicit override
            auto it = matJson.find("defaultgraph");
            if (it != matJson.end() && it->is_object()) {
                auto ov = it->find("overriden_blend_mode");
                if (ov != it->end() && ov->is_string()) {
                    std::string v = ov->get<std::string>();
                    if (v == "Opaque") { outBlend = BlendMode::Opaque; return; }
                    if (v == "Alpha" || v == "AlphaBlend" || v == "Transparent")
                        { outBlend = BlendMode::AlphaBlend; return; }
                    if (v == "Additive") { outBlend = BlendMode::AlphaBlend; return; }
                    // Unknown override: keep default Opaque.
                }
            }
            // 2) parent fxgraph keywords
            auto gi = matJson.find("graphinstances");
            if (gi != matJson.end() && gi->is_array()) {
                for (const auto& inst : *gi) {
                    if (!inst.is_object()) continue;
                    auto p = inst.find("parent");
                    if (p == inst.end() || !p->is_string()) continue;
                    std::string parent = p->get<std::string>();
                    // Transparent / additive signals (stronger -> any match = AlphaBlend)
                    if (parent.find("ForceAdditive.fxgraph") != std::string::npos ||
                        parent.find("SoftAlphaParticle.fxgraph") != std::string::npos ||
                        parent.find("LuminanceAlpha.fxgraph") != std::string::npos ||
                        parent.find("DisableZWrites.fxgraph") != std::string::npos) {
                        outBlend = BlendMode::AlphaBlend;
                        return;
                    }
                }
            }
            // 3) path heuristic fallback
            std::string pathLower = matPath;
            std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), ::tolower);
            if (pathLower.find("/effects/") != std::string::npos ||
                pathLower.find("_glow") != std::string::npos ||
                pathLower.find("ghost") != std::string::npos ||
                pathLower.find("transparentobjects") != std::string::npos) {
                outBlend = BlendMode::AlphaBlend;
                return;
            }
            // Default: Opaque.
        };
        detectBlendMode();

        std::function<void(const nlohmann::json&)> findDds = [&](const nlohmann::json& j) {
            if (j.is_object()) {
                for (auto& [key, val] : j.items()) {
                    if (key == "path" && val.is_string()) {
                        std::string p = val.get<std::string>();
                        std::string pLower = p;
                        std::transform(pLower.begin(), pLower.end(), pLower.begin(), ::tolower);
                        if (pLower.size() > 4 && pLower.substr(pLower.size() - 4) == ".dds") {
                            // Skip obvious non-diffuse DDS when picking fallback (Fix #4a).
                            bool isNonDiffuse =
                                pLower.find("_normal") != std::string::npos ||
                                pLower.find("_nrm")    != std::string::npos ||
                                pLower.find("_rough")  != std::string::npos ||
                                pLower.find("_mask")   != std::string::npos ||
                                pLower.find("_ao")     != std::string::npos ||
                                pLower.find("_spec")   != std::string::npos ||
                                pLower.find("_gloss")  != std::string::npos;
                            if (fallbackDds.empty() && !isNonDiffuse)
                                fallbackDds = p;
                            // Expanded diffuse detection (Fix #4a, 2026-04-15).
                            // Positive markers that identify albedo/base-color DDS.
                            if (pLower.find("_colour") != std::string::npos ||
                                pLower.find("_color") != std::string::npos ||
                                pLower.find("_basecolour") != std::string::npos ||
                                pLower.find("_basecolor") != std::string::npos ||
                                pLower.find("_albedo") != std::string::npos ||
                                pLower.find("_diffuse") != std::string::npos ||
                                pLower.find("_diff") != std::string::npos) {
                                ddsPath = p;
                            }
                        }
                    }
                    findDds(val);
                }
            } else if (j.is_array()) {
                for (auto& elem : j) findDds(elem);
            }
        };
        findDds(matJson);
    } catch (...) {
        return nullptr;
    }

    if (ddsPath.empty()) ddsPath = fallbackDds;
    if (ddsPath.empty()) return nullptr;

    void* ddsNode = mgr->FindNodeByPath(ddsPath);
    if (!ddsNode) return nullptr;

    auto ddsRes2 = mgr->ReadFile(ddsNode);
    if (!ddsRes2 || ddsRes2.value.empty()) return nullptr;
    std::vector<uint8_t> ddsData = std::move(ddsRes2.value);

    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = DirectX::CreateDDSTextureFromMemory(
        m_device, ddsData.data(), ddsData.size(), nullptr, &srv);
    if (FAILED(hr) || !srv) return nullptr;

    outDdsPath = ddsPath;
    return srv;
}

} // namespace GGPKExp
