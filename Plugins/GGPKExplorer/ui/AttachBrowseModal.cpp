#include "AttachBrowseModal.h"

#include "../core/GGPKManager.h"
#include "../core/VirtualTree.h"
#include "../core/FileTypeDetector.h"
#include "../viewers/aoc/ItemVisualIdentityCatalog.h"
#include "../viewers/aoc/ItemIconCache.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace GGPKExp {

namespace {

constexpr float kIconCellSize         = 32.0f;
constexpr float kIconCellPadding      =  6.0f;   // left + right padding around icon
constexpr int   kDefaultLoadsPerFrame =  8;

bool EndsWithCI(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[s.size() - n + i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b) return false;
    }
    return true;
}

std::string ToLowerAscii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // anonymous namespace

AttachBrowseModal::AttachBrowseModal()  = default;
AttachBrowseModal::~AttachBrowseModal() = default;

void AttachBrowseModal::Open(const std::string& socketName, AttachCallback onAttach) {
    m_showModal     = true;
    m_needsFocus    = true;
    m_targetSocket  = socketName;
    m_onAttach      = std::move(onAttach);
    m_filterBuf[0]  = '\0';
    m_showSm        = true;
    m_showAoc       = true;
    m_selectedIndex = -1;
    m_lastError.clear();

    // Reset DAT tab transient state so reopen starts fresh.
    m_datFilterBuf[0]    = '\0';
    m_datSelectedIndex   = -1;
    m_datSelectedFileIdx = -1;
    m_datLastError.clear();
    m_datNeedsFocus      = true;
    // Re-seed filter if catalog is loaded (no-op otherwise; EnsureDatCatalog
    // will seed on first tab activation).
    if (m_catalog && m_catalog->IsLoaded()) {
        ApplyDatFilter();
    }

    if (m_tree != m_cachedTreePtr) {
        RebuildFileCache();
    }
    ApplyFilter();
}

void AttachBrowseModal::Close() {
    m_showModal = false;
    m_onAttach  = nullptr;
    m_lastError.clear();
}

void AttachBrowseModal::RebuildFileCache() {
    m_allMeshPaths.clear();
    m_cachedTreePtr = m_tree;
    if (!m_tree) return;

    // NOTE: must use ForEachFileUnder (not GetFlatList) — GetFlatList only
    // returns entries the user has expanded in the tree panel, so a fresh
    // unexpanded session would expose only top-level files (~12 visible).
    // ForEachFileUnder walks the ENTIRE GGPK regardless of UI expansion.
    m_allMeshPaths.reserve(16384);
    m_tree->ForEachFileUnder("",
        [this](const std::string& fullPath,
               const std::string& /*name*/,
               int64_t /*fileSize*/,
               void* /*nodePtr*/) {
            // Accept .sm and .aoc only. FileTypeDetector returns FileType::Mesh
            // for .sm / .smd / .ast — we want only .sm, so we check extension
            // directly.
            if (EndsWithCI(fullPath, ".sm") || EndsWithCI(fullPath, ".aoc")) {
                m_allMeshPaths.push_back(fullPath);
            }
        });
    std::sort(m_allMeshPaths.begin(), m_allMeshPaths.end());
}

void AttachBrowseModal::ApplyFilter() {
    m_filteredIndices.clear();
    m_selectedIndex = -1;

    const std::string filter = ToLowerAscii(m_filterBuf);
    m_filteredIndices.reserve(m_allMeshPaths.size());

    for (std::size_t i = 0; i < m_allMeshPaths.size(); ++i) {
        const std::string& path = m_allMeshPaths[i];
        const bool isSm  = EndsWithCI(path, ".sm");
        const bool isAoc = EndsWithCI(path, ".aoc");
        if (isSm  && !m_showSm)  continue;
        if (isAoc && !m_showAoc) continue;

        if (!filter.empty()) {
            const std::string lower = ToLowerAscii(path);
            if (lower.find(filter) == std::string::npos) continue;
        }
        m_filteredIndices.push_back(i);
    }
}

