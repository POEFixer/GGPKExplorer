#pragma once
#include <string>
#include <vector>
#include <functional>

namespace GGPKExp {

class VirtualTree;
class GGPKManager;

// -----------------------------------------------------------------------------
// Phase 5 / T17 — reusable GGPK file-picker modal.
//
// Shows every file in the GGPK (filtered by extension list) with a search
// box. Click a file → onSelect callback fires with the full path.
//
// Source preference:
//   * GGPKManager (when provided) — covers BOTH loose GGPK files and bundle2
//     contents (~hundreds of thousands of files in POE2).
//   * VirtualTree (fallback) — covers only loose GGPK files (~1k in POE2).
//     Used if the caller has no manager handy.
//
// Usage:
//   picker.Open(mgr, tree, ".csd", {}, [](const std::string& p){ ... });
//   // In your Render loop, call picker.Render() at WINDOW scope (outside
//   // any BeginChild) — ImGui modals live at window scope.
//
// filterExt: single extension including dot (e.g. ".csd"), empty = any.
// filterExts: alternative list (e.g. {".csd", ".otc"}), empty = ignored.
// -----------------------------------------------------------------------------
class GgpkTreePicker {
public:
    // Preferred: pass both sources; manager wins when non-null.
    void Open(GGPKManager* manager,
              VirtualTree* tree,
              const std::string& filterExt,
              const std::vector<std::string>& filterExts,
              std::function<void(const std::string&)> onSelect);

    // Legacy overload — tree-only. Kept for any callers that don't have a
    // manager handle; covers loose files only.
    void Open(VirtualTree* tree,
              const std::string& filterExt,
              const std::vector<std::string>& filterExts,
              std::function<void(const std::string&)> onSelect);

    // Call every frame from window scope (outside BeginChild).
    void Render();

    bool IsOpen() const { return m_open; }

private:
    GGPKManager* m_manager = nullptr;
    VirtualTree* m_tree = nullptr;
    std::string m_filter;
    std::vector<std::string> m_filters;
    std::function<void(const std::string&)> m_onSelect;
    bool m_open = false;
    bool m_openRequested = false;
    char m_searchBuf[256] = {};

    bool ExtensionMatches(const std::string& pathLower) const;
};

} // namespace GGPKExp
