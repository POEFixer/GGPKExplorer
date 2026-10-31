#include "ImageViewer.h"

#include <DDSTextureLoader11.h>
#include <DirectXTex.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include "../core/ImageExport.h"
#include "../core/PathUtils.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <commdlg.h>

namespace GGPKExp {

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static std::string ToLowerExt(const std::string& filename) {
    auto dot = filename.rfind('.');
    if (dot == std::string::npos) return {};
    std::string ext = filename.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

static bool IsDDSData(const uint8_t* data, size_t size) {
    // DDS files start with the magic number 0x20534444 ("DDS ")
    if (size < 4) return false;
    return data[0] == 'D' && data[1] == 'D' && data[2] == 'S' && data[3] == ' ';
}

std::string ImageViewer::DXGIFormatToString(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:       return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:  return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM:       return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:  return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8X8_UNORM:       return "B8G8R8X8_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:   return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_FLOAT:   return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R10G10B10A2_UNORM:    return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_BC1_UNORM:            return "BC1 (DXT1)";
    case DXGI_FORMAT_BC1_UNORM_SRGB:       return "BC1 (DXT1) sRGB";
    case DXGI_FORMAT_BC2_UNORM:            return "BC2 (DXT3)";
    case DXGI_FORMAT_BC2_UNORM_SRGB:       return "BC2 (DXT3) sRGB";
    case DXGI_FORMAT_BC3_UNORM:            return "BC3 (DXT5)";
    case DXGI_FORMAT_BC3_UNORM_SRGB:       return "BC3 (DXT5) sRGB";
    case DXGI_FORMAT_BC4_UNORM:            return "BC4";
    case DXGI_FORMAT_BC4_SNORM:            return "BC4 Signed";
    case DXGI_FORMAT_BC5_UNORM:            return "BC5";
    case DXGI_FORMAT_BC5_SNORM:            return "BC5 Signed";
    case DXGI_FORMAT_BC6H_UF16:            return "BC6H (unsigned)";
    case DXGI_FORMAT_BC6H_SF16:            return "BC6H (signed)";
    case DXGI_FORMAT_BC7_UNORM:            return "BC7";
    case DXGI_FORMAT_BC7_UNORM_SRGB:       return "BC7 sRGB";
    case DXGI_FORMAT_R8_UNORM:             return "R8_UNORM";
    case DXGI_FORMAT_A8_UNORM:             return "A8_UNORM";
    case DXGI_FORMAT_R8G8_UNORM:           return "R8G8_UNORM";
    default: {
        char buf[32];
        snprintf(buf, sizeof(buf), "DXGI Format %d", static_cast<int>(fmt));
        return buf;
    }
    }
}

// -----------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------

ImageViewer::~ImageViewer() {
    ReleaseTexture();
}

void ImageViewer::Initialize(ID3D11Device* device) {
    m_device = device;
}

void ImageViewer::ReleaseTexture() {
    m_srv.Reset();
    m_width = 0;
    m_height = 0;
}

void ImageViewer::Clear() {
    ReleaseTexture();
    m_rawData.clear();
    m_isDDS = false;
    m_zoom = 1.0f;
    m_pan = {0, 0};
    m_dragging = false;
    m_formatName.clear();
    m_loaded = false;
    m_error.clear();
}

// -----------------------------------------------------------------------
// LoadDDS — uses DDSTextureLoader11 for GPU texture, gets dimensions
// -----------------------------------------------------------------------

bool ImageViewer::LoadDDS(const uint8_t* data, size_t size) {
    if (!m_device) {
        m_error = "No D3D device available";
        return false;
    }

    HRESULT hr = DirectX::CreateDDSTextureFromMemory(
        m_device, data, size, nullptr, m_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Failed to load DDS texture (HRESULT 0x%08lX)", hr);
        m_error = buf;
        return false;
    }

    // Extract dimensions from the SRV's underlying texture
    Microsoft::WRL::ComPtr<ID3D11Resource> res;
    m_srv->GetResource(res.GetAddressOf());
    if (res) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        hr = res.As(&tex);
        if (SUCCEEDED(hr) && tex) {
            D3D11_TEXTURE2D_DESC desc{};
            tex->GetDesc(&desc);
            m_width = static_cast<int>(desc.Width);
            m_height = static_cast<int>(desc.Height);
            m_formatName = "DDS " + DXGIFormatToString(desc.Format);
        }
    }

    return true;
}

// -----------------------------------------------------------------------
// LoadRaster — uses stb_image for PNG/JPG/BMP/TGA/HDR
// -----------------------------------------------------------------------

