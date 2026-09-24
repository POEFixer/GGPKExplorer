#pragma once
#include <functional>
#include <vector>
#include <imgui.h>

namespace GGPKExp {

class KeyboardShortcuts {
public:
    struct Binding {
        ImGuiKey key;
        bool ctrl = false;
        bool shift = false;
        bool alt = false;
        std::function<void()> action;
    };

    void Register(ImGuiKey key, bool ctrl, bool shift, bool alt, std::function<void()> action);
    void ProcessInput();
    void Clear();

private:
    std::vector<Binding> m_bindings;
};

} // namespace GGPKExp
