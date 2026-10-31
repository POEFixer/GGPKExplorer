#include "ImageExport.h"

#include <DirectXTex.h>
#include <stb_image_write.h>

#include <fstream>

namespace GGPKExp::ImageExport {

namespace {

// RGBA8 pixels as PNG bytes.
bool RgbaToPngBytes(const uint8_t* pixels, int width, int height, int strideBytes,
                    std::vector<uint8_t>& png, std::string* err) {
    if (!pixels || width <= 0 || height <= 0) { if (err) *err = "invalid image"; return false; }

    png.clear();
    auto cb = [](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<uint8_t>*>(ctx);
        auto* bytes = static_cast<uint8_t*>(data);
        v->insert(v->end(), bytes, bytes + size);
    };
    if (stbi_write_png_to_func(cb, &png, width, height, 4, pixels, strideBytes) == 0 || png.empty()) {
        if (err) *err = "PNG encode failed";
        return false;
    }
    return true;
}

bool WritePng(const std::vector<uint8_t>& png, const std::filesystem::path& out, std::string* err) {
    std::ofstream os(out, std::ios::binary);
    if (!os.is_open()) { if (err) *err = "cannot open output file"; return false; }
    os.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!os.good()) { if (err) *err = "write failed"; return false; }
    return true;
}

} // namespace

bool RgbaToPng(const uint8_t* pixels, int width, int height, int strideBytes,
               const std::filesystem::path& out, std::string* err) {
    std::vector<uint8_t> png;
    if (!RgbaToPngBytes(pixels, width, height, strideBytes, png, err)) return false;
    return WritePng(png, out, err);
}

bool DdsBytesToPng(const std::vector<uint8_t>& ddsBytes,
                   const std::filesystem::path& out, std::string* err) {
    std::vector<uint8_t> png;
    if (!DdsBytesToPngBytes(ddsBytes, png, err)) return false;
    return WritePng(png, out, err);
}

bool DdsBytesToPngBytes(const std::vector<uint8_t>& ddsBytes, std::vector<uint8_t>& png, std::string* err) {
    if (ddsBytes.empty()) { if (err) *err = "empty DDS"; return false; }

    DirectX::TexMetadata metadata{};
    DirectX::ScratchImage ddsImage;
    HRESULT hr = DirectX::LoadFromDDSMemory(ddsBytes.data(), ddsBytes.size(),
                                            DirectX::DDS_FLAGS_NONE, &metadata, ddsImage);
    if (FAILED(hr)) { if (err) *err = "failed to parse DDS"; return false; }

    const DirectX::Image* src = ddsImage.GetImage(0, 0, 0);
    if (!src) { if (err) *err = "no DDS image data"; return false; }

    if (DirectX::IsCompressed(metadata.format)) {
        DirectX::ScratchImage decompressed;
        hr = DirectX::Decompress(*src, DXGI_FORMAT_R8G8B8A8_UNORM, decompressed);
        if (FAILED(hr)) { if (err) *err = "failed to decompress DDS"; return false; }
        const DirectX::Image* di = decompressed.GetImage(0, 0, 0);
        if (!di) { if (err) *err = "decompressed image null"; return false; }
        return RgbaToPngBytes(di->pixels, static_cast<int>(di->width), static_cast<int>(di->height),
                              static_cast<int>(di->rowPitch), png, err);
    }

    if (metadata.format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        DirectX::ScratchImage converted;
        hr = DirectX::Convert(*src, DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT,
                              DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) { if (err) *err = "failed to convert DDS to RGBA"; return false; }
        const DirectX::Image* ci = converted.GetImage(0, 0, 0);
        if (!ci) { if (err) *err = "converted image null"; return false; }
        return RgbaToPngBytes(ci->pixels, static_cast<int>(ci->width), static_cast<int>(ci->height),
                              static_cast<int>(ci->rowPitch), png, err);
    }

    return RgbaToPngBytes(src->pixels, static_cast<int>(src->width), static_cast<int>(src->height),
                          static_cast<int>(src->rowPitch), png, err);
}

} // namespace GGPKExp::ImageExport
