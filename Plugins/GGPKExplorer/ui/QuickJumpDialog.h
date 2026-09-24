#pragma once
#include "../core/VirtualTree.h"
#include <string>
#include <vector>
#include <functional>
#include <imgui.h>

namespace GGPKExp {

class QuickJumpDialog {
public:
    using NavigateCallback = std::function<void(const std::string&)>;

    void SetTree(VirtualTree* tree) { m_tree = tree; }
    void SetOnNavigate(NavigateCallback cb) { m_onNavigate = std::move(cb); }

    void Open();
    void Close() { m_showQuickJump = false; }
    bool IsOpen() const { return m_showQuickJump; }

    void Render();

private:
    void UpdateResults(const char* query);

    VirtualTree* m_tree = nullptr;  // non-owning, lifetime guaranteed by GGPKExplorer
    NavigateCallback m_onNavigate;

    bool m_showQuickJump = false;
    char m_quickJumpBuf[256] = {};
    struct QuickJumpResult { std::string path; std::string name; int score; };
    std::vector<QuickJumpResult> m_quickJumpResults;
    int m_quickJumpSelected = 0;
    bool m_quickJumpNeedsFocus = false;
};

} // namespace GGPKExp
