#pragma once
#include "IViewer.h"
#include <string>
#include <vector>
#include <cstdint>
#include <d3d11.h>
#include <wrl/client.h>
#include <imgui.h>

namespace GGPKExp {

class ImageViewer : public IViewer {
public:
    ~ImageViewer();

    void Initialize(ID3D11Device* device);
    void LoadData(std::vector<uint8_t> data, const std::string& path) override;
    void Clear() override;
    void Render() override;
    bool ExportPNG(const std::string& outputPath);

    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    ID3D11Device* m_device = nullptr;  // non-owning, lifetime guaranteed by host (ImGui D3D device)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    int m_width = 0;
    int m_height = 0;
    float m_zoom = 1.0f;
    ImVec2 m_pan{0, 0};
    bool m_dragging = false;
    ImVec2 m_dragStart{0, 0};
    ImVec2 m_panStart{0, 0};
    std::string m_formatName;

    std::vector<uint8_t> m_rawData;
    bool m_isDDS = false;

    void ReleaseTexture();
    bool LoadDDS(const uint8_t* data, size_t size);
    bool LoadRaster(const uint8_t* data, size_t size);
    bool CreateTextureFromRGBA(const uint8_t* pixels, int w, int h);

    static std::string DXGIFormatToString(DXGI_FORMAT fmt);
};

} // namespace GGPKExp
