#include "KeyboardShortcuts.h"

namespace GGPKExp {

void KeyboardShortcuts::Register(ImGuiKey key, bool ctrl, bool shift, bool alt, std::function<void()> action) {
    m_bindings.push_back({ key, ctrl, shift, alt, std::move(action) });
}

void KeyboardShortcuts::ProcessInput() {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        return;

    auto& io = ImGui::GetIO();
    bool ctrlDown = io.KeyCtrl;
    bool shiftDown = io.KeyShift;
    bool altDown = io.KeyAlt;

    for (const auto& b : m_bindings) {
        if (b.ctrl != ctrlDown) continue;
        if (b.shift != shiftDown) continue;
        if (b.alt != altDown) continue;
        if (ImGui::IsKeyPressed(b.key)) {
            b.action();
        }
    }
}

void KeyboardShortcuts::Clear() {
    m_bindings.clear();
}

} // namespace GGPKExp
