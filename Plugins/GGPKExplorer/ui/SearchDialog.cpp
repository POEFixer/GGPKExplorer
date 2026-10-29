// Plugins/GGPKExplorer/ui/SearchDialog.cpp
#include "SearchDialog.h"
#include "../core/TextEncoding.h"
#include <imgui.h>
#include <cstring>
#include <algorithm>
#include <map>
#include <sstream>
#include <cstdio>
#include <cmath>

namespace GGPKExp {

constexpr SearchDialog::FilterPreset SearchDialog::FILTER_PRESETS[];

void SearchDialog::Open(const std::vector<std::string>& scopePaths, bool openFindReplaceTab) {
    m_isOpen = true;
    m_scopePaths = scopePaths;
    m_activeTab = openFindReplaceTab ? 1 : 0;
    m_showPreview = false;

    // Initialize custom filter from default preset
    if (m_customFilterBuf[0] == '\0') {
        strncpy(m_customFilterBuf, FILTER_PRESETS[1].value, sizeof(m_customFilterBuf) - 1);
    }
    if (m_findFileFilterBuf[0] == '\0') {
        strncpy(m_findFileFilterBuf, "*.*", sizeof(m_findFileFilterBuf) - 1);
    }
    m_isScopedSearch = false;

    m_userResizedHeight = false;
    m_firstRender = true;
    m_measuredContentHeight = 520.0f;
}

void SearchDialog::Open(int tab, const std::string& scopePath, bool isFileScope) {
    m_isOpen = true;
    m_scopePaths = { scopePath };
    m_activeTab = tab;
    m_showPreview = false;
    m_isScopedSearch = true;
    (void)isFileScope;   // the scope row shows the full path (m_scopePaths)

    // Reset result delivery flags
    m_ffResultDelivered = false;
    m_frResultDelivered = false;

    m_userResizedHeight = false;
    m_firstRender = true;
    m_measuredContentHeight = 520.0f;
}

void SearchDialog::Close() {
    m_isOpen = false;
    m_showPreview = false;
}

void SearchDialog::AddToHistory(const std::string& query) {
    AddToHistoryList(m_searchHistory, query);
}

std::string SearchDialog::GetCurrentFilter() const {
    if (m_filterPresetIndex >= 0 && m_filterPresetIndex < PRESET_COUNT - 1)
        return FILTER_PRESETS[m_filterPresetIndex].value;
    return m_customFilterBuf;
}

bool SearchDialog::AreAllScopePathsFiles() const {
    if (!m_tree || m_scopePaths.empty()) return false;
    const auto& flatList = m_tree->GetFlatList();
    for (const auto& p : m_scopePaths) {
        uint32_t idx = m_tree->FindByPath(p);
        if (idx == UINT32_MAX) return false;
        if (idx >= flatList.size()) return false;
        if (flatList[idx].isDirectory) return false;
    }
    return true;
}

SearchOptions SearchDialog::BuildFindFileOptions() const {
    SearchOptions opts;
    opts.query = m_findFileBuf;
    opts.matchCase = m_ffMatchCase;
    opts.matchWholeWord = m_ffMatchWholeWord;
    opts.inSubFolders = m_ffInSubFolders;
    opts.mode = static_cast<SearchMode>(m_ffSearchMode);
    opts.filters = m_findFileFilterBuf;
    opts.scopePaths = m_scopePaths;
    return opts;
}

SearchOptions SearchDialog::BuildContentOptions() const {
    SearchOptions opts;
    opts.query = m_findContentBuf;
    opts.replaceWith = m_replaceBuf;
    opts.matchCase = m_frMatchCase;
    opts.matchWholeWord = m_frMatchWholeWord;
    opts.inSubFolders = m_frInSubFolders;
    opts.mode = static_cast<SearchMode>(m_frSearchMode);
    opts.dotMatchesNewline = m_frDotMatchesNewline;
    opts.filters = GetCurrentFilter();
    opts.scopePaths = m_scopePaths;
    return opts;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void SearchDialog::DoFindFiles() {
    if (!m_fileSearcher || !m_tree) return;
    auto opts = BuildFindFileOptions();
    if (opts.query.empty()) return;
    AddToHistory(opts.query);
    m_ffResultDelivered = false;
    m_fileSearcher->Start(opts, m_tree, m_searchFileNames, m_searchFolderNames, m_searchByPath);
}

void SearchDialog::DoFindInFiles() {
    if (!m_contentSearcher || !m_tree || !m_mgr) return;
    auto opts = BuildContentOptions();
    if (opts.query.empty()) return;
    AddToHistory(opts.query);
    m_frResultDelivered = false;
    m_contentSearcher->StartFind(opts, m_tree, m_mgr, m_datSearcher, m_schema);
}

void SearchDialog::DoReplaceInFiles() {
    if (!m_contentSearcher || !m_tree || !m_mgr) return;
    auto opts = BuildContentOptions();
    if (opts.query.empty()) return;
    AddToHistory(opts.query);
    AddToHistoryList(m_replaceHistory, opts.replaceWith);

    std::vector<ReplacementPreview> datPreviews;
    auto results = m_contentSearcher->ReplaceInFiles(opts, m_tree, m_mgr, m_datSearcher, m_schema, datPreviews);

    // If there are datc64 files, show preview for those
    if (!datPreviews.empty()) {
        m_previewData = std::move(datPreviews);
        m_showPreview = true;
    }

    // Report text file replacements immediately
    if (!results.empty() && m_onReplaceLog)
        m_onReplaceLog(results);
}

void SearchDialog::DoApplyPreview() {
    if (!m_mgr || !m_tree) return;

    std::vector<ReplaceResult> allResults;
    const auto& flatList = m_tree->GetFlatList();

    for (const auto& preview : m_previewData) {
        // Find nodePtr by path
        uint32_t idx = m_tree->FindByPath(preview.fullPath);
        if (idx == UINT32_MAX) continue;
        const auto& entry = flatList[idx];
        if (!entry.nodePtr) continue;

        auto readRes = m_mgr->ReadFile(entry.nodePtr);
        if (!readRes || readRes.value.empty()) continue;
        auto& data = readRes.value;

        if (preview.isDatc64 && m_datSearcher && m_schema) {
            // Extract table name from path
            auto slash = preview.fullPath.rfind('/');
            std::string filename = (slash != std::string::npos)
                ? preview.fullPath.substr(slash + 1) : preview.fullPath;
            auto dot = filename.rfind('.');
            std::string tableName = (dot != std::string::npos) ? filename.substr(0, dot) : filename;

            auto newData = m_datSearcher->ApplyReplacements(data, preview, m_schema, tableName);
            if (!newData.empty() && newData != data) {
                m_mgr->WriteFile(entry.nodePtr, newData);
                allResults.push_back({preview.fullPath, static_cast<int>(preview.entries.size())});
            }
        } else {
            // Text file: decode to UTF-8, apply replacements, re-encode
            auto encoding = DetectEncoding(data.data(), data.size());
            bool hasBom = false;
            if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
                hasBom = true;
            else if (data.size() >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF)))
                hasBom = true;

            std::string text = DecodeToUtf8(data.data(), data.size(), encoding);
            int count = 0;

            std::string replaced;
            std::istringstream stream(text);
            std::string line;
            int lineNum = 1;
            std::map<int, std::string> lineReplacements;
            for (const auto& e : preview.entries)
                lineReplacements[e.line] = e.after;

            while (std::getline(stream, line)) {
                auto it = lineReplacements.find(lineNum);
                if (it != lineReplacements.end()) {
                    replaced += it->second;
                    count++;
                } else {
                    replaced += line;
                }
                replaced += '\n';
                lineNum++;
            }
            if (!text.empty() && text.back() != '\n' && !replaced.empty() && replaced.back() == '\n')
                replaced.pop_back();

            if (count > 0) {
                auto newData = EncodeFromUtf8(replaced, encoding, hasBom);
                m_mgr->WriteFile(entry.nodePtr, newData);
                allResults.push_back({preview.fullPath, count});
            }
        }
    }