void AttachBrowseModal::TriggerAttach() {
    m_lastError.clear();
    if (m_selectedIndex < 0 ||
        m_selectedIndex >= static_cast<int>(m_filteredIndices.size())) {
        m_lastError = "No selection";
        return;
    }
    if (!m_mgr) {
        m_lastError = "GGPK not loaded";
        return;
    }
    const std::string& path = m_allMeshPaths[m_filteredIndices[static_cast<std::size_t>(m_selectedIndex)]];
    void* node = m_mgr->FindNodeByPath(path);
    if (!node) {
        m_lastError = "File not found: " + path;
        return;
    }
    auto res = m_mgr->ReadFile(node);
    if (!res.success) {
        m_lastError = "Read failed: " + res.error;
        return;
    }
    if (m_onAttach) {
        m_onAttach(m_targetSocket, path, res.value);
    }
    Close();
}

void AttachBrowseModal::RenderFilesTab() {
    if (m_needsFocus) {
        ImGui::SetKeyboardFocusHere();
        m_needsFocus = false;
    }
    if (ImGui::InputTextWithHint("##AocBrowseFilter", "Filter by path...",
                                 m_filterBuf, sizeof(m_filterBuf))) {
        ApplyFilter();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(".sm", &m_showSm))  ApplyFilter();
    ImGui::SameLine();
    if (ImGui::Checkbox(".aoc", &m_showAoc)) ApplyFilter();

    ImGui::Text("Results: %zu / %zu", m_filteredIndices.size(), m_allMeshPaths.size());

    const float listHeight = ImGui::GetContentRegionAvail().y - 100.0f;
    ImGui::BeginChild("##AocBrowseList",
                      ImVec2(0.0f, listHeight > 100.0f ? listHeight : 100.0f), true);
    if (m_filteredIndices.empty()) {
        ImGui::TextDisabled("%s",
            m_allMeshPaths.empty() ? "No .sm / .aoc files in GGPK" : "No matches");
    } else {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_filteredIndices.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const std::string& path = m_allMeshPaths[m_filteredIndices[static_cast<std::size_t>(i)]];
                const bool selected = (m_selectedIndex == i);
                ImGui::PushID(i);
                if (ImGui::Selectable(path.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                    m_selectedIndex = i;
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        TriggerAttach();
                    }
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();

    // Early-return guard: if TriggerAttach above closed the modal (double-click),
    // do not fire the Enter-key block further down in the same frame.
    if (!m_showModal) return;

    if (m_selectedIndex >= 0 &&
        m_selectedIndex < static_cast<int>(m_filteredIndices.size())) {
        ImGui::TextWrapped("Selected: %s",
            m_allMeshPaths[m_filteredIndices[static_cast<std::size_t>(m_selectedIndex)]].c_str());
    } else {
        ImGui::TextDisabled("(no selection)");
    }

    if (!m_lastError.empty()) {
        ImGui::TextColored(ImVec4(0.92f, 0.30f, 0.30f, 1.0f), "Error: %s", m_lastError.c_str());
    }

    ImGui::Separator();
    const bool canAttach = (m_selectedIndex >= 0 && m_mgr != nullptr);
    if (!canAttach) ImGui::BeginDisabled();
    if (ImGui::Button("Attach", ImVec2(100.0f, 0.0f))) {
        TriggerAttach();
    }
    if (!canAttach) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f))) {
        Close();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        if (canAttach) TriggerAttach();
    }
}

void AttachBrowseModal::EnsureDatCatalog() {
    // Manager-change invalidation — mirror of Files tab tree-ptr pattern.
    if (m_cachedMgrPtrForDat != m_mgr) {
        if (m_catalog) m_catalog->Reset();
        if (m_iconCache) m_iconCache->Clear();   // Phase 5.1.5c — drop stale textures
        m_cachedMgrPtrForDat = m_mgr;
        m_datLoadState = DatLoadState::Idle;
    }

    // Phase 5.1.5c — lazy-create the icon cache on first DAT-tab visit
    // (or after a manager swap cleared it by re-construction). Initialize is
    // idempotent — re-running it is cheap and handles the case where the
    // cache was constructed before SetDevice arrived.
    if (!m_iconCache) {
        m_iconCache = std::make_unique<GGPKExp::Aoc::ItemIconCache>();
    }
    if (m_iconCache && m_device && m_mgr) {
        m_iconCache->Initialize(m_device, m_mgr);
    }

    if (m_datLoadState != DatLoadState::Idle) return;

    m_datLoadState = DatLoadState::Loading;   // first — so the Loading frame below renders

    if (!m_catalog) m_catalog = std::make_unique<GGPKExp::Aoc::ItemVisualIdentityCatalog>();

    const bool ok = m_catalog->Load(m_mgr, m_schema, nullptr);
    if (!ok) {
        m_datLoadError = m_catalog->GetLoadError();
        m_datLoadState = DatLoadState::Failed;
        return;
    }

    m_datLoadError.clear();
    m_datLoadState = DatLoadState::Ready;
    ApplyDatFilter();                         // seed m_datFilteredIndices with full catalog
}

