// AocAudioSystem.cpp — real miniaudio-backed sound-event player for AOC viewer.
//
// The MINIAUDIO_IMPLEMENTATION macro is defined in AudioViewer.cpp (single TU for
// miniaudio's translation unit). Here we include the header only — linker resolves
// all ma_* symbols against the implementation in AudioViewer.cpp.
//
// miniaudio.h generates many warnings under /W4; wrap the include to silence them
// for this TU only (AocAudioSystem.cpp is built with default /W4).

#include "AocAudioSystem.h"
#include "../../core/GGPKManager.h"

#pragma warning(push, 0)
#include "../lib/miniaudio.h"
#pragma warning(pop)

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GGPKExp { namespace Aoc {

namespace {
constexpr const char* kExtensions[] = { ".ogg", ".wav", ".oga" };
}

// ---------------------------------------------------------------------------
// Impl — PIMPL body holding miniaudio state, resolve cache, and playing list.
// ---------------------------------------------------------------------------
struct AocAudioSystem::Impl {
    GGPKManager* mgr = nullptr;
    std::string  aocDir;

    ma_engine    engine{};
    bool         engineStarted = false;

    float        volume = 0.5f;
    bool         muted  = false;

    // Path resolve cache: logical name (no extension) -> decoded bytes.
    // nullptr value marks a miss (so we don't retry GGPK reads every Trigger).
    std::unordered_map<std::string, std::shared_ptr<std::vector<uint8_t>>> cache;
    std::mutex                                                             cacheMtx;

    // Track decoder/sound lifetimes. Each Trigger() creates a raw pair; they are
    // cleaned up in Stop() (ma_sound_uninit + ma_decoder_uninit + delete).
    // For a short-lived viewer this is fine; the list grows per-trigger.
    struct Playing {
        ma_decoder* dec = nullptr;
        ma_sound*   snd = nullptr;
    };
    std::vector<Playing> playing;
    std::mutex           playingMtx;

    // Pending-missing queue (populated by Resolve when cache miss -> file not found).
    // Drained by MainThreadPump() on the UI thread.
    std::vector<std::string> pendingMissing;
    std::mutex               pendingMtx;

    // Session-long log of every missing logical name encountered.
    std::vector<std::string> missingLog;

    MissingCallback onMissing;
    std::mutex      missingMtx;

    // Resolve a logical filename (no extension) to decoded bytes. Thread-safe.
    // Returns nullptr on cache miss + queues a `pendingMissing` entry.
    std::shared_ptr<std::vector<uint8_t>> Resolve(const std::string& logical) {
        {
            std::lock_guard<std::mutex> lk(cacheMtx);
            auto it = cache.find(logical);
            if (it != cache.end()) return it->second;
        }

        std::shared_ptr<std::vector<uint8_t>> result;
        if (mgr) {
            for (const char* ext : kExtensions) {
                const std::string candidate = logical + ext;
                // Try the candidate path as-is first (logical paths in .aoc are usually
                // rooted at the GGPK root, e.g. "audio/dialogue/foo/bar.ogg").
                if (void* node = mgr->FindNodeByPath(candidate)) {
                    auto r = mgr->ReadFile(node);
                    if (r.success) {
                        result = std::make_shared<std::vector<uint8_t>>(std::move(r.value));
                        break;
                    }
                }
                // Fallback: try resolving relative to the .aoc file's directory.
                if (!aocDir.empty()) {
                    const std::string relative = aocDir + candidate;
                    if (void* node2 = mgr->FindNodeByPath(relative)) {
                        auto r = mgr->ReadFile(node2);
                        if (r.success) {
                            result = std::make_shared<std::vector<uint8_t>>(std::move(r.value));
                            break;
                        }
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lk(cacheMtx);
            cache[logical] = result;   // may be nullptr -- still a valid cache entry
        }
        if (!result) {
            std::lock_guard<std::mutex> lk(pendingMtx);
            pendingMissing.push_back(logical);
        }
        return result;
    }
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AocAudioSystem::AocAudioSystem()  : m_impl(std::make_unique<Impl>()) {}
AocAudioSystem::~AocAudioSystem() { Stop(); }

// ---------------------------------------------------------------------------
// Configure — bind GGPK manager + the directory of the currently-open .aoc.
// ---------------------------------------------------------------------------

void AocAudioSystem::Configure(GGPKManager* mgr, std::string aocDir) {
    m_impl->mgr    = mgr;
    m_impl->aocDir = std::move(aocDir);
    std::lock_guard<std::mutex> lk(m_impl->cacheMtx);
    m_impl->cache.clear();
}

// ---------------------------------------------------------------------------
// Start / Stop — init/tear down miniaudio engine
// ---------------------------------------------------------------------------

void AocAudioSystem::Start() {
    if (m_impl->engineStarted) return;
    ma_engine_config cfg = ma_engine_config_init();
    if (ma_engine_init(&cfg, &m_impl->engine) == MA_SUCCESS) {
        m_impl->engineStarted = true;
        ma_engine_set_volume(&m_impl->engine, m_impl->muted ? 0.0f : m_impl->volume);
    }
}

void AocAudioSystem::Stop() {
    if (!m_impl->engineStarted) return;
    {
        std::lock_guard<std::mutex> lk(m_impl->playingMtx);
        for (auto& p : m_impl->playing) {
            if (p.snd) {
                ma_sound_uninit(p.snd);
                delete p.snd;
            }
            if (p.dec) {
                ma_decoder_uninit(p.dec);
                delete p.dec;
            }
        }
        m_impl->playing.clear();
    }
    ma_engine_uninit(&m_impl->engine);
    m_impl->engineStarted = false;
}

bool AocAudioSystem::IsOperational() const { return m_impl->engineStarted; }

// ---------------------------------------------------------------------------
// Trigger — play a clip from a SoundEventClip or a raw logical name.
// ---------------------------------------------------------------------------

void AocAudioSystem::Trigger(const Parsers::SoundEventClip& clip) {
    TriggerByLogicalName(clip.filename, clip.boneName);
}

void AocAudioSystem::TriggerByLogicalName(const std::string& filename,
                                          const std::string& /*boneName*/) {
    if (!m_impl->engineStarted || m_impl->muted) return;
    if (filename.empty()) return;

    auto buf = m_impl->Resolve(filename);
    if (!buf || buf->empty()) return;

    // Keep the shared buffer alive at least until the decoder is uninited.
    // The resolve cache already holds a strong ref (long-lived across triggers),
    // so we don't need extra bookkeeping here — `buf` ensures survival through
    // this scope; subsequent playback reads from cache's copy via the decoder.
    ma_decoder* dec = new ma_decoder{};
    ma_decoder_config dcfg = ma_decoder_config_init_default();
    if (ma_decoder_init_memory(buf->data(), buf->size(), &dcfg, dec) != MA_SUCCESS) {
        delete dec;
        return;
    }

    ma_sound* snd = new ma_sound{};
    if (ma_sound_init_from_data_source(&m_impl->engine, dec,
                                       MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC,
                                       nullptr, snd) != MA_SUCCESS) {
        ma_decoder_uninit(dec);
        delete dec;
        delete snd;
        return;
    }

    ma_sound_start(snd);
    {
        std::lock_guard<std::mutex> lk(m_impl->playingMtx);
        m_impl->playing.push_back({ dec, snd });
    }
}

// ---------------------------------------------------------------------------
// Volume / mute
// ---------------------------------------------------------------------------

void AocAudioSystem::SetMasterVolume(float v) {
    m_impl->volume = std::max(0.0f, std::min(1.0f, v));
    if (m_impl->engineStarted) {
        ma_engine_set_volume(&m_impl->engine, m_impl->muted ? 0.0f : m_impl->volume);
    }
}

void AocAudioSystem::SetMuted(bool m) {
    m_impl->muted = m;
    if (m_impl->engineStarted) {
        ma_engine_set_volume(&m_impl->engine, m_impl->muted ? 0.0f : m_impl->volume);
    }
}

float AocAudioSystem::MasterVolume() const { return m_impl->volume; }
bool  AocAudioSystem::Muted()        const { return m_impl->muted;  }

// ---------------------------------------------------------------------------
// Missing-file reporting
// ---------------------------------------------------------------------------

void AocAudioSystem::SetOnMissing(MissingCallback cb) {
    std::lock_guard<std::mutex> lk(m_impl->missingMtx);
    m_impl->onMissing = std::move(cb);
}

void AocAudioSystem::MainThreadPump() {
    std::vector<std::string> drained;
    {
        std::lock_guard<std::mutex> lk(m_impl->pendingMtx);
        drained.swap(m_impl->pendingMissing);
    }
    if (drained.empty()) return;

    MissingCallback cb;
    {
        std::lock_guard<std::mutex> lk(m_impl->missingMtx);
        cb = m_impl->onMissing;
        for (auto& s : drained) m_impl->missingLog.push_back(s);
    }
    if (cb) {
        for (const auto& s : drained) cb(s);
    }
}

const std::vector<std::string>& AocAudioSystem::MissingLog() const {
    return m_impl->missingLog;
}

}} // namespace GGPKExp::Aoc