bool ImageViewer::LoadRaster(const uint8_t* data, size_t size) {
    if (!m_device) {
        m_error = "No D3D device available";
        return false;
    }

    int w = 0, h = 0, comp = 0;
    unsigned char* pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &comp, 4);
    if (!pixels) {
        m_error = "Failed to decode image: ";
        m_error += stbi_failure_reason();
        return false;
    }

    bool ok = CreateTextureFromRGBA(pixels, w, h);
    stbi_image_free(pixels);

    if (!ok) {
        m_error = "Failed to create GPU texture from image data";
        return false;
    }

    m_width = w;
    m_height = h;

    const char* compName = "RGBA";
    switch (comp) {
    case 1: compName = "Grayscale"; break;
    case 2: compName = "Gray+Alpha"; break;
    case 3: compName = "RGB"; break;
    case 4: compName = "RGBA"; break;
    }
    m_formatName = compName;

    return true;
}

// -----------------------------------------------------------------------
// CreateTextureFromRGBA — uploads raw RGBA pixels to D3D11
// -----------------------------------------------------------------------

bool ImageViewer::CreateTextureFromRGBA(const uint8_t* pixels, int w, int h) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA initData{};
    initData.pSysMem = pixels;
    initData.SysMemPitch = static_cast<UINT>(w * 4);
    initData.SysMemSlicePitch = 0;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = m_device->CreateTexture2D(&desc, &initData, &tex);
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    hr = m_device->CreateShaderResourceView(tex, &srvDesc, m_srv.ReleaseAndGetAddressOf());
    tex->Release();

    return SUCCEEDED(hr);
}

// -----------------------------------------------------------------------
// LoadData — main entry: detect format, dispatch to loader
// -----------------------------------------------------------------------

void ImageViewer::LoadData(std::vector<uint8_t> data, const std::string& path) {
    ReleaseTexture();
    m_error.clear();
    m_rawData = std::move(data);
    m_isDDS = false;
    m_loaded = false;

    if (m_rawData.empty()) {
        m_error = "Empty file data";
        return;
    }

    if (!m_device) {
        m_error = "No D3D device — image display unavailable";
        return;
    }

    // Detect DDS by magic bytes first, then by extension
    bool isDDS = IsDDSData(m_rawData.data(), m_rawData.size());
    if (!isDDS) {
        std::string ext = ToLowerExt(path);
        isDDS = (ext == ".dds");
    }

    bool ok = false;
    if (isDDS) {
        m_isDDS = true;
        ok = LoadDDS(m_rawData.data(), m_rawData.size());
    } else {
        ok = LoadRaster(m_rawData.data(), m_rawData.size());
    }

    // Reset view
    m_zoom = 1.0f;
    m_pan = {0, 0};
    m_dragging = false;
    m_loaded = ok;
}

// -----------------------------------------------------------------------
// Render — draws the image viewer UI with zoom/pan/export controls
// -----------------------------------------------------------------------