    m_showPreview = false;
    m_previewData.clear();

    if (!allResults.empty() && m_onReplaceLog)
        m_onReplaceLog(allResults);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void SearchDialog::Render() {
    if (!m_isOpen) return;

    bool progressVisibleNow = (m_fileSearcher && m_fileSearcher->IsRunning())
                            || (m_contentSearcher && m_contentSearcher->IsRunning());

    if (m_firstRender) {
        ImGui::SetNextWindowSize(ImVec2(620.0f, 520.0f), ImGuiCond_Appearing);
        m_firstRender = false;
    } else if (!m_userResizedHeight) {
        // Auto-size to measured content height from previous frame
        ImGui::SetNextWindowSize(ImVec2(m_lastWindowWidth, m_measuredContentHeight), ImGuiCond_Always);
        m_lastRenderedHeight = m_measuredContentHeight;
    }

    const std::string windowTitle = LabelId(I18n::Tr("search_title"), "SearchDialog");
    if (!ImGui::Begin(windowTitle.c_str(), &m_isOpen)) {
        m_progressVisibleLastFrame = progressVisibleNow;
        ImGui::End();
        return;
    }

    // Tab bar
    if (ImGui::BeginTabBar("##SearchTabs")) {
        ImGuiTabItemFlags findFileFlags = (m_activeTab == 0) ? ImGuiTabItemFlags_SetSelected : 0;
        ImGuiTabItemFlags findReplaceFlags = (m_activeTab == 1) ? ImGuiTabItemFlags_SetSelected : 0;
        // Reset after first frame
        if (m_activeTab >= 0) m_activeTab = -1;

        if (ImGui::BeginTabItem(I18n::TrId("search_tab_find_file"), nullptr, findFileFlags)) {
            RenderFindFileTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(I18n::TrId("search_tab_find_replace"), nullptr, findReplaceFlags)) {
            RenderFindReplaceTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // Measure content height for auto-resize on next frame
    float contentBottomY = ImGui::GetCursorPosY();
    float desiredH = contentBottomY
                   + ImGui::GetStyle().WindowPadding.y
                   + ImGui::GetFrameHeightWithSpacing()  // title bar
                   + 4.0f;
    if (desiredH > 100.0f && desiredH < 1200.0f) {
        m_measuredContentHeight = desiredH;
    }

    // Track actual window size — detect user-driven vertical resize
    ImVec2 actualSize = ImGui::GetWindowSize();
    if (std::fabs(actualSize.x - m_lastWindowWidth) > 0.5f) {
        m_lastWindowWidth = actualSize.x;
    }
    float expectedH = m_measuredContentHeight;
    if (std::fabs(actualSize.y - expectedH) > 3.0f
        && std::fabs(actualSize.y - m_lastRenderedHeight) > 3.0f) {
        m_userResizedHeight = true;
    }
    m_lastRenderedHeight = actualSize.y;
    m_progressVisibleLastFrame = progressVisibleNow;

    ImGui::End();

    // Replace preview modal
    if (m_showPreview)
        RenderReplacePreview();
}

// ---------------------------------------------------------------------------
// RenderFindFileTab
// ---------------------------------------------------------------------------

// The width of the widest of `labels` plus one em: the column the input fields start at.
static float LabelColumn(std::initializer_list<const char*> labels) {
    float widest = 0.0f;
    for (const char* label : labels) {
        const float w = ImGui::CalcTextSize(label, nullptr, true).x;
        if (w > widest) widest = w;
    }
    return widest + ImGui::GetFontSize();
}

// The read-only scope line: the selected paths, or the whole tree.
static std::string ScopeText(const std::vector<std::string>& scopePaths) {
    std::string scopeStr;
    for (size_t i = 0; i < scopePaths.size(); ++i) {
        if (i > 0) scopeStr += ", ";
        scopeStr += scopePaths[i];
    }
    if (scopeStr.empty()) scopeStr = I18n::Tr("search_scope_entire_tree");
    return scopeStr;
}

// The "Extended" search mode label: its escape list is code, never translated.
static std::string ExtendedModeLabel(const char* id) {
    return LabelId((std::string(I18n::Tr("search_mode_extended")) + " (\\n, \\r, \\t, \\0, \\x...)").c_str(), id);
}

void SearchDialog::RenderFindFileTab() {
    const char* findWhat = I18n::Tr("search_find_what");
    const char* filters = I18n::Tr("search_filters");
    const char* scope = I18n::Tr("search_scope");
    const std::string findAll = LabelId(I18n::Tr("search_find_all"), "ff_find_all");
    float labelW = LabelColumn({ findWhat, filters, scope });
    float availW = ImGui::GetContentRegionAvail().x;
    float btnW = ButtonWidth(findAll.c_str(), 6.0f);
    float inputW = availW - labelW - btnW - 16.0f;
    if (inputW < 100.0f) inputW = 100.0f;

    // Find what
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(findWhat);
    ImGui::SameLine(labelW);
    bool enterPressed = RenderHistoryInput("##FFQuery", m_findFileBuf, sizeof(m_findFileBuf),
                                            m_searchHistory, inputW);
    ImGui::SameLine();
    if (ImGui::Button(findAll.c_str(), ImVec2(btnW, 0)) || enterPressed)
        DoFindFiles();

    // Filters
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(filters);
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(inputW);
    ImGui::InputText("##FFFilter", m_findFileFilterBuf, sizeof(m_findFileFilterBuf));

    // Scope (read-only)
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(scope);
    ImGui::SameLine(labelW);
    ImGui::TextColored(ImVec4(0.53f, 0.67f, 1.0f, 1.0f), "%s", ScopeText(m_scopePaths).c_str());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Checkboxes in two columns
    ImGui::Columns(2, nullptr, false);

    ImGui::Checkbox(I18n::TrId("search_file_names"), &m_searchFileNames);
    ImGui::Checkbox(I18n::TrId("search_folder_names"), &m_searchFolderNames);
    ImGui::Checkbox(I18n::TrId("search_by_path"), &m_searchByPath);
    ImGui::Checkbox(LabelId(I18n::Tr("search_match_whole_word"), "ff_match_whole_word").c_str(), &m_ffMatchWholeWord);
    ImGui::Checkbox(LabelId(I18n::Tr("search_match_case"), "ff_match_case").c_str(), &m_ffMatchCase);
    {
        bool allFiles = AreAllScopePathsFiles();
        ImGui::BeginDisabled(allFiles);
        ImGui::Checkbox(LabelId(I18n::Tr("search_in_subfolders"), "ff_in_subfolders").c_str(), &m_ffInSubFolders);
        bool hovered = ImGui::IsItemHovered();
        ImGui::EndDisabled();
        if (allFiles && hovered)
            ImGui::SetTooltip("%s", I18n::Tr("search_subfolders_not_applicable"));
    }

    ImGui::NextColumn();

    ImGui::TextUnformatted(I18n::Tr("search_mode"));
    ImGui::RadioButton(LabelId(I18n::Tr("search_mode_normal"), "ff_mode_normal").c_str(), &m_ffSearchMode, 0);
    ImGui::RadioButton(ExtendedModeLabel("ff_mode_extended").c_str(), &m_ffSearchMode, 1);
    ImGui::RadioButton(LabelId(I18n::Tr("search_mode_regex"), "ff_mode_regex").c_str(), &m_ffSearchMode, 2);

    ImGui::Columns(1);

    // Progress and result delivery
    ImGui::Spacing();
    if (m_fileSearcher && m_fileSearcher->IsRunning()) {
        float progress = m_fileSearcher->GetProgress();
        ImGui::ProgressBar(progress, ImVec2(-1, 0), I18n::Tr("search_searching"));
    } else if (m_fileSearcher && !m_ffResultDelivered) {
        // Search finished — deliver result exactly once
        auto result = m_fileSearcher->GetResult();
        if (result.completed && m_onResults) {
            m_ffResultDelivered = true;
            m_onResults(result);
        } else if (result.completed) {
            m_ffResultDelivered = true;
        }
    }
}

// ---------------------------------------------------------------------------
// RenderFindReplaceTab
// ---------------------------------------------------------------------------

void SearchDialog::RenderFindReplaceTab() {
    const char* findWhat = I18n::Tr("search_find_what");
    const char* replaceWith = I18n::Tr("search_replace_with");
    const char* filters = I18n::Tr("search_filters");
    const char* scope = I18n::Tr("search_scope");
    const std::string findAll = LabelId(I18n::Tr("search_find_all"), "fr_find_all");
    const char* findNext = I18n::TrId("search_find_next");
    const char* replaceInFiles = I18n::TrId("search_replace_in_files");
    float labelW = LabelColumn({ findWhat, replaceWith, filters, scope });
    float availW = ImGui::GetContentRegionAvail().x;
    float btnW = ButtonWidth(findAll.c_str(), 10.0f);
    if (ButtonWidth(findNext, 10.0f) > btnW) btnW = ButtonWidth(findNext, 10.0f);
    if (ButtonWidth(replaceInFiles, 10.0f) > btnW) btnW = ButtonWidth(replaceInFiles, 10.0f);
    float inputW = availW - labelW - btnW - 16.0f;
    if (inputW < 100.0f) inputW = 100.0f;

    float btnX = labelW + inputW + ImGui::GetStyle().ItemSpacing.x;

    // Find what
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(findWhat);
    ImGui::SameLine(labelW);
    bool enterPressed = RenderHistoryInput("##FRQuery", m_findContentBuf, sizeof(m_findContentBuf),
                                            m_searchHistory, inputW);
    ImGui::SameLine();
    if (ImGui::Button(findAll.c_str(), ImVec2(btnW, 0)) || enterPressed)
        DoFindInFiles();

    // Replace with (directly under Find what)
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(replaceWith);
    ImGui::SameLine(labelW);
    RenderHistoryInput("##FRReplace", m_replaceBuf, sizeof(m_replaceBuf),
                        m_replaceHistory, inputW);
    ImGui::SameLine();
    if (ImGui::Button(findNext, ImVec2(btnW, 0)) && m_findContentBuf[0] != '\0') {
        DoFindNext();
    }

    // Filters (with Replace in Files button on the right)
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(filters);
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(inputW);
    if (m_filterPresetIndex < PRESET_COUNT - 1) {
        // Show preset name in combo
        if (ImGui::BeginCombo("##FRFilter", I18n::Tr(FILTER_PRESETS[m_filterPresetIndex].nameKey))) {
            for (int i = 0; i < PRESET_COUNT; ++i) {
                bool isSelected = (i == m_filterPresetIndex);
                if (ImGui::Selectable(LabelId(I18n::Tr(FILTER_PRESETS[i].nameKey), FILTER_PRESETS[i].nameKey).c_str(),
                                      isSelected)) {
                    m_filterPresetIndex = i;
                    if (i == PRESET_COUNT - 1) {
                        // Custom — copy current preset value into custom buffer
                        strncpy(m_customFilterBuf,
                                (m_filterPresetIndex > 0 && m_filterPresetIndex < PRESET_COUNT - 1)
                                    ? FILTER_PRESETS[m_filterPresetIndex].value : "*.*",
                                sizeof(m_customFilterBuf) - 1);
                    }
                }
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    } else {
        // Custom mode — editable text field
        ImGui::InputText("##FRCustomFilter", m_customFilterBuf, sizeof(m_customFilterBuf));
        ImGui::SameLine();
        if (ImGui::SmallButton(I18n::TrId("search_presets"))) {
            m_filterPresetIndex = 1; // back to default
        }
    }
    ImGui::SameLine(btnX);
    if (ImGui::Button(replaceInFiles, ImVec2(btnW, 0)))
        DoReplaceInFiles();

    // Scope
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(scope);
    ImGui::SameLine(labelW);
    ImGui::TextColored(ImVec4(0.53f, 0.67f, 1.0f, 1.0f), "%s", ScopeText(m_scopePaths).c_str());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Checkboxes
    ImGui::Columns(2, nullptr, false);

    ImGui::Checkbox(LabelId(I18n::Tr("search_match_whole_word"), "fr_match_whole_word").c_str(), &m_frMatchWholeWord);
    ImGui::Checkbox(LabelId(I18n::Tr("search_match_case"), "fr_match_case").c_str(), &m_frMatchCase);
    {
        bool allFiles = AreAllScopePathsFiles();
        ImGui::BeginDisabled(allFiles);
        ImGui::Checkbox(LabelId(I18n::Tr("search_in_subfolders"), "fr_in_subfolders").c_str(), &m_frInSubFolders);
        bool hovered = ImGui::IsItemHovered();
        ImGui::EndDisabled();
        if (allFiles && hovered)
            ImGui::SetTooltip("%s", I18n::Tr("search_subfolders_not_applicable"));
    }

    ImGui::NextColumn();

    ImGui::TextUnformatted(I18n::Tr("search_mode"));
    ImGui::RadioButton(LabelId(I18n::Tr("search_mode_normal"), "fr_mode_normal").c_str(), &m_frSearchMode, 0);
    ImGui::RadioButton(ExtendedModeLabel("fr_mode_extended").c_str(), &m_frSearchMode, 1);
    ImGui::RadioButton(LabelId(I18n::Tr("search_mode_regex"), "fr_mode_regex").c_str(), &m_frSearchMode, 2);
    if (m_frSearchMode == 2) {
        ImGui::SameLine();
        ImGui::Checkbox(I18n::TrId("search_dot_matches_newline"), &m_frDotMatchesNewline);
    }

    ImGui::Columns(1);

    // Info bar
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 0.8f), "%s",
        (std::string("\xef\x81\x9a ") + I18n::Tr("search_datc64_info")).c_str());
    if (!m_schema || !m_schema->IsLoaded()) {
        // .datc64 values decode through the DAT schema (game data the host may still be downloading).
        ImGui::SameLine(0.0f, ImGui::GetFontSize());
        SchemaNotReadyLine(m_schema ? m_schema->GetError() : std::string());
    }

    // Progress and result delivery
    ImGui::Spacing();
    if (m_contentSearcher && m_contentSearcher->IsRunning()) {
        float progress = m_contentSearcher->GetProgress();
        ImGui::ProgressBar(progress, ImVec2(-1, 0), I18n::Tr("search_searching"));
    } else if (m_contentSearcher && !m_frResultDelivered) {
        // Search finished — deliver result exactly once
        auto result = m_contentSearcher->GetResult();
        if (result.completed && m_onResults) {
            m_frResultDelivered = true;
            m_onResults(result);
        }
    }
}

// ---------------------------------------------------------------------------
// RenderReplacePreview
// ---------------------------------------------------------------------------

void SearchDialog::RenderReplacePreview() {
    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
    const std::string windowTitle = LabelId(I18n::Tr("preview_title"), "ReplacePreview");
    if (!ImGui::Begin(windowTitle.c_str(), &m_showPreview)) {
        ImGui::End();
        return;
    }

    int totalEntries = 0;
    for (const auto& p : m_previewData)
        totalEntries += static_cast<int>(p.entries.size());

    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s",
        I18n::TrF("preview_summary", { { "replacements", Num("%d", totalEntries) },
                                       { "files", Num("%d", static_cast<int>(m_previewData.size())) } }).c_str());
    ImGui::Separator();

    // Scrollable area
    ImGui::BeginChild("##PreviewList", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 4));

    for (const auto& preview : m_previewData) {
        // File header
        const char* icon = preview.isDatc64 ? "\xef\x83\x8e " : "\xef\x85\x9c ";
        ImGui::TextColored(ImVec4(0.3f, 0.6f, 1.0f, 1.0f), "%s%s", icon, preview.fullPath.c_str());

        // Table for entries
        if (ImGui::BeginTable("##PreviewEntries", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(I18n::TrId("preview_col_row"), ImGuiTableColumnFlags_WidthFixed, 0.0f);
            ImGui::TableSetupColumn(I18n::TrId("preview_col_column"), ImGuiTableColumnFlags_WidthFixed, 0.0f);
            ImGui::TableSetupColumn(I18n::TrId("preview_col_before"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(I18n::TrId("preview_col_after"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (const auto& e : preview.entries) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d", e.line);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e.column.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", e.before.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", e.after.c_str());
            }

            ImGui::EndTable();
        }
        ImGui::Spacing();
    }

    ImGui::EndChild();

    // Buttons
    const char* cancelLabel = I18n::TrId("common_cancel");
    const std::string applyLabel = LabelId(
        I18n::TrF("preview_apply", { { "count", Num("%d", totalEntries) } }).c_str(), "preview_apply");
    float btnWidth = ButtonWidth(cancelLabel, 11.5f);
    if (ButtonWidth(applyLabel.c_str(), 11.5f) > btnWidth) btnWidth = ButtonWidth(applyLabel.c_str(), 11.5f);
    float spacing = 8.0f;
    float totalBtnW = btnWidth * 2 + spacing;
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - totalBtnW + ImGui::GetCursorPosX());

    if (ImGui::Button(cancelLabel, ImVec2(btnWidth, 0))) {
        m_showPreview = false;
        m_previewData.clear();
    }
    ImGui::SameLine(0, spacing);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.5f, 0.1f, 1.0f));
    if (ImGui::Button(applyLabel.c_str(), ImVec2(btnWidth, 0))) {
        DoApplyPreview();
    }
    ImGui::PopStyleColor();

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Find Next — incremental search
// ---------------------------------------------------------------------------

void SearchDialog::DoFindNext() {
    std::string query(m_findContentBuf);
    if (query.empty()) return;

    bool caseSensitive = m_frMatchCase;

    // Reset state if query or scope changed
    if (m_findNext.query != query || m_findNext.caseSensitive != caseSensitive ||
        m_findNext.scopePath != (m_scopePaths.empty() ? "" : m_scopePaths[0]) ||
        !m_findNext.initialized) {
        m_findNext = {};
        m_findNext.query = query;
        m_findNext.caseSensitive = caseSensitive;
        m_findNext.scopePath = m_scopePaths.empty() ? "" : m_scopePaths[0];
        m_findNext.isFileScope = m_isScopedSearch &&
            !m_findNext.scopePath.empty() &&
            m_findNext.scopePath.back() != '/';

        if (m_findNext.isFileScope) {
            m_findNext.currentFile = m_findNext.scopePath;
        } else {
            CollectTextFiles(m_findNext.scopePath, m_findNext.fileList);
            if (m_findNext.fileList.empty()) return;
            m_findNext.currentFile = m_findNext.fileList[0];
            m_findNext.currentFileIndex = 0;
        }
        m_findNext.currentLine = 0;
        m_findNext.currentCol = 0;
        m_findNext.initialized = true;
    }

    // Search from current position (currentLine/currentCol are 0-based)
    int foundLine = -1, foundCol = -1;
    if (FindNextInFile(m_findNext.currentFile, m_findNext.currentLine, m_findNext.currentCol,
                       foundLine, foundCol)) {
        // foundLine is 1-based (for ScrollToLine), store 0-based for next search
        m_findNext.currentLine = foundLine - 1;
        m_findNext.currentCol = foundCol + static_cast<int>(m_findNext.query.size());
        if (m_onNavigateToLine)
            m_onNavigateToLine(m_findNext.currentFile, foundLine);
        return;
    }

    // Not found in current file
    if (m_findNext.isFileScope) {
        // Wrap around
        m_findNext.currentLine = 0;
        m_findNext.currentCol = 0;
        if (FindNextInFile(m_findNext.currentFile, 0, 0, foundLine, foundCol)) {
            m_findNext.currentLine = foundLine - 1;
            m_findNext.currentCol = foundCol + static_cast<int>(m_findNext.query.size());
            if (m_onNavigateToLine)
                m_onNavigateToLine(m_findNext.currentFile, foundLine);
        }
    } else {
        // Try next files
        int startIdx = m_findNext.currentFileIndex + 1;
        int totalFiles = static_cast<int>(m_findNext.fileList.size());
        for (int attempt = 0; attempt < totalFiles; ++attempt) {
            int idx = (startIdx + attempt) % totalFiles;
            const auto& file = m_findNext.fileList[idx];
            if (FindNextInFile(file, 0, 0, foundLine, foundCol)) {
                m_findNext.currentFileIndex = idx;
                m_findNext.currentFile = file;
                m_findNext.currentLine = foundLine - 1;
                m_findNext.currentCol = foundCol + static_cast<int>(m_findNext.query.size());
                if (m_onNavigateToLine)
                    m_onNavigateToLine(file, foundLine);
                return;
            }
        }
    }
}

bool SearchDialog::FindNextInFile(const std::string& filePath, int startLine, int startCol,
                                   int& outLine, int& outCol) {
    if (!m_mgr || filePath.empty()) return false;

    uint32_t idx = m_tree ? m_tree->FindByPath(filePath) : UINT32_MAX;
    if (idx == UINT32_MAX) return false;

    const auto& flatList = m_tree->GetFlatList();
    if (idx >= flatList.size() || flatList[idx].isDirectory) return false;

    auto readRes = m_mgr->ReadFile(flatList[idx].nodePtr);
    if (!readRes || readRes.value.empty()) return false;
    auto& data = readRes.value;

    std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    std::string query = m_findNext.query;

    if (!m_findNext.caseSensitive) {
        std::transform(text.begin(), text.end(), text.begin(), ::tolower);
        std::transform(query.begin(), query.end(), query.begin(), ::tolower);
    }

    // Convert (startLine, startCol) to byte offset — both are 0-based
    size_t byteOffset = 0;
    {
        int line = 0;
        for (size_t i = 0; i < text.size() && line < startLine; ++i) {
            if (text[i] == '\n') ++line;
        }
        // Count bytes for startLine lines
        int curLine = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            if (curLine == startLine) {
                byteOffset = i + static_cast<size_t>(startCol);
                break;
            }
            if (text[i] == '\n') ++curLine;
        }
        if (curLine < startLine) return false; // startLine beyond file
    }

    if (byteOffset > text.size()) byteOffset = text.size();

    // Single find from byte offset — O(n)
    size_t found = text.find(query, byteOffset);
    if (found == std::string::npos) return false;

    // Convert byte position back to 1-based line + 0-based column
    int line = 0;
    size_t lineStart = 0;
    for (size_t i = 0; i < found; ++i) {
        if (text[i] == '\n') {
            ++line;
            lineStart = i + 1;
        }
    }
    outLine = line + 1; // 1-based for ScrollToLine
    outCol = static_cast<int>(found - lineStart);
    return true;
}

void SearchDialog::CollectTextFiles(const std::string& folderPath,
                                     std::vector<std::string>& out) {
    if (!m_tree) return;
    m_tree->ForEachFileUnder(folderPath,
        [&out](const std::string& fullPath, const std::string& name,
               int64_t /*fileSize*/, void* /*nodePtr*/) {
            // Filter to text-like files
            auto dot = name.rfind('.');
            if (dot == std::string::npos) return;
            std::string ext = name.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".txt" || ext == ".json" || ext == ".xml" || ext == ".cfg" ||
                ext == ".csv" || ext == ".ini" || ext == ".csd" || ext == ".lua" ||
                ext == ".shader" || ext == ".hlsl" || ext == ".glsl" || ext == ".fx" ||
                ext == ".filter" || ext == ".py" || ext == ".sql" || ext == ".html") {
                out.push_back(fullPath);
            }
        });
}

// ---------------------------------------------------------------------------
// History helpers
// ---------------------------------------------------------------------------

void SearchDialog::AddToHistoryList(std::vector<std::string>& history, const std::string& entry) {
    if (entry.empty()) return;
    history.erase(std::remove(history.begin(), history.end(), entry), history.end());
    history.insert(history.begin(), entry);
    if (history.size() > 10)
        history.resize(10);
}

bool SearchDialog::RenderHistoryInput(const char* id, char* buf, int bufSize,
                                       std::vector<std::string>& history, float inputW) {
    float arrowW = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(inputW - arrowW);
    bool enterPressed = ImGui::InputText(id, buf, bufSize, ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::SameLine(0, 0);
    std::string popupId = std::string(id) + "_popup";
    if (ImGui::ArrowButton((std::string(id) + "_arrow").c_str(), ImGuiDir_Down)) {
        if (!history.empty())
            ImGui::OpenPopup(popupId.c_str());
    }

    if (ImGui::BeginPopup(popupId.c_str())) {
        for (const auto& item : history) {
            if (ImGui::Selectable(item.c_str())) {
                strncpy(buf, item.c_str(), static_cast<size_t>(bufSize) - 1);
                buf[bufSize - 1] = '\0';
            }
        }
        ImGui::EndPopup();
    }

    return enterPressed;
}

// ---------------------------------------------------------------------------
// State persistence
// ---------------------------------------------------------------------------

void SearchDialog::SaveState(nlohmann::json& j) const {
    j["filterPresetIndex"] = m_filterPresetIndex;
    j["frMatchWholeWord"] = m_frMatchWholeWord;
    j["frMatchCase"] = m_frMatchCase;
    j["frInSubFolders"] = m_frInSubFolders;
    j["frSearchMode"] = m_frSearchMode;
    j["frDotMatchesNewline"] = m_frDotMatchesNewline;
    j["ffMatchWholeWord"] = m_ffMatchWholeWord;
    j["ffMatchCase"] = m_ffMatchCase;
    j["ffInSubFolders"] = m_ffInSubFolders;
    j["ffSearchMode"] = m_ffSearchMode;
    j["searchFileNames"] = m_searchFileNames;
    j["searchFolderNames"] = m_searchFolderNames;
    j["searchByPath"] = m_searchByPath;
    j["searchHistory"] = m_searchHistory;
    j["replaceHistory"] = m_replaceHistory;
}

void SearchDialog::LoadState(const nlohmann::json& j) {
    if (j.contains("filterPresetIndex")) m_filterPresetIndex = j["filterPresetIndex"].get<int>();
    if (j.contains("frMatchWholeWord"))  m_frMatchWholeWord = j["frMatchWholeWord"].get<bool>();
    if (j.contains("frMatchCase"))       m_frMatchCase = j["frMatchCase"].get<bool>();
    if (j.contains("frInSubFolders"))    m_frInSubFolders = j["frInSubFolders"].get<bool>();
    if (j.contains("frSearchMode"))      m_frSearchMode = j["frSearchMode"].get<int>();
    if (j.contains("frDotMatchesNewline")) m_frDotMatchesNewline = j["frDotMatchesNewline"].get<bool>();
    if (j.contains("ffMatchWholeWord"))  m_ffMatchWholeWord = j["ffMatchWholeWord"].get<bool>();
    if (j.contains("ffMatchCase"))       m_ffMatchCase = j["ffMatchCase"].get<bool>();
    if (j.contains("ffInSubFolders"))    m_ffInSubFolders = j["ffInSubFolders"].get<bool>();
    if (j.contains("ffSearchMode"))      m_ffSearchMode = j["ffSearchMode"].get<int>();
    if (j.contains("searchFileNames"))   m_searchFileNames = j["searchFileNames"].get<bool>();
    if (j.contains("searchFolderNames")) m_searchFolderNames = j["searchFolderNames"].get<bool>();
    if (j.contains("searchByPath"))      m_searchByPath = j["searchByPath"].get<bool>();
    if (j.contains("searchHistory") && j["searchHistory"].is_array()) {
        m_searchHistory.clear();
        for (const auto& s : j["searchHistory"])
            if (s.is_string()) m_searchHistory.push_back(s.get<std::string>());
    }
    if (j.contains("replaceHistory") && j["replaceHistory"].is_array()) {
        m_replaceHistory.clear();
        for (const auto& s : j["replaceHistory"])
            if (s.is_string()) m_replaceHistory.push_back(s.get<std::string>());
    }
}

} // namespace GGPKExp
