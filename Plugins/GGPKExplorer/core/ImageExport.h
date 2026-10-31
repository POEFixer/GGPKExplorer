#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace GGPKExp::ImageExport {

// Write RGBA8 pixels as PNG. Unicode-safe: encodes to memory, then writes the
// file through std::filesystem::path. Returns false on failure (*err set when non-null).
bool RgbaToPng(const uint8_t* pixels, int width, int height, int strideBytes,
               const std::filesystem::path& out, std::string* err);

// Decode a DDS blob (DirectXTex) to RGBA and write it as PNG (mip 0 / slice 0).
bool DdsBytesToPng(const std::vector<uint8_t>& ddsBytes,
                   const std::filesystem::path& out, std::string* err);

// The same into memory: `png` holds exactly the bytes DdsBytesToPng writes (thread-safe:
// Save As converts on several threads).
bool DdsBytesToPngBytes(const std::vector<uint8_t>& ddsBytes, std::vector<uint8_t>& png, std::string* err);

} // namespace GGPKExp::ImageExport
