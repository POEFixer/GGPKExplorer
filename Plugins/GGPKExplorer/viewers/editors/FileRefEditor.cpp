#include "../CellEditorPanel.h"
#include "../DatViewer.h"
#include "../../core/GGPKManager.h"
#include "../../core/VirtualTree.h"
#include "IconHelpers.h"
#include <imgui.h>
#include <cfloat>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <string>
#include <variant>
#include <vector>

namespace GGPKExp {

// -----------------------------------------------------------------------------
// Phase 5 / T16 — FileRef cell editor.
//
// UI layout:
//   [ file: .csd ]   (column hint, optional)
//   [ input text                                 ]
//   OK File exists / ! Not found in GGPK         (live validation)
//   [-------- autocomplete dropdown (≤10) ------]   (only when ≥3 chars)
//   -------------------------------------------------
//   [ Browse... ] [ Clear ]
//
// Live validation runs on every keystroke (FindByPathInsensitive is cheap —
// O(1) hashmap lookup). The full-tree autocomplete scan is throttled to
// 100ms-after-last-type since it iterates all files.
//
// Picker modal (T17) is stored as a member but its Render() is called from
// CellEditorPanel::Render at WINDOW scope (outside BeginChild) — ImGui modals
// live at window scope.
// -----------------------------------------------------------------------------

namespace {

std::string ToLowerAsciiFR(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) {
        o.push_back(static_cast<char>(std::tolower(c)));
    }
    return o;
}

bool MatchesFileFilter(const std::string& pathLower, const ColumnDef& col) {
    if (col.refFile.empty() && col.refFiles.empty()) return true;
    if (!col.refFile.empty()) {
        const auto& ext = col.refFile;
        if (pathLower.size() >= ext.size() &&
            pathLower.compare(pathLower.size() - ext.size(), ext.size(), ext) == 0)
        {
            return true;
        }
    }
    for (const auto& ext : col.refFiles) {
        if (pathLower.size() >= ext.size() &&
            pathLower.compare(pathLower.size() - ext.size(), ext.size(), ext) == 0)
        {
            return true;
        }
    }
    return false;
}

} // namespace

