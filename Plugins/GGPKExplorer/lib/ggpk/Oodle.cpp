#include "Oodle.h"
#include <Windows.h>
#include <algorithm>

namespace GGPK {

// Static member definitions
void* Oodle::s_dllHandle = nullptr;
Oodle::OodleLZ_CompressFn Oodle::s_compress = nullptr;
Oodle::OodleLZ_DecompressFn Oodle::s_decompress = nullptr;
Oodle::OodleLZ_GetCompressedBufferSizeNeededFn Oodle::s_getCompressedBufferSizeNeeded = nullptr;
Oodle::OodleLZ_GetCompressScratchMemBoundFn Oodle::s_getCompressScratchMemBound = nullptr;
Oodle::OodleLZDecoder_MemorySizeNeededFn Oodle::s_decoderMemorySizeNeeded = nullptr;
thread_local Oodle::ThreadState Oodle::t_state{};

void Oodle::LoadDll(const std::wstring& dllPath) {
    if (s_dllHandle)
        return; // Already loaded

    HMODULE hModule = LoadLibraryW(dllPath.c_str());
    if (!hModule)
        throw std::runtime_error("Failed to load oo2core.dll (error " + std::to_string(GetLastError()) + ")");

    s_compress = reinterpret_cast<OodleLZ_CompressFn>(
        GetProcAddress(hModule, "OodleLZ_Compress"));
    s_decompress = reinterpret_cast<OodleLZ_DecompressFn>(
        GetProcAddress(hModule, "OodleLZ_Decompress"));
    s_getCompressedBufferSizeNeeded = reinterpret_cast<OodleLZ_GetCompressedBufferSizeNeededFn>(
        GetProcAddress(hModule, "OodleLZ_GetCompressedBufferSizeNeeded"));
    s_getCompressScratchMemBound = reinterpret_cast<OodleLZ_GetCompressScratchMemBoundFn>(
        GetProcAddress(hModule, "OodleLZ_GetCompressScratchMemBound"));
    s_decoderMemorySizeNeeded = reinterpret_cast<OodleLZDecoder_MemorySizeNeededFn>(
        GetProcAddress(hModule, "OodleLZDecoder_MemorySizeNeeded"));

    if (!s_compress || !s_decompress || !s_getCompressedBufferSizeNeeded)
        throw std::runtime_error("Failed to resolve oo2core.dll exports");

    s_dllHandle = hModule;
}

bool Oodle::IsLoaded() {
    return s_dllHandle != nullptr;
}

void Oodle::Initialize(const Settings& settings) {
    if (!s_dllHandle)
        throw std::runtime_error("Oodle DLL not loaded. Call LoadDll() first.");

    if (settings.chunkSize <= 0)
        throw std::invalid_argument("ChunkSize must be positive");

    // Check if we can skip re-initialization (same or compatible settings)
    if (t_state.initialized &&
        t_state.settings.chunkSize >= settings.chunkSize &&
        (!settings.enableCompressing ||
         (t_state.settings.enableCompressing &&
          static_cast<int>(t_state.settings.compressionLevel) >= static_cast<int>(settings.compressionLevel) &&
          t_state.settings.compressor == settings.compressor))) {
        t_state.settings = settings;
        return;
    }

    // Calculate scratch memory size needed
    int32_t memSize = 0;
    if (s_decoderMemorySizeNeeded) {
        memSize = s_decoderMemorySizeNeeded(
            static_cast<int32_t>(Compressor::Invalid), // any compressor
            static_cast<intptr_t>(settings.chunkSize));
    }

    if (settings.enableCompressing && s_getCompressScratchMemBound) {
        auto scratchBound = static_cast<int32_t>(s_getCompressScratchMemBound(
            static_cast<int32_t>(settings.compressor),
            static_cast<int32_t>(settings.compressionLevel),
            static_cast<intptr_t>(settings.chunkSize),
            nullptr));
        if (scratchBound < 0) {
            // OODLELZ_SCRATCH_MEM_NO_BOUND(-1)
            memSize = 6 * 1024 * 1024; // 6MB fallback
        } else if (scratchBound > memSize) {
            memSize = scratchBound;
        }
    }

    if (memSize > 0 && (t_state.scratchMemory.empty() || static_cast<int32_t>(t_state.scratchMemory.size()) < memSize)) {
        t_state.scratchMemory.resize(static_cast<size_t>(memSize));
    }

    t_state.settings = settings;
    t_state.initialized = true;
}

int32_t Oodle::Compress(const uint8_t* input, intptr_t inputSize, uint8_t* output) {
    if (!t_state.initialized)
        throw std::runtime_error("Oodle not initialized on this thread. Call Initialize() first.");

    auto result = s_compress(
        static_cast<int32_t>(t_state.settings.compressor),
        input, inputSize,
        output,
        static_cast<int32_t>(t_state.settings.compressionLevel),
        nullptr, nullptr, nullptr,
        t_state.scratchMemory.empty() ? nullptr : t_state.scratchMemory.data(),
        static_cast<intptr_t>(t_state.scratchMemory.size()));

    // OodleLZ_Compress returns <= 0 (OODLELZ_FAILED) when the supplied scratch
    // buffer is too small for the chosen compressor/level. Retry letting Oodle
    // allocate its own workspace (scratch = null) before giving up — robust against
    // an undersized bound from OodleLZ_GetCompressScratchMemBound.
    if (result <= 0 && !t_state.scratchMemory.empty()) {
        result = s_compress(
            static_cast<int32_t>(t_state.settings.compressor),
            input, inputSize,
            output,
            static_cast<int32_t>(t_state.settings.compressionLevel),
            nullptr, nullptr, nullptr,
            nullptr, 0);
    }

    return static_cast<int32_t>(result);
}

intptr_t Oodle::Decompress(const uint8_t* input, intptr_t inputSize, uint8_t* output, intptr_t outputSize) {
    if (!t_state.initialized)
        throw std::runtime_error("Oodle not initialized on this thread. Call Initialize() first.");

    return s_decompress(
        input, inputSize,
        output, outputSize,
        1,  // fuzzSafe
        0,  // checkCRC
        0,  // verbosity
        nullptr, 0, // dict
        nullptr, nullptr, // callback
        t_state.scratchMemory.empty() ? nullptr : t_state.scratchMemory.data(),
        static_cast<intptr_t>(t_state.scratchMemory.size()),
        3); // threadPhase
}

int32_t Oodle::GetCompressedBufferSize() {
    return static_cast<int32_t>(s_getCompressedBufferSizeNeeded(
        static_cast<int32_t>(t_state.settings.compressor),
        static_cast<intptr_t>(t_state.settings.chunkSize)));
}

int32_t Oodle::GetCompressedBufferSize(intptr_t uncompressedSize) {
    return static_cast<int32_t>(s_getCompressedBufferSizeNeeded(
        static_cast<int32_t>(t_state.settings.compressor),
        uncompressedSize));
}

void Oodle::Release() {
    t_state.settings.chunkSize = 0;
    t_state.settings.enableCompressing = false;
    t_state.scratchMemory.clear();
    t_state.scratchMemory.shrink_to_fit();
    t_state.initialized = false;
}

} // namespace GGPK