void AttachBrowseModal::RenderDatTab() {
    // Preconditions — disable tab body with a message when we can't proceed.
    if (!m_schema) {
        ImGui::TextDisabled("DAT schema not available.");
        return;
    }
    if (!m_mgr) {
        ImGui::TextDisabled("GGPK not loaded.");
        return;
    }

    EnsureDatCatalog();

    if (m_datLoadState == DatLoadState::Loading) {
        ImGui::Text("Loading item catalog...");
        ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()),
                           ImVec2(-1.0f, 0.0f),
                           "BaseItemTypes / ItemVisualIdentity / ItemClasses");
        return;
    }

    if (m_datLoadState == DatLoadState::Failed) {
        ImGui::TextColored(ImVec4(0.92f, 0.30f, 0.30f, 1.0f),
                           "Load failed: %s", m_datLoadError.c_str());
        if (ImGui::Button("Retry")) {
            m_datLoadState = DatLoadState::Idle;
        }
        return;
    }

    RenderDatFilterRow();
    RenderDatResultsTable();
    // Early-return guard: if double-click in the results table triggered
    // TriggerDatAttach -> Close(), skip the selection panel + button row so
    // the Enter-key hotkey in RenderDatButtonsRow can't re-fire the attach
    // in the same frame. Mirrors the Files-tab pattern in RenderFilesTab.
    if (!m_showModal) return;
    RenderDatSelectionPanel();
    RenderDatButtonsRow();
}

