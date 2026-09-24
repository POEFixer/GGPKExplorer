#pragma once
#include "../../core/parsers/AocTypes.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GGPKExp {
class GGPKManager;
namespace Aoc {

// miniaudio-backed sound-event player. PIMPL: header is device-agnostic.
class AocAudioSystem {
public:
    AocAudioSystem();
    ~AocAudioSystem();

    AocAudioSystem(const AocAudioSystem&)            = delete;
    AocAudioSystem& operator=(const AocAudioSystem&) = delete;

    // Wire GGPK + re-trigger debounce (ms). Does not start audio device yet.
    void Configure(GGPKManager* mgr, std::string aocDir);

    // Initialises miniaudio device (fails silently — IsOperational() reflects outcome).
    void Start();
    void Stop();
    bool IsOperational() const;

    // Trigger a clip now. Logical filename has NO extension — resolver tries .ogg/.wav/.oga.
    // Missing files are reported via OnMissingCallback (set separately).
    void Trigger(const Parsers::SoundEventClip& clip);

    // Absolute-path audio (e.g. Info-tab "Play" button resolves once and passes here).
    void TriggerByLogicalName(const std::string& filename, const std::string& boneName = "");

    void SetMasterVolume(float v);   // 0..1 clamp internally
    void SetMuted(bool m);
    float MasterVolume() const;
    bool  Muted() const;

    // Invoked on main thread the next time MainThreadPump() runs, for any clip
    // whose file could not be resolved.
    using MissingCallback = std::function<void(const std::string& logicalName)>;
    void SetOnMissing(MissingCallback cb);

    // Drain any worker-thread warnings/missing notifications; call once per frame.
    void MainThreadPump();

    // Full list of unresolved logical names encountered this session.
    const std::vector<std::string>& MissingLog() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}} // namespace GGPKExp::Aoc
