#include "GgpkTreePicker.h"
#include "../../core/GGPKManager.h"
#include "../../core/VirtualTree.h"
#include <imgui.h>
#include <cfloat>
#include <cctype>

namespace GGPKExp {

// -----------------------------------------------------------------------------
// Phase 5 / T17 — reusable tree picker. See header for contract.
//
// Lookup note: VirtualTree::ForEachFileUnder uses a void-returning visitor
// (no early-termination via bool). Once the user clicks a file we must set
// m_open=false and let the loop finish naturally; we also stop appending to
// the visible list by short-circuiting with m_open check.
// -----------------------------------------------------------------------------

namespace {

std::string ToLowerAscii(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) {
        o.push_back(static_cast<char>(std::tolower(c)));
    }
    return o;
}

} // namespace

void GgpkTreePicker::Open(GGPKManager* manager,
                          VirtualTree* tree,
                          const std::string& filterExt,
                          const std::vector<std::string>& filterExts,
                          std::function<void(const std::string&)> onSelect)
{
    m_manager = manager;
    m_tree = tree;
    m_filter = filterExt;
    m_filters = filterExts;
    m_onSelect = std::move(onSelect);
    m_open = true;
    m_openRequested = true;
    m_searchBuf[0] = '\0';
}

void GgpkTreePicker::Open(VirtualTree* tree,
                          const std::string& filterExt,
                          const std::vector<std::string>& filterExts,
                          std::function<void(const std::string&)> onSelect)
{
    // Legacy overload — loose-file coverage only. Delegates to the full
    // overload with a null manager.
    Open(nullptr, tree, filterExt, filterExts, std::move(onSelect));
}

bool GgpkTreePicker::ExtensionMatches(const std::string& pathLower) const
{
    if (m_filter.empty() && m_filters.empty()) return true;
    if (!m_filter.empty()) {
        const auto& ext = m_filter;
        if (pathLower.size() >= ext.size() &&
            pathLower.compare(pathLower.size() - ext.size(), ext.size(), ext) == 0)
        {
            return true;
        }
    }
    for (const auto& ext : m_filters) {
        if (pathLower.size() >= ext.size() &&
            pathLower.compare(pathLower.size() - ext.size(), ext.size(), ext) == 0)
        {
            return true;
        }
    }
    return false;
}

void GgpkTreePicker::Render()
{
    if (!m_open) return;

    if (m_openRequested) {
        ImGui::OpenPopup("Browse GGPK");
        m_openRequested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(700.0f, 500.0f), ImGuiCond_Appearing);

    bool stayOpen = true;
    if (ImGui::BeginPopupModal("Browse GGPK", &stayOpen)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##search", "Filter by substring...",
                                 m_searchBuf, sizeof(m_searchBuf));

        const std::string needle = ToLowerAscii(m_searchBuf);

        const float footerH = ImGui::GetFrameHeightWithSpacing() + 8.0f;
        ImGui::BeginChild("##tree_files", ImVec2(0, -footerH), true);

        // Prefer GGPKManager enumeration (bundle-aware, ~hundreds of
        // thousands of paths). Fall back to VirtualTree for callers that
        // only wired the loose-file tree.
        auto drawRow = [&](const std::string& fullPath) {
            if (!m_open) return;
            const std::string lp = ToLowerAscii(fullPath);
            if (!ExtensionMatches(lp)) return;
            if (!needle.empty() && lp.find(needle) == std::string::npos) return;

            ImGui::PushID(fullPath.c_str());
            if (ImGui::Selectable(fullPath.c_str())) {
                if (m_onSelect) m_onSelect(fullPath);
                m_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        };

        if (m_manager) {
            m_manager->ForEachFile(
                [&](const std::string& fullPath, int64_t /*fileSize*/, void* /*nodePtr*/) {
                    drawRow(fullPath);
                });
        } else if (m_tree) {
            m_tree->ForEachFileUnder("",
                [&](const std::string& fullPath, const std::string& /*name*/,
                    int64_t /*fileSize*/, void* /*nodePtr*/) {
                    drawRow(fullPath);
                });
        }

        ImGui::EndChild();

        if (ImGui::Button("Cancel")) {
            m_open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (!stayOpen) {
        m_open = false;
    }
}

} // namespace GGPKExp