void AttachBrowseModal::Render() {
    if (!m_showModal) return;

    const std::string title = "Browse and Attach##AocBrowse";
    ImGui::OpenPopup(title.c_str());
    ImGui::SetNextWindowSize(ImVec2(720.0f, 520.0f), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal(title.c_str(), &m_showModal, ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("Target socket: %s", m_targetSocket.c_str());
        ImGui::Separator();

        if (ImGui::BeginTabBar("##AocBrowseTabs", ImGuiTabBarFlags_None)) {
            if (ImGui::BeginTabItem("Files")) {
                if (m_activeTab != Tab::Files) m_activeTab = Tab::Files;
                RenderFilesTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("By Item Name")) {
                if (m_activeTab != Tab::ByItemName) {
                    m_activeTab = Tab::ByItemName;
                    m_datNeedsFocus = true;    // focus filter input on first frame of this tab
                }
                RenderDatTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::EndPopup();
    }
}

void AttachBrowseModal::RenderDatFilterRow() {
    if (m_datNeedsFocus) {
        ImGui::SetKeyboardFocusHere();
        m_datNeedsFocus = false;
    }
    if (ImGui::InputTextWithHint("##DatFilter", "Filter by name or class...",
                                 m_datFilterBuf, sizeof(m_datFilterBuf))) {
        ApplyDatFilter();
    }
    const std::size_t total = m_catalog ? m_catalog->Items().size() : 0;
    ImGui::Text("Results: %zu / %zu", m_datFilteredIndices.size(), total);
}

void AttachBrowseModal::RenderDatResultsTable() {
    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Sortable |
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_SortMulti;

    // Reserve space for the selection panel + buttons below (~120px).
    const float tableHeight   = ImGui::GetContentRegionAvail().y - 120.0f;
    const float clampedHeight = tableHeight > 200.0f ? tableHeight : 200.0f;

    const float rowHeight = kIconCellSize + 2.0f;

    if (ImGui::BeginTable("##DatResults", 4, flags, ImVec2(0.0f, clampedHeight))) {
        ImGui::TableSetupColumn("##Icon",
            ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
            kIconCellSize + kIconCellPadding);
        ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_DefaultSort, 0.55f);
        ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_None,        0.30f);
        ImGui::TableSetupColumn("Files", ImGuiTableColumnFlags_WidthFixed,  60.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        if (auto* specs = ImGui::TableGetSortSpecs()) {
            if (specs->SpecsDirty) {
                ApplyDatSort(specs);
                specs->SpecsDirty = false;
            }
        }

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_datFilteredIndices.size()), rowHeight);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                if (row < 0 || row >= static_cast<int>(m_datFilteredIndices.size())) continue;
                const std::size_t itemIdx = m_datFilteredIndices[static_cast<std::size_t>(row)];
                if (!m_catalog || itemIdx >= m_catalog->Items().size()) continue;
                const auto& item = m_catalog->Items()[itemIdx];

                ImGui::PushID(row);
                ImGui::TableNextRow(0, rowHeight);

                // Col 0 — Icon
                ImGui::TableNextColumn();
                if (!item.ddsPath.empty() && m_iconCache) {
                    ImTextureID tex = m_iconCache->Get(item.ddsPath);
                    if (tex != ImTextureID{}) {
                        ImGui::Image(tex, ImVec2(kIconCellSize, kIconCellSize));
                    }
                }

                // Col 1 — Name (click-area spans all columns via SpanAllColumns;
                // size.y matches the row height so the taller row's hit-rect
                // reaches to the bottom of the cell).
                ImGui::TableNextColumn();
                const bool selected = (row == m_datSelectedIndex);
                if (ImGui::Selectable(item.name.c_str(), selected,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick,
                                      ImVec2(0.0f, rowHeight))) {
                    OnDatRowClicked(row);
                    if (ImGui::IsMouseDoubleClicked(0)) TriggerDatAttach();
                }

                // Col 2 — Class
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(item.className.c_str());

                // Col 3 — Files count
                ImGui::TableNextColumn();
                ImGui::Text("%zu", item.files.size());

                ImGui::PopID();
            }
        }
        ImGui::EndTable();

        // Phase 5.1.5c — drain a bounded number of DDS loads per frame.
        if (m_iconCache) m_iconCache->PumpQueue(kDefaultLoadsPerFrame);
    }
}

void AttachBrowseModal::RenderDatSelectionPanel() {
    if (m_datSelectedIndex < 0 ||
        m_datSelectedIndex >= static_cast<int>(m_datFilteredIndices.size()) ||
        !m_catalog) {
        ImGui::TextDisabled("(no selection)");
        if (!m_datLastError.empty()) {
            ImGui::TextColored(ImVec4(0.92f, 0.30f, 0.30f, 1.0f),
                               "Error: %s", m_datLastError.c_str());
        }
        return;
    }

    const auto& item = m_catalog->Items()[m_datFilteredIndices[static_cast<std::size_t>(m_datSelectedIndex)]];
    ImGui::Text("Selected: %s  (%s)", item.name.c_str(), item.id.c_str());

    const char* preview = "(select)";
    if (m_datSelectedFileIdx >= 0 &&
        m_datSelectedFileIdx < static_cast<int>(item.files.size())) {
        preview = item.files[static_cast<std::size_t>(m_datSelectedFileIdx)].label.c_str();
    }
    if (ImGui::BeginCombo("File", preview)) {
        for (int i = 0; i < static_cast<int>(item.files.size()); ++i) {
            const bool sel = (i == m_datSelectedFileIdx);
            ImGui::PushID(i);
            if (ImGui::Selectable(item.files[static_cast<std::size_t>(i)].label.c_str(), sel)) {
                m_datSelectedFileIdx = i;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", item.files[static_cast<std::size_t>(i)].path.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    if (!m_datLastError.empty()) {
        ImGui::TextColored(ImVec4(0.92f, 0.30f, 0.30f, 1.0f),
                           "Error: %s", m_datLastError.c_str());
    }
}

void AttachBrowseModal::RenderDatButtonsRow() {
    ImGui::Separator();

    const bool canAttach =
        (m_datSelectedIndex >= 0) &&
        (m_datSelectedFileIdx >= 0) &&
        (m_mgr != nullptr);

    if (!canAttach) ImGui::BeginDisabled();
    if (ImGui::Button("Attach", ImVec2(100.0f, 0.0f))) TriggerDatAttach();
    if (!canAttach) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f))) Close();

    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        if (canAttach) TriggerDatAttach();
    }
}

