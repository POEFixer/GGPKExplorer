#include "BookmarkManager.h"
#include "UiText.h"
#include <algorithm>
#include <cctype>

namespace GGPKExp {

std::string BookmarkManager::Render(VirtualTree& tree) {
    std::string navTarget;

    if (!m_showBookmarkManager) return navTarget;

    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(I18n::TrId("bookmarks_title"), &m_showBookmarkManager)) {
        ImGui::End();
        return navTarget;
    }

    // Filter input
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##bmFilter", I18n::Tr("common_filter_by_path"), m_bookmarkFilter, sizeof(m_bookmarkFilter));

    ImGui::Spacing();

    const auto& bookmarks = tree.GetBookmarks();
    std::string filterStr(m_bookmarkFilter);
    for (auto& c : filterStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Table
    // Build sorted index for display
    struct BmView { int idx; };
    std::vector<BmView> filtered;
    filtered.reserve(bookmarks.size());
    for (int i = 0; i < static_cast<int>(bookmarks.size()); ++i) {
        if (!filterStr.empty()) {
            std::string lowerPath = bookmarks[i].path;
            for (auto& c : lowerPath) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lowerPath.find(filterStr) == std::string::npos)
                continue;
        }
        filtered.push_back({ i });
    }

    const char* goLabel = I18n::TrId("bookmarks_go");
    const char* removeLabel = I18n::TrId("bookmarks_remove");

    if (ImGui::BeginTable("##bmTable", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable,
            ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 4))) {
        // Fixed columns fit their translated header and cells (width 0: the table is not resizable).
        ImGui::TableSetupColumn(I18n::TrId("bookmarks_col_icon"), ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 0.0f, 0);
        ImGui::TableSetupColumn(I18n::TrId("bookmarks_col_path"), ImGuiTableColumnFlags_WidthStretch, 0.0f, 1);
        ImGui::TableSetupColumn(I18n::TrId("bookmarks_col_actions"), ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 0.0f, 2);
        ImGui::TableHeadersRow();

        // Apply sorting
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
            if (sortSpecs->SpecsCount > 0) {
                const auto& spec = sortSpecs->Specs[0];
                bool ascending = (spec.SortDirection == ImGuiSortDirection_Ascending);
                if (spec.ColumnUserID == 0) {
                    // Sort by icon index
                    std::sort(filtered.begin(), filtered.end(), [&](const BmView& a, const BmView& b) {
                        int va = bookmarks[a.idx].iconIndex, vb = bookmarks[b.idx].iconIndex;
                        return ascending ? (va < vb) : (va > vb);
                    });
                } else if (spec.ColumnUserID == 1) {
                    // Sort by path
                    std::sort(filtered.begin(), filtered.end(), [&](const BmView& a, const BmView& b) {
                        int cmp = _stricmp(bookmarks[a.idx].path.c_str(), bookmarks[b.idx].path.c_str());
                        return ascending ? (cmp < 0) : (cmp > 0);
                    });
                }
            }
        }

        static const char* bmIcons[] = {
            "\xef\x80\x85", "\xef\x80\x84", "\xef\x80\xa4", "\xef\x83\xa7", "\xef\x81\xad",
            "\xef\x8e\xa5", "\xef\x84\x91", "\xef\x80\xae", "\xef\x83\xb3", "\xef\x81\xae"
        };

        std::string pathToRemove;
        for (const auto& v : filtered) {
            const auto& bm = bookmarks[v.idx];
            ImGui::TableNextRow();
            ImGui::PushID(bm.path.c_str());

            // Icon column -- centered
            ImGui::TableNextColumn();
            if (bm.iconIndex >= 0 && bm.iconIndex < 10) {
                float colW = ImGui::GetColumnWidth();
                float textW = ImGui::CalcTextSize(bmIcons[bm.iconIndex]).x;
                float rowH = ImGui::GetTextLineHeightWithSpacing();
                float textH = ImGui::GetTextLineHeight();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (colW - textW) * 0.5f);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - textH) * 0.5f);
                ImGui::Text("%s", bmIcons[bm.iconIndex]);
            }

            // Path column
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(bm.path.c_str());

            // Actions column -- centered
            ImGui::TableNextColumn();
            {
                float colW = ImGui::GetColumnWidth();
                float btnW = ImGui::CalcTextSize(goLabel, nullptr, true).x +
                             ImGui::CalcTextSize(removeLabel, nullptr, true).x +
                             ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x;
                float rowH = ImGui::GetTextLineHeightWithSpacing();
                float btnH = ImGui::GetFrameHeight();
                if (colW > btnW)
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (colW - btnW) * 0.5f);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - btnH) * 0.5f);
                if (ImGui::SmallButton(goLabel)) {
                    navTarget = bm.path;
                    m_showBookmarkManager = false;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(removeLabel)) {
                    pathToRemove = bm.path;
                }
            }

            ImGui::PopID();
        }

        ImGui::EndTable();

        if (!pathToRemove.empty()) {
            tree.RemoveBookmark(pathToRemove);
        }
    }

    // Bottom bar
    ImGui::Separator();
    if (bookmarks.empty()) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(I18n::TrId("bookmarks_clear_all"))) {
        m_bookmarkClearConfirm = true;
    }
    if (bookmarks.empty()) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::TextDisabled("%s", I18n::TrF("bookmarks_count", { { "count", Num("%d", static_cast<int>(bookmarks.size())) } }).c_str());

    if (m_bookmarkClearConfirm) {
        ImGui::OpenPopup("##ClearAllBookmarks");
        m_bookmarkClearConfirm = false;
    }
    if (ImGui::BeginPopupModal("##ClearAllBookmarks", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(I18n::TrF("bookmarks_clear_confirm",
            { { "count", Num("%d", static_cast<int>(bookmarks.size())) } }).c_str());
        ImGui::Spacing();
        const char* yesLabel = I18n::TrId("common_yes");
        const char* cancelLabel = I18n::TrId("common_cancel");
        float confirmW = ButtonWidth(yesLabel, 6.0f);
        if (ButtonWidth(cancelLabel, 6.0f) > confirmW) confirmW = ButtonWidth(cancelLabel, 6.0f);
        if (ImGui::Button(yesLabel, ImVec2(confirmW, 0))) {
            tree.SetBookmarks({});
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(cancelLabel, ImVec2(confirmW, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();

    return navTarget;
}

} // namespace GGPKExp
