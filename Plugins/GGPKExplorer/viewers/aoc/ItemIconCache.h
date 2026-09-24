#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <imgui.h>   // ImTextureID

namespace GGPKExp {

class GGPKManager;

namespace Aoc {

// Phase 5.1.5c — Session-scoped cache of DDS icon textures for the
// DAT Item Browser (AttachBrowseModal "By Item Name" tab).
//
// Design highlights (see design doc 2026-04-18-aoc-viewer-thumbnail-rendering-design.md):
//   - Lazy: Get() returns ImTextureID{} on first miss and enqueues the path.
//   - Rate-limited: PumpQueue(budget) drains up to `budget` loads per frame,
//     so a filter-change burst of ~30 uncached rows spreads across ~4 frames.
//   - SEH-wrapped: LoadOneSafe() catches access violations inside the DDS
//     loader (NVIDIA driver is known to occasionally crash on malformed DDS).
//   - Not LRU: in practice the live set is <500 textures × <64 KB VRAM.
//     Cache is cleared on GGPK-manager swap.
class ItemIconCache {
public:
    ItemIconCache()  = default;
    ~ItemIconCache() = default;

    ItemIconCache(const ItemIconCache&)            = delete;
    ItemIconCache& operator=(const ItemIconCache&) = delete;

    // Bind the GPU device + GGPK reader. Must be called before Get/PumpQueue.
    void Initialize(ID3D11Device* device, GGPKManager* manager);

    // Returns a loaded SRV as ImTextureID, or ImTextureID{} if not yet loaded.
    // On miss, enqueues the path for background loading. Repeated calls for
    // already-queued / already-failed paths are no-ops on the queue.
    ImTextureID Get(const std::string& ddsPath);

    // Drains up to `budget` enqueued paths, performing SEH-protected DDS load
    // for each. Call once per frame after table rendering.
    void PumpQueue(int budget);

    // Drops all textures and the pending queue. Call when the GGPK manager
    // pointer changes (user closed one GGPK and opened another).
    void Clear();

    // SEH wrapper — defined separately to keep the __try block free of
    // C++ objects with destructors. Do not call from inside another SEH frame.
    bool LoadOneSafe(const std::string& path);

    // Actual DDS decode. Public only so the SEH wrapper can forward to it;
    // external callers should use Get()/PumpQueue().
    bool LoadOne(const std::string& path);

private:
    enum class LoadState { Queued, Loaded, Failed };

    struct Entry {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        LoadState state = LoadState::Queued;
    };

    static std::string NormalizeKey(const std::string& path);

    ID3D11Device* m_device  = nullptr;   // non-owning
    GGPKManager*  m_manager = nullptr;   // non-owning

    std::unordered_map<std::string, Entry> m_textures;
    std::deque<std::string>                m_queue;
    std::unordered_set<std::string>        m_queuedSet;
};

} // namespace Aoc
} // namespace GGPKExp