void CellEditorPanel::RenderFileRef()
{
    if (!m_colDef || !m_owner) return;

    const bool fa6 = m_owner->IsFA6Available();
    // Both sources are non-owning pointers held by DatViewer. We rely on the
    // GGPKManager-backed index for validation + autocomplete because the
    // VirtualTree only sees loose GGPK files (~1k in POE2) while most asset
    // paths live inside bundle2 (~600k). Tree is still passed to the picker
    // for now as a fallback — the picker itself prefers the manager when
    // available.
    GGPKManager* mgr = m_owner->GetGGPKManager();
    VirtualTree*  tree = m_owner->GetTree();
    if (!mgr && !tree) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "GGPK not available");
        return;
    }

    auto fileExists = [&](const std::string& p) -> bool {
        if (p.empty()) return false;
        if (mgr) return mgr->FindNodeByPath(p) != nullptr;
        return tree && (tree->FindByPathInsensitive(p) != UINT32_MAX);
    };

    // -------------------------------------------------------------------------
    // 1. Re-initialize the input buffer whenever the panel re-binds to a new
    //    (row,col). We also re-query existence for the initial value so the
    //    validation indicator is correct on first frame without requiring a
    //    keystroke.
    // -------------------------------------------------------------------------
    if (m_fileRefLastBoundRow != m_row || m_fileRefLastBoundCol != m_col) {
        const std::string cur = std::holds_alternative<std::string>(m_buffer)
            ? std::get<std::string>(m_buffer)
            : std::string{};
        strncpy_s(m_fileRefBuf, sizeof(m_fileRefBuf),
                  cur.c_str(), sizeof(m_fileRefBuf) - 1);
        m_fileRefBuf[sizeof(m_fileRefBuf) - 1] = '\0';
        m_fileRefCompletions.clear();
        m_fileRefExists = fileExists(cur);
        m_fileRefLastBoundRow = m_row;
        m_fileRefLastBoundCol = m_col;
        m_fileRefNeedsRescan = false;
    }

    // -------------------------------------------------------------------------
    // 2. Column hint (refFile / refFiles from schema).
    // -------------------------------------------------------------------------
    if (!m_colDef->refFile.empty()) {
        ImGui::TextDisabled("file: %s", m_colDef->refFile.c_str());
    }
    if (!m_colDef->refFiles.empty()) {
        std::string joined;
        for (const auto& f : m_colDef->refFiles) {
            if (!joined.empty()) joined += ", ";
            joined += f;
        }
        ImGui::TextDisabled("files: [%s]", joined.c_str());
    }

    // -------------------------------------------------------------------------
    // 3. Text input. Each keystroke → update buffer, mark dirty, schedule
    //    a throttled rescan.
    // -------------------------------------------------------------------------
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##filepath", m_fileRefBuf, sizeof(m_fileRefBuf))) {
        m_buffer = std::string(m_fileRefBuf);
        m_dirty = true;
        m_fileRefLastTypeTime = ImGui::GetTime();
        m_fileRefNeedsRescan = true;
    }

    // -------------------------------------------------------------------------
    // 4. Throttled rescan — runs 100ms after the last keystroke. Updates both
    //    existence validation and autocomplete list (filtered by extension).
    //    Autocomplete source prefers GGPKManager (bundle-aware); falls back
    //    to VirtualTree when no manager is available.
    // -------------------------------------------------------------------------
    const double now = ImGui::GetTime();
    if (m_fileRefNeedsRescan && (now - m_fileRefLastTypeTime) > 0.1) {
        m_fileRefNeedsRescan = false;
        const std::string path(m_fileRefBuf);
        m_fileRefExists = fileExists(path);

        m_fileRefCompletions.clear();
        if (path.size() >= 3) {
            const std::string needle = ToLowerAsciiFR(path);
            const ColumnDef& col = *m_colDef;
            if (mgr) {
                mgr->ForEachFile(
                    [&](const std::string& fullPath, int64_t /*fileSize*/, void* /*nodePtr*/) {
                        if (m_fileRefCompletions.size() >= 10) return;
                        const std::string lp = ToLowerAsciiFR(fullPath);
                        if (!MatchesFileFilter(lp, col)) return;
                        if (lp.find(needle) == std::string::npos) return;
                        m_fileRefCompletions.push_back(fullPath);
                    });
            } else if (tree) {
                // Visitor is void-returning; cap via size() check inside.
                tree->ForEachFileUnder("",
                    [&](const std::string& fullPath, const std::string& /*name*/,
                        int64_t /*fileSize*/, void* /*nodePtr*/)
                    {
                        if (m_fileRefCompletions.size() >= 10) return;
                        const std::string lp = ToLowerAsciiFR(fullPath);
                        if (!MatchesFileFilter(lp, col)) return;
                        if (lp.find(needle) == std::string::npos) return;
                        m_fileRefCompletions.push_back(fullPath);
                    });
            }
        }
    }

    // -------------------------------------------------------------------------
    // 5. Validation indicator.
    // -------------------------------------------------------------------------
    if (m_fileRefBuf[0]) {
        if (m_fileRefExists) {
            const std::string okLbl = IconLabel(fa6, kIconOk, "File exists");
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", okLbl.c_str());
        } else {
            const std::string warnLbl = IconLabel(fa6, kIconWarn, "Not found in GGPK");
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", warnLbl.c_str());
        }
    }

    // -------------------------------------------------------------------------
    // 6. Autocomplete dropdown.
    // -------------------------------------------------------------------------
    if (!m_fileRefCompletions.empty()) {
        ImGui::BeginChild("##ac_list", ImVec2(0, 150), true);
        for (const auto& p : m_fileRefCompletions) {
            ImGui::PushID(p.c_str());
            if (ImGui::Selectable(p.c_str())) {
                strncpy_s(m_fileRefBuf, sizeof(m_fileRefBuf),
                          p.c_str(), sizeof(m_fileRefBuf) - 1);
                m_fileRefBuf[sizeof(m_fileRefBuf) - 1] = '\0';
                m_buffer = p;
                m_dirty = true;
                m_fileRefExists = true;
                m_fileRefCompletions.clear();
                ImGui::PopID();
                break; // m_fileRefCompletions just mutated
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    ImGui::Separator();

    // -------------------------------------------------------------------------
    // 7. Browse / Clear buttons.
    // -------------------------------------------------------------------------
    const std::string lblBrowse = IconLabel(fa6, kIconBrowse, "Browse...");
    if (ImGui::Button(lblBrowse.c_str())) {
        std::vector<std::string> filters = m_colDef->refFiles;
        // Pass the manager first so the picker lists bundle contents too.
        m_fileRefPicker.Open(mgr, tree, m_colDef->refFile, filters,
            [this](const std::string& selected) {
                strncpy_s(m_fileRefBuf, sizeof(m_fileRefBuf),
                          selected.c_str(), sizeof(m_fileRefBuf) - 1);
                m_fileRefBuf[sizeof(m_fileRefBuf) - 1] = '\0';
                m_buffer = selected;
                m_dirty = true;
                m_fileRefExists = true;
                m_fileRefCompletions.clear();
            });
    }
    ImGui::SameLine();
    const std::string lblClearFR = IconLabel(fa6, kIconClear, "Clear");
    if (ImGui::Button(lblClearFR.c_str())) {
        m_fileRefBuf[0] = '\0';
        m_buffer = std::string{};
        m_dirty = true;
        m_fileRefExists = false;
        m_fileRefCompletions.clear();
    }

    // Picker modal is rendered from CellEditorPanel::Render at window scope
    // (outside BeginChild). Do NOT call m_fileRefPicker.Render() here.
}

} // namespace GGPKExp
