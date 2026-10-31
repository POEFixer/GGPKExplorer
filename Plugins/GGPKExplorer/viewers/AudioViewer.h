#pragma once
#include "IViewer.h"
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace GGPKExp {

class AudioViewer : public IViewer {
public:
    AudioViewer();
    ~AudioViewer();

    void LoadData(std::vector<uint8_t> data, const std::string& path) override;
    void Clear() override;
    void Render() override;

private:
    struct AudioEngine;
    std::unique_ptr<AudioEngine> m_engine;  // owning, PIMPL for miniaudio types

    std::vector<uint8_t> m_data;  // kept alive for decoder
    std::string m_filename;
    bool m_playing = false;
    bool m_paused = false;
    float m_volume = 0.8f;

    // Cached info
    uint32_t m_sampleRate = 0;
    uint32_t m_channels = 0;
    uint64_t m_totalFrames = 0;
    float m_durationSec = 0.0f;
    std::string m_formatName;

    void StopAndCleanup();
    static std::string DetectFormat(const std::string& filename);
    static std::string FormatTime(float seconds);
};

} // namespace GGPKExp
