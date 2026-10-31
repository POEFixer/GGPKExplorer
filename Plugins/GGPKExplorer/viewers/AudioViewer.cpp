// AudioViewer.cpp — Audio playback using miniaudio (single-header library)
//
// miniaudio.h and stb_vorbis.c generate many warnings under /W4.
// This entire TU is compiled with warnings disabled (set in .vcxproj).

#pragma warning(push, 0)

#define STB_VORBIS_HEADER_ONLY
#include "../lib/stb_vorbis.c"

// The one miniaudio implementation of the plugin: only the decoder and the
// playback device are used, so the high-level engine, the node graph, the
// resource manager, encoding and sound generation are left out, and so is
// the FLAC decoder (.flac is not an audio type here; see FileTypeDetector).
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_FLAC
#define MINIAUDIO_IMPLEMENTATION
#include "../lib/miniaudio.h"

// Now pull in the full stb_vorbis implementation
#undef STB_VORBIS_HEADER_ONLY
#include "../lib/stb_vorbis.c"

#pragma warning(pop)

#include "AudioViewer.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>

namespace GGPKExp {

// ---------------------------------------------------------------------------
// PIMPL — keeps miniaudio types out of the header
// ---------------------------------------------------------------------------

struct AudioViewer::AudioEngine {
    ma_decoder decoder{};
    ma_device  device{};
    bool decoderInit = false;
    bool deviceInit  = false;
};

// ---------------------------------------------------------------------------
// Device data callback — feeds PCM frames from decoder to output
// ---------------------------------------------------------------------------

static void audio_data_callback(ma_device* pDevice, void* pOutput,
                                const void* /*pInput*/, ma_uint32 frameCount)
{
    auto* pDecoder = static_cast<ma_decoder*>(pDevice->pUserData);
    if (!pDecoder) return;
    ma_decoder_read_pcm_frames(pDecoder, pOutput, frameCount, nullptr);
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

AudioViewer::AudioViewer()  = default;

AudioViewer::~AudioViewer()
{
    Clear();
}

// ---------------------------------------------------------------------------
// DetectFormat — derive human-readable name from file extension
// ---------------------------------------------------------------------------

std::string AudioViewer::DetectFormat(const std::string& filename)
{
    auto dot = filename.rfind('.');
    if (dot == std::string::npos) return "Unknown";
    std::string ext = filename.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".wav")  return "WAV";
    if (ext == ".mp3")  return "MP3";
    if (ext == ".ogg")  return "OGG Vorbis";
    return "Audio";
}

// ---------------------------------------------------------------------------
// FormatTime — seconds -> "MM:SS"
// ---------------------------------------------------------------------------

std::string AudioViewer::FormatTime(float seconds)
{
    if (seconds < 0.0f) seconds = 0.0f;
    int totalSec = static_cast<int>(seconds);
    int min = totalSec / 60;
    int sec = totalSec % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d", min, sec);
    return buf;
}

// ---------------------------------------------------------------------------
// StopAndCleanup — tear down device + decoder
// ---------------------------------------------------------------------------

void AudioViewer::StopAndCleanup()
{
    if (m_engine) {
        if (m_engine->deviceInit) {
            ma_device_stop(&m_engine->device);
            ma_device_uninit(&m_engine->device);
            m_engine->deviceInit = false;
        }
        if (m_engine->decoderInit) {
            ma_decoder_uninit(&m_engine->decoder);
            m_engine->decoderInit = false;
        }
    }
    m_playing = false;
    m_paused  = false;
}

// ---------------------------------------------------------------------------
// LoadData — initialize decoder + device from memory buffer
// ---------------------------------------------------------------------------

void AudioViewer::LoadData(std::vector<uint8_t> data,
                           const std::string& path)
{
    Clear();

    if (data.empty()) {
        m_error = "No audio data";
        return;
    }

    // Keep the data — miniaudio reads from this pointer
    m_data     = std::move(data);
    m_filename = path;

    m_engine = std::make_unique<AudioEngine>();

    // Init decoder from memory
    ma_decoder_config decoderCfg = ma_decoder_config_init_default();
    ma_result result = ma_decoder_init_memory(
        m_data.data(), m_data.size(), &decoderCfg, &m_engine->decoder);

    if (result != MA_SUCCESS) {
        m_error = "Failed to decode audio (error " + std::to_string(result) + ")";
        m_engine.reset();
        return;
    }
    m_engine->decoderInit = true;

    // Read format info from decoder
    m_sampleRate = m_engine->decoder.outputSampleRate;
    m_channels   = m_engine->decoder.outputChannels;
    m_formatName = DetectFormat(m_filename);

    // Get total length
    ma_uint64 totalFrames = 0;
    ma_decoder_get_length_in_pcm_frames(&m_engine->decoder, &totalFrames);
    m_totalFrames = totalFrames;
    m_durationSec = (m_sampleRate > 0)
                        ? static_cast<float>(totalFrames) / static_cast<float>(m_sampleRate)
                        : 0.0f;

    // Init playback device
    ma_device_config deviceCfg = ma_device_config_init(ma_device_type_playback);
    deviceCfg.playback.format   = m_engine->decoder.outputFormat;
    deviceCfg.playback.channels = m_channels;
    deviceCfg.sampleRate        = m_sampleRate;
    deviceCfg.dataCallback      = audio_data_callback;
    deviceCfg.pUserData         = &m_engine->decoder;

    result = ma_device_init(nullptr, &deviceCfg, &m_engine->device);
    if (result != MA_SUCCESS) {
        m_error = "Failed to initialize audio device (error " + std::to_string(result) + ")";
        ma_decoder_uninit(&m_engine->decoder);
        m_engine->decoderInit = false;
        m_engine.reset();
        return;
    }
    m_engine->deviceInit = true;

    // Set initial volume
    ma_device_set_master_volume(&m_engine->device, m_volume);

    m_loaded = true;
}

// ---------------------------------------------------------------------------
// Clear — full reset to empty state
// ---------------------------------------------------------------------------

void AudioViewer::Clear()
{
    StopAndCleanup();
    m_engine.reset();

    m_data.clear();
    m_filename.clear();
    m_loaded      = false;
    m_sampleRate  = 0;
    m_channels    = 0;
    m_totalFrames = 0;
    m_durationSec = 0.0f;
    m_formatName.clear();
    m_error.clear();
    m_volume = 0.8f;
}

// ---------------------------------------------------------------------------
// Render — ImGui UI: transport controls, seekbar, volume, info
// ---------------------------------------------------------------------------

void AudioViewer::Render()
{
    if (!m_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error: %s", m_error.c_str());
        return;
    }

    if (!m_loaded || !m_engine) {
        ImGui::TextDisabled("No audio loaded.");
        return;
    }

    // --- Transport controls ---
    ImGui::Spacing();

    // Play / Pause button
    if (m_playing && !m_paused) {
        if (ImGui::Button("Pause", ImVec2(80, 0))) {
            ma_device_stop(&m_engine->device);
            m_paused = true;
        }
    } else {
        if (ImGui::Button(m_paused ? "Resume" : "Play", ImVec2(80, 0))) {
            ma_device_start(&m_engine->device);
            m_playing = true;
            m_paused  = false;
        }
    }

    ImGui::SameLine();

    // Stop button
    if (ImGui::Button("Stop", ImVec2(80, 0))) {
        ma_device_stop(&m_engine->device);
        ma_decoder_seek_to_pcm_frame(&m_engine->decoder, 0);
        m_playing = false;
        m_paused  = false;
    }

    // --- Seekbar ---
    ImGui::Spacing();

    ma_uint64 cursor = 0;
    ma_decoder_get_cursor_in_pcm_frames(&m_engine->decoder, &cursor);

    float cursorSec = (m_sampleRate > 0)
                          ? static_cast<float>(cursor) / static_cast<float>(m_sampleRate)
                          : 0.0f;

    // Time label left
    std::string timeText = FormatTime(cursorSec) + " / " + FormatTime(m_durationSec);

    // Slider for seeking
    float seekPos = cursorSec;
    float maxPos  = (m_durationSec > 0.01f) ? m_durationSec : 0.01f;

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120.0f);
    if (ImGui::SliderFloat("##Seek", &seekPos, 0.0f, maxPos, "")) {
        // User dragged the slider — seek
        ma_uint64 targetFrame = static_cast<ma_uint64>(
            seekPos * static_cast<float>(m_sampleRate));
        if (targetFrame > m_totalFrames) targetFrame = m_totalFrames;
        ma_decoder_seek_to_pcm_frame(&m_engine->decoder, targetFrame);
    }

