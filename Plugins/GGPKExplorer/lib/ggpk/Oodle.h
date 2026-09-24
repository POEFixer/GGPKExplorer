#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace GGPK {

/// Oodle compression/decompression wrapper for oo2core.dll.
/// All methods are thread-safe but Initialize() must be called once per thread before use.
class Oodle {
public:
    enum class Compressor : int32_t {
        Invalid = -1,
        None = 3,
        Kraken = 8,
        Mermaid = 9,
        Selkie = 11,
        Hydra = 12,
        Leviathan = 13
    };

    enum class CompressionLevel : int32_t {
        None = 0,
        SuperFast = 1,
        VeryFast = 2,
        Fast = 3,
        Normal = 4,
        Optimal1 = 5,
        Optimal2 = 6,
        Optimal3 = 7,
        Optimal4 = 8,
        Optimal5 = 9,
        HyperFast1 = -1,
        HyperFast2 = -2,
        HyperFast3 = -3,
        HyperFast4 = -4
    };

    struct Settings {
        int32_t chunkSize = 256 * 1024; // 256KB
        Compressor compressor = Compressor::Leviathan;
        CompressionLevel compressionLevel = CompressionLevel::Normal;
        bool enableCompressing = true;
    };

    /// Load oo2core.dll from the given path. Must be called once before any other method.
    static void LoadDll(const std::wstring& dllPath);

    /// Initialize thread-local state. Must be called per-thread before Compress/Decompress.
    static void Initialize(const Settings& settings);

    /// Compress data. Returns compressed size. Output buffer must be at least GetCompressedBufferSize() bytes.
    static int32_t Compress(const uint8_t* input, intptr_t inputSize, uint8_t* output);

    /// Decompress data. Returns decompressed size.
    static intptr_t Decompress(const uint8_t* input, intptr_t inputSize, uint8_t* output, intptr_t outputSize);

    /// Get minimum output buffer size needed for compression.
    static int32_t GetCompressedBufferSize();
    static int32_t GetCompressedBufferSize(intptr_t uncompressedSize);

    /// Release thread-local resources (optional, auto-released on thread exit).
    static void Release();

    /// Check if the DLL is loaded.
    static bool IsLoaded();

private:
    // Function pointer types matching oo2core exports
    using OodleLZ_CompressFn = intptr_t(__stdcall*)(
        int32_t compressor, const uint8_t* buffer, intptr_t bufferSize,
        uint8_t* output, int32_t level,
        void* pOptions, void* dictBase, void* lrm,
        void* scratchMem, intptr_t scratchSize);

    using OodleLZ_DecompressFn = intptr_t(__stdcall*)(
        const uint8_t* buffer, intptr_t bufferSize,
        uint8_t* output, intptr_t outputSize,
        int32_t fuzzSafe, int32_t checkCRC, int32_t verbosity,
        uint8_t* dictBase, intptr_t dictSize,
        void* fpCallback, void* callbackUserData,
        void* decoderMem, intptr_t decoderMemSize, int32_t threadPhase);

    using OodleLZ_GetCompressedBufferSizeNeededFn = intptr_t(__stdcall*)(
        int32_t compressor, intptr_t bufferSize);

    using OodleLZ_GetCompressScratchMemBoundFn = intptr_t(__stdcall*)(
        int32_t compressor, int32_t level, intptr_t bufferSize, void* pOptions);

    using OodleLZDecoder_MemorySizeNeededFn = int32_t(__stdcall*)(
        int32_t compressor, intptr_t outputSize);

    // DLL handle and function pointers (global, loaded once)
    static void* s_dllHandle;
    static OodleLZ_CompressFn s_compress;
    static OodleLZ_DecompressFn s_decompress;
    static OodleLZ_GetCompressedBufferSizeNeededFn s_getCompressedBufferSizeNeeded;
    static OodleLZ_GetCompressScratchMemBoundFn s_getCompressScratchMemBound;
    static OodleLZDecoder_MemorySizeNeededFn s_decoderMemorySizeNeeded;

    // Thread-local state
    struct ThreadState {
        Settings settings{};
        std::vector<uint8_t> scratchMemory;
        bool initialized = false;
    };
    static thread_local ThreadState t_state;
};

} // namespace GGPK
