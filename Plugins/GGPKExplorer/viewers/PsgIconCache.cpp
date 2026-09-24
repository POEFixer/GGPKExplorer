#include "PsgIconCache.h"
#include "../core/GGPKManager.h"

#include <DDSTextureLoader11.h>

#include <algorithm>
#include <cctype>
#include <excpt.h>

namespace GGPKExp {

// SEH-wrapped loader — catches access violations inside nvwgf2umx.dll during
// CreateDDSTextureFromMemory. The pointer-only parameters ensure the __try
// block has no C++ objects with destructors; LoadOne's internal C++ state
// is cleaned up by its own destructors inside its own stack frame.
static bool LoadOneSafeImpl(PsgIconCache* cache, const std::string* path) {
    bool ok = false;
    __try {
        ok = cache->LoadOne(*path);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

void PsgIconCache::Initialize(ID3D11Device* device, GGPKManager* manager) {
    m_device = device;
    m_manager = manager;
}

std::string PsgIconCache::NormalizeKey(const std::string& path) {
    std::string out = path;
    for (auto& c : out) {
        if (c == '\\') c = '/';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool PsgIconCache::LoadOne(const std::string& ggpkPath) {
    if (!m_device || !m_manager) return false;

    std::string key = NormalizeKey(ggpkPath);
    if (m_textures.find(key) != m_textures.end()) return true;

    void* nodePtr = m_manager->FindNodeByPath(ggpkPath);
    if (!nodePtr) return false;

    auto readResult = m_manager->ReadFile(nodePtr);
    if (!readResult) return false;

    const auto& data = readResult.value;
    if (data.empty()) return false;

    Entry entry;
    HRESULT hr = DirectX::CreateDDSTextureFromMemory(
        m_device,
        data.data(),
        data.size(),
        nullptr,
        entry.srv.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !entry.srv) return false;

    Microsoft::WRL::ComPtr<ID3D11Resource> res;
    entry.srv->GetResource(res.GetAddressOf());
    if (res) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(res.As(&tex)) && tex) {
            D3D11_TEXTURE2D_DESC desc{};
            tex->GetDesc(&desc);
            entry.width = static_cast<int>(desc.Width);
            entry.height = static_cast<int>(desc.Height);
        }
    }

    m_textures.emplace(std::move(key), std::move(entry));
    return true;
}

void PsgIconCache::PreloadAll(const std::vector<std::string>& ggpkPaths) {
    if (!m_device || !m_manager) return;
    for (const auto& path : ggpkPaths) {
        if (path.empty()) continue;
        LoadOne(path);
    }
}

bool PsgIconCache::LoadOneSafe(const std::string& ggpkPath) {
    return LoadOneSafeImpl(this, &ggpkPath);
}

ImTextureID PsgIconCache::Get(const std::string& ggpkPath) const {
    auto it = m_textures.find(NormalizeKey(ggpkPath));
    if (it == m_textures.end() || !it->second.srv) return ImTextureID{};
    return reinterpret_cast<ImTextureID>(it->second.srv.Get());
}

void PsgIconCache::GetSize(const std::string& ggpkPath, int& outW, int& outH) const {
    outW = outH = 0;
    auto it = m_textures.find(NormalizeKey(ggpkPath));
    if (it == m_textures.end()) return;
    outW = it->second.width;
    outH = it->second.height;
}

void PsgIconCache::Clear() {
    m_textures.clear();
}

} // namespace GGPKExp