void ImageViewer::Render() {
    if (!m_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error: %s", m_error.c_str());
        return;
    }

    if (!m_loaded || !m_srv.Get()) {
        ImGui::TextDisabled("No image loaded");
        return;
    }

    // -- Info bar --
    ImGui::Text("%dx%d | %s | Zoom: %.0f%%", m_width, m_height, m_formatName.c_str(), m_zoom * 100.0f);

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // Fit to window button
    if (ImGui::SmallButton("Fit") || (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_0))) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        avail.y -= ImGui::GetFrameHeightWithSpacing(); // account for info bar
        if (m_width > 0 && m_height > 0) {
            float zoomX = avail.x / static_cast<float>(m_width);
            float zoomY = avail.y / static_cast<float>(m_height);
            m_zoom = (std::min)(zoomX, zoomY);
            if (m_zoom < 0.01f) m_zoom = 0.01f;
            m_pan = {0, 0};
        }
    }

    ImGui::SameLine();

    // 1:1 button
    if (ImGui::SmallButton("1:1") || (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_1))) {
        m_zoom = 1.0f;
        m_pan = {0, 0};
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // Export PNG button
    if (ImGui::SmallButton("Export PNG")) {
        wchar_t savePath[MAX_PATH] = {};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFilter = L"PNG Files (*.png)\0*.png\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = savePath;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = L"png";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
        ofn.lpstrTitle = L"Export as PNG";

        if (GetSaveFileNameW(&ofn)) {
            const std::string utf8Path = PathUtils::WideToUtf8(savePath);
            if (!utf8Path.empty()) ExportPNG(utf8Path);
        }
    }

    // -- Image area --
    ImGui::BeginChild("##ImageArea", ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);

    // Zoom with mouse wheel
    if (ImGui::IsWindowHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            float oldZoom = m_zoom;
            m_zoom *= (1.0f + wheel * 0.1f);
            m_zoom = std::clamp(m_zoom, 0.1f, 10.0f);

            // Zoom toward mouse position
            ImVec2 mousePos = ImGui::GetMousePos();
            ImVec2 windowPos = ImGui::GetWindowPos();
            ImVec2 localMouse{mousePos.x - windowPos.x, mousePos.y - windowPos.y};
            float zoomRatio = m_zoom / oldZoom;
            m_pan.x = localMouse.x - zoomRatio * (localMouse.x - m_pan.x);
            m_pan.y = localMouse.y - zoomRatio * (localMouse.y - m_pan.y);
        }
    }

    // Pan with middle mouse or right mouse button drag
    if (ImGui::IsWindowHovered() && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        m_dragging = true;
        m_dragStart = ImGui::GetMousePos();
        m_panStart = m_pan;
    }
    if (m_dragging) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            ImVec2 mousePos = ImGui::GetMousePos();
            m_pan.x = m_panStart.x + (mousePos.x - m_dragStart.x);
            m_pan.y = m_panStart.y + (mousePos.y - m_dragStart.y);
        } else {
            m_dragging = false;
        }
    }

    // Draw checkerboard background behind image
    ImVec2 imgSize{static_cast<float>(m_width) * m_zoom, static_cast<float>(m_height) * m_zoom};
    ImVec2 cursorScreen = ImGui::GetCursorScreenPos();
    ImVec2 imgOrigin{cursorScreen.x + m_pan.x, cursorScreen.y + m_pan.y};

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Clip checkerboard to visible area
    ImVec2 clipMin = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();
    ImVec2 clipMax{clipMin.x + windowSize.x, clipMin.y + windowSize.y};
    drawList->PushClipRect(clipMin, clipMax, true);

    // Checkerboard pattern (16px squares)
    {
        float checkSize = 16.0f;
        ImU32 colorA = IM_COL32(128, 128, 128, 255);
        ImU32 colorB = IM_COL32(192, 192, 192, 255);

        float startX = (std::max)(imgOrigin.x, clipMin.x);
        float startY = (std::max)(imgOrigin.y, clipMin.y);
        float endX = (std::min)(imgOrigin.x + imgSize.x, clipMax.x);
        float endY = (std::min)(imgOrigin.y + imgSize.y, clipMax.y);

        if (startX < endX && startY < endY) {
            for (float y = startY; y < endY; y += checkSize) {
                for (float x = startX; x < endX; x += checkSize) {
                    int gridX = static_cast<int>((x - imgOrigin.x) / checkSize);
                    int gridY = static_cast<int>((y - imgOrigin.y) / checkSize);
                    ImU32 col = ((gridX + gridY) % 2 == 0) ? colorA : colorB;

                    float rx0 = x;
                    float ry0 = y;
                    float rx1 = (std::min)(x + checkSize, endX);
                    float ry1 = (std::min)(y + checkSize, endY);
                    // Also clamp to image bounds
                    rx1 = (std::min)(rx1, imgOrigin.x + imgSize.x);
                    ry1 = (std::min)(ry1, imgOrigin.y + imgSize.y);

                    drawList->AddRectFilled(ImVec2(rx0, ry0), ImVec2(rx1, ry1), col);
                }
            }
        }
    }

    drawList->PopClipRect();

    // Draw image
    ImGui::SetCursorPos(ImVec2(m_pan.x, m_pan.y));
    ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(m_srv.Get())), imgSize);

    ImGui::EndChild();
}

// -----------------------------------------------------------------------
// ExportPNG — decode DDS or raster data to RGBA, write as PNG
// -----------------------------------------------------------------------

bool ImageViewer::ExportPNG(const std::string& outputPath) {
    if (m_rawData.empty()) {
        m_error = "No data to export";
        return false;
    }

    if (m_isDDS) {
        return ImageExport::DdsBytesToPng(m_rawData, PathUtils::FromUtf8(outputPath), &m_error);
    }

    // Raster: re-decode with stb_image and write
    int w = 0, h = 0, comp = 0;
    unsigned char* pixels = stbi_load_from_memory(
        m_rawData.data(), static_cast<int>(m_rawData.size()), &w, &h, &comp, 4);
    if (!pixels) {
        m_error = "Failed to decode image for export";
        return false;
    }

    bool ok = ImageExport::RgbaToPng(pixels, w, h, w * 4, PathUtils::FromUtf8(outputPath), &m_error);
    stbi_image_free(pixels);
    return ok;
}

} // namespace GGPKExp
