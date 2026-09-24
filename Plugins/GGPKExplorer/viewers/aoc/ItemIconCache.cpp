#include "ItemIconCache.h"
#include "../../core/GGPKManager.h"

#include <DDSTextureLoader11.h>

#include <algorithm>
#include <cctype>
#include <excpt.h>
#include <utility>

namespace GGPKExp {
namespace Aoc {

// SEH-wrapped loader — catches access violations inside the DDS decoder
// (NVIDIA driver occasionally faults on malformed textures).
// Pointer-only parameters ensure the __try block has no C++ objects with
// destructors; LoadOne's internal C++ state is cleaned up by its own
// destructors inside its own stack frame.
static bool LoadOneSafeImpl(ItemIconCache* cache, const std::string* path) {
    bool ok = false;
    __try {
        ok = cache->LoadOne(*path);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

void ItemIconCache::Initialize(ID3D11Device* device, GGPKManager* manager) {
    m_device  = device;
    m_manager = manager;
}

std::string ItemIconCache::NormalizeKey(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    for (char c : path) {
        if (c == '\\') c = '/';
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

ImTextureID ItemIconCache::Get(const std::string& ddsPath) {
    if (ddsPath.empty()) return ImTextureID{};

    const std::string key = NormalizeKey(ddsPath);

    auto it = m_textures.find(key);
    if (it != m_textures.end()) {
        if (it->second.state == LoadState::Loaded && it->second.srv) {
            return reinterpret_cast<ImTextureID>(it->second.srv.Get());
        }
        // Either queued but not yet drained, or failed — return blank.
        return ImTextureID{};
    }

    // First sighting: create a Queued entry and enqueue for load.
    // Dedup: m_queuedSet ensures a path is queued at most once.
    if (m_queuedSet.insert(key).second) {
        Entry entry;
        entry.state = LoadState::Queued;
        m_textures.emplace(key, std::move(entry));
        m_queue.push_back(key);
    }
    return ImTextureID{};
}

void ItemIconCache::PumpQueue(int budget) {
    if (!m_device || !m_manager) return;
    while (budget-- > 0 && !m_queue.empty()) {
        std::string key = std::move(m_queue.front());
        m_queue.pop_front();
        m_queuedSet.erase(key);
        LoadOneSafe(key);
    }
}

void ItemIconCache::Clear() {
    m_textures.clear();
    m_queue.clear();
    m_queuedSet.clear();
}

bool ItemIconCache::LoadOneSafe(const std::string& path) {
    return LoadOneSafeImpl(this, &path);
}

bool ItemIconCache::LoadOne(const std::string& path) {
    if (!m_device || !m_manager) return false;

    const std::string key = NormalizeKey(path);

    // GGPK lookup — key is lowercase already; FindNodeByPath is case-tolerant.
    void* nodePtr = m_manager->FindNodeByPath(key);
    if (!nodePtr) {
        m_textures[key] = Entry{ {}, LoadState::Failed };
        return false;
    }

    auto readResult = m_manager->ReadFile(nodePtr);
    if (!readResult.success) {
        m_textures[key] = Entry{ {}, LoadState::Failed };
        return false;
    }
    const auto& data = readResult.value;
    if (data.empty()) {
        m_textures[key] = Entry{ {}, LoadState::Failed };
        return false;
    }

    Entry entry;
    HRESULT hr = DirectX::CreateDDSTextureFromMemory(
        m_device,
        data.data(),
        data.size(),
        nullptr,
        entry.srv.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !entry.srv) {
        m_textures[key] = Entry{ {}, LoadState::Failed };
        return false;
    }
    entry.state = LoadState::Loaded;
    m_textures[key] = std::move(entry);
    return true;
}

} // namespace Aoc
} // namespace GGPKExp
