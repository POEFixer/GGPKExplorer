#pragma once
#include "IViewer.h"
#include <vector>
#include <cstdint>
#include <cstddef>

namespace GGPKExp {

class HexViewer : public IViewer {
public:
    void LoadData(std::vector<uint8_t> data, const std::string& path) override;
    void Clear() override;
    void Render() override;

private:
    std::vector<uint8_t> m_data;
};

} // namespace GGPKExp
