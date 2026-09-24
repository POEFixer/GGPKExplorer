#include "HexViewer.h"
#include <imgui.h>
#include <imgui_memory_editor.h>

namespace GGPKExp {

// Static instance — reused across calls (MemoryEditor is stateful per-instance for scroll etc.)
static MemoryEditor s_memEdit;

void HexViewer::LoadData(std::vector<uint8_t> data, const std::string& /*path*/) {
    m_data = std::move(data);
    m_loaded = true;
    m_error.clear();
}

void HexViewer::Clear() {
    m_data.clear();
    m_loaded = false;
    m_error.clear();
}

void HexViewer::Render() {
    if (!m_loaded || m_data.empty()) {
        ImGui::TextDisabled("No data loaded");
        return;
    }
    s_memEdit.ReadOnly = true;
    s_memEdit.DrawContents(const_cast<void*>(static_cast<const void*>(m_data.data())), m_data.size());
}

} // namespace GGPKExp
