#pragma once
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <string>

namespace GGPK {

/// Hash functions for file path lookup in the Index.
/// Must produce identical results to LibGGPK3 C# implementation.
namespace NameHash {

    /// MurmurHash64A - used since PoE 3.21.2.
    /// Input must be lowercased UTF-8 bytes (without trailing '/').
    inline uint64_t MurmurHash64A(const uint8_t* data, size_t length, uint64_t seed = 0x1337B33F) {
        if (length == 0)
            return 0xF42A94E69CFF42FEull;

        // TrimEnd('/')
        while (length > 0 && data[length - 1] == '/')
            --length;
        if (length == 0)
            return 0xF42A94E69CFF42FEull;

        constexpr uint64_t m = 0xC6A4A7935BD1E995ull;
        constexpr int r = 47;

        seed ^= static_cast<uint64_t>(length) * m;

        const uint8_t* p = data;
        const uint8_t* end = data + (length / 8) * 8;

        while (p < end) {
            uint64_t k;
            std::memcpy(&k, p, 8); // little-endian safe on x64
            k *= m;
            seed = (seed ^ (k ^ (k >> r)) * m) * m;
            p += 8;
        }

        size_t remaining = length % 8;
        if (remaining != 0) {
            // Load remaining bytes into uint64, zeroing upper bytes
            uint64_t k = 0;
            std::memcpy(&k, p, remaining);
            // Mask: only keep 'remaining' bytes worth of bits
            // On little-endian x64, memcpy already gives us the right layout
            seed = (seed ^ k) * m;
        }

        seed = (seed ^ (seed >> r)) * m;
        return seed ^ (seed >> r);
    }

    /// FNV1a64Hash - used for GGPK before patch 3.21.2.
    /// Appends "++" to the path internally. Does case-lowering for A-Z.
    inline uint64_t FNV1a64Hash(const uint8_t* data, size_t length) {
        uint64_t hash = 0xCBF29CE484222325ull;
        constexpr uint64_t FNV_prime = 0x100000001B3ull;

        // TrimEnd('/')
        while (length > 0 && data[length - 1] == '/')
            --length;

        for (size_t i = 0; i < length; ++i) {
            uint64_t ch = data[i];
            // ToLower for A-Z
            if (ch >= 'A' && ch <= 'Z')
                ch = ch + ('a' - 'A');
            hash = (hash ^ ch) * FNV_prime;
        }

        // Append "++"
        hash = (hash ^ static_cast<uint64_t>('+')) * FNV_prime;
        hash = (hash ^ static_cast<uint64_t>('+')) * FNV_prime;

        return hash;
    }

    /// Compute the path hash for a file path string.
    /// Automatically lowercases for MurmurHash (post-3.21.2).
    /// `useMurmur` should be true for PoE >= 3.21.2 (detected from _Directories[0].PathHash).
    inline uint64_t ComputeHash(const std::string& path, bool useMurmur) {
        if (useMurmur) {
            // Lowercase the path first, then hash as UTF-8
            std::string lower = path;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return MurmurHash64A(reinterpret_cast<const uint8_t*>(lower.data()), lower.size());
        } else {
            // FNV1a does its own case conversion
            return FNV1a64Hash(reinterpret_cast<const uint8_t*>(path.data()), path.size());
        }
    }

    /// The expected hash of empty path for MurmurHash64A detection.
    constexpr uint64_t MURMUR_EMPTY_HASH = 0xF42A94E69CFF42FEull;

} // namespace NameHash

} // namespace GGPK
