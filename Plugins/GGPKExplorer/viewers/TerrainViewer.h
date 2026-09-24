#pragma once
#include "IViewer.h"
#include "../core/parsers/TerrainGridParser.h"
#include <string>
#include <vector>
#include <cstdint>
#include <d3d11.h>
#include <imgui.h>

namespace GGPKExp {

class TerrainViewer : public IViewer {
public:
    ~TerrainViewer() = default;

    void Initialize(ID3D11Device* device) { m_device = device; }
    void LoadData(std::vector<uint8_t> data, const std::string& path) override;
    void Clear() override;
    void Render() override;

    bool HasPendingNavigation() const override { return !m_pendingNav.empty(); }
    std::string TakePendingNavigation() override {
        std::string p = std::move(m_pendingNav);
        m_pendingNav.clear();
        return p;
    }

private:
    ID3D11Device* m_device = nullptr;
    Parsers::TdtData m_tdt;
    std::string m_pendingNav;

    void RenderPathLink(const char* label, const std::string& path);
};

} // namespace GGPKExp