void AttachBrowseModal::ApplyDatFilter() {
    m_datFilteredIndices.clear();
    m_datSelectedIndex   = -1;
    m_datSelectedFileIdx = -1;

    if (!m_catalog) return;

    // Lowercase filter text once per call.
    std::string filter;
    filter.reserve(std::strlen(m_datFilterBuf));
    for (const char* p = m_datFilterBuf; *p; ++p)
        filter += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));

    const auto& items = m_catalog->Items();
    m_datFilteredIndices.reserve(items.size());

    for (std::size_t i = 0; i < items.size(); ++i) {
        if (filter.empty() || items[i].searchBlob.find(filter) != std::string::npos) {
            m_datFilteredIndices.push_back(i);
        }
    }
}

void AttachBrowseModal::ApplyDatSort(ImGuiTableSortSpecs* specs) {
    if (!specs || specs->SpecsCount <= 0 || !m_catalog) return;
    const auto& items = m_catalog->Items();

    // Single-column sort (Name / Class / Files). SpecOrder 0 is primary.
    const ImGuiTableColumnSortSpecs& s = specs->Specs[0];
    const bool asc = (s.SortDirection == ImGuiSortDirection_Ascending);

    auto less = [&](std::size_t a, std::size_t b) -> bool {
        const auto& ia = items[a];
        const auto& ib = items[b];
        int cmp = 0;
        switch (s.ColumnIndex) {
        case 0: cmp = ia.name.compare(ib.name); break;
        case 1: cmp = ia.className.compare(ib.className); break;
        case 2: {
            const auto fa = ia.files.size();
            const auto fb = ib.files.size();
            cmp = (fa < fb) ? -1 : (fa > fb) ? 1 : 0;
            break;
        }
        default: break;
        }
        return asc ? (cmp < 0) : (cmp > 0);
    };

    std::sort(m_datFilteredIndices.begin(), m_datFilteredIndices.end(), less);

    // Selection index is an index into m_datFilteredIndices; after sort the
    // position changes. Reset to avoid stale highlight; user reselects.
    m_datSelectedIndex   = -1;
    m_datSelectedFileIdx = -1;
}

void AttachBrowseModal::OnDatRowClicked(int filteredRow) {
    if (filteredRow < 0 || filteredRow >= static_cast<int>(m_datFilteredIndices.size())) return;
    m_datSelectedIndex = filteredRow;

    if (!m_catalog) { m_datSelectedFileIdx = -1; return; }
    const auto& item = m_catalog->Items()[m_datFilteredIndices[static_cast<std::size_t>(filteredRow)]];
    m_datSelectedFileIdx = GGPKExp::Aoc::ItemVisualIdentityCatalog::PickDefaultFileIndex(item, m_parentAocPath);
    m_datLastError.clear();
}

void AttachBrowseModal::TriggerDatAttach() {
    m_datLastError.clear();
    if (!m_mgr || !m_catalog) { m_datLastError = "GGPK not loaded"; return; }
    if (m_datSelectedIndex < 0 ||
        m_datSelectedIndex >= static_cast<int>(m_datFilteredIndices.size())) {
        m_datLastError = "No item selected";
        return;
    }
    const auto& item = m_catalog->Items()[m_datFilteredIndices[static_cast<std::size_t>(m_datSelectedIndex)]];
    if (m_datSelectedFileIdx < 0 ||
        m_datSelectedFileIdx >= static_cast<int>(item.files.size())) {
        m_datLastError = "No file selected";
        return;
    }
    const std::string& path = item.files[static_cast<std::size_t>(m_datSelectedFileIdx)].path;

    void* node = m_mgr->FindNodeByPath(path);
    if (!node) { m_datLastError = "File not found: " + path; return; }
    auto res = m_mgr->ReadFile(node);
    if (!res.success) { m_datLastError = "Read failed: " + res.error; return; }

    if (m_onAttach) {
        m_onAttach(m_targetSocket, path, res.value);
    }
    Close();
}

} // namespace GGPKExp