    ImGui::SameLine();
    ImGui::Text("%s", timeText.c_str());

    // --- Volume ---
    ImGui::Spacing();

    ImGui::SetNextItemWidth(200.0f);
    float volumePct = m_volume * 100.0f;
    if (ImGui::SliderFloat("Volume", &volumePct, 0.0f, 100.0f, "%.0f%%")) {
        m_volume = volumePct / 100.0f;
        if (m_engine->deviceInit) {
            ma_device_set_master_volume(&m_engine->device, m_volume);
        }
    }

    // --- File info ---
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("File:        %s", m_filename.c_str());
    ImGui::Text("Format:      %s", m_formatName.c_str());
    ImGui::Text("Sample Rate: %u Hz", m_sampleRate);
    ImGui::Text("Channels:    %u", m_channels);
    ImGui::Text("Duration:    %s (%.2f sec)", FormatTime(m_durationSec).c_str(), m_durationSec);
    ImGui::Text("Total Frames:%llu", static_cast<unsigned long long>(m_totalFrames));

    // Size
    float sizeMB = static_cast<float>(m_data.size()) / (1024.0f * 1024.0f);
    if (sizeMB >= 1.0f) {
        ImGui::Text("Data Size:   %.2f MB", sizeMB);
    } else {
        float sizeKB = static_cast<float>(m_data.size()) / 1024.0f;
        ImGui::Text("Data Size:   %.1f KB", sizeKB);
    }

    // Auto-detect end of playback: if playing and cursor >= totalFrames, reset state
    if (m_playing && !m_paused && m_totalFrames > 0 && cursor >= m_totalFrames) {
        ma_device_stop(&m_engine->device);
        ma_decoder_seek_to_pcm_frame(&m_engine->decoder, 0);
        m_playing = false;
        m_paused  = false;
    }
}

} // namespace GGPKExp
