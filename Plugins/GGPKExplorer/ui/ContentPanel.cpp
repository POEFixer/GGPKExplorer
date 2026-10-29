#include "ContentPanel.h"
#include "SchemaGate.h"
#include "UiText.h"
#include "../core/TextEncoding.h"
#include "../viewers/HexViewer.h"
#include "../viewers/TextViewer.h"
#include "../viewers/ImageViewer.h"
#include "../viewers/DatViewer.h"
#include "../viewers/AudioViewer.h"
#include "../viewers/TerrainViewer.h"
#include "../viewers/MtpViewer.h"
#include "../viewers/MatTableViewer.h"
#include "../viewers/MeshViewer.h"
#include "../viewers/PsgViewer.h"
#include "../viewers/AocViewer.h"
#include <imgui.h>

namespace GGPKExp {

// ---------------------------------------------------------------------------
// InitViewers — called once from SetD3DDevice when D3D device first available
// ---------------------------------------------------------------------------

void ContentPanel::InitViewers()
{
    if (m_viewersInitialized)
        return;

    m_viewers[FileType::Binary]      = std::make_unique<HexViewer>();
    m_viewers[FileType::Text]        = std::make_unique<TextViewer>();
    m_viewers[FileType::Audio]       = std::make_unique<AudioViewer>();
    m_viewers[FileType::MatTable]    = std::make_unique<MatTableViewer>();
    m_viewers[FileType::TerrainMeta] = std::make_unique<MtpViewer>();

    // Shader uses the same TextViewer — store a second entry pointing to the
    // Text viewer. We cannot share ownership, so Shader maps to Text at lookup time.
    // (Handled in GetViewer() fallback, not stored separately.)

    auto imgViewer = std::make_unique<ImageViewer>();
    if (m_device)
        imgViewer->Initialize(m_device);
    m_viewers[FileType::Image] = std::move(imgViewer);

    auto datViewer = std::make_unique<DatViewer>();
    if (m_schema)
        datViewer->SetSchema(m_schema);
    if (m_taskRunner)
        datViewer->SetTaskRunner(m_taskRunner);
    if (m_tree)
        datViewer->SetTree(m_tree);
    if (m_manager)
        datViewer->SetGGPKManager(m_manager);
    datViewer->SetFA6Available(m_fa6Available);
    // Phase 7 / T23 — push the persisted editor panel width into the new viewer
    // so the pane appears at the user's last-dragged size on first open.
    datViewer->SetDatEditorPanelWidth(m_cachedDatEditorPanelWidth);
    m_viewers[FileType::Dat] = std::move(datViewer);

    auto terrViewer = std::make_unique<TerrainViewer>();
    if (m_device)
        terrViewer->Initialize(m_device);
    m_viewers[FileType::Terrain] = std::move(terrViewer);

    auto psgViewer = std::make_unique<PsgViewer>();
    psgViewer->SetContext(m_manager, m_schema, m_device, m_taskRunner);
    if (m_uiSettings) psgViewer->SetUiSettings(m_uiSettings);
    m_viewers[FileType::SkillTree] = std::move(psgViewer);

    auto meshViewer = std::make_unique<MeshViewer>();
    if (m_device)
        meshViewer->Initialize(m_device);
    if (m_manager)
        meshViewer->SetContext(m_manager, m_tree);
    // Apply cached animation settings (set before viewer existed).
    if (m_animPanelState)
        meshViewer->SetAnimPanelState(m_animPanelState);
    if (m_viewSettings)
        meshViewer->SetViewSettings(m_viewSettings);
    if (m_playbackSpeedPtr)
        meshViewer->SetPlaybackSpeedPtr(m_playbackSpeedPtr);
    if (m_hasInitialPlaybackSpeed)
        meshViewer->SetPlaybackSpeed(m_initialPlaybackSpeed);
    m_viewers[FileType::Mesh] = std::move(meshViewer);

    auto aocViewer = std::make_unique<AocViewer>();
    if (m_device)
        aocViewer->Initialize(m_device);
    if (m_manager)
        aocViewer->SetContext(m_manager, m_tree);
    if (m_schema) {
        aocViewer->SetDatSchema(m_schema);
    }
    // Phase 4 — inject previous-selection provider so "Attach Selected Mesh"
    // context menu in the Sockets tab can reach ContentPanel's state.
    aocViewer->SetSelectionProvider([this]() -> GGPKExp::AocContentSelection {
        auto ps = GetPreviousSelection();
        GGPKExp::AocContentSelection out;
        out.path  = std::move(ps.path);
        out.bytes = std::move(ps.bytes);
        return out;
    });
    // Apply cached animation settings (set before viewer existed).
    if (m_animPanelState)
        aocViewer->SetAnimPanelState(m_animPanelState);
    if (m_playbackSpeedPtr)
        aocViewer->SetPlaybackSpeedPtr(m_playbackSpeedPtr);
    if (m_hasInitialPlaybackSpeed)
        aocViewer->SetPlaybackSpeed(m_initialPlaybackSpeed);
    if (!m_pluginDir.empty())
        aocViewer->SetPluginDir(m_pluginDir);
    m_viewers[FileType::Aoc] = std::move(aocViewer);

    m_viewersInitialized = true;
}

// ---------------------------------------------------------------------------
// Animation settings forwarders — dispatch to MeshViewer + AocViewer.
// Cached when viewers don't exist yet; applied on InitViewers().
// ---------------------------------------------------------------------------

void ContentPanel::SetAnimPanelState(GGPKExp::AnimationPanelState* state)
{
    m_animPanelState = state;
    if (auto* mv = dynamic_cast<MeshViewer*>(GetViewer(FileType::Mesh)))
        mv->SetAnimPanelState(state);
    if (auto* av = dynamic_cast<AocViewer*>(GetViewer(FileType::Aoc)))
        av->SetAnimPanelState(state);
}

void ContentPanel::SetViewSettings(GGPKExp::MeshViewSettings* state)
{
    m_viewSettings = state;
    if (auto* mv = dynamic_cast<MeshViewer*>(GetViewer(FileType::Mesh)))
        mv->SetViewSettings(state);
}

bool ContentPanel::ConsumeUiSettingsDirty()
{
    bool m = false, a = false, p = false;
    if (auto* mv = dynamic_cast<MeshViewer*>(GetViewer(FileType::Mesh)))
        m = mv->ConsumeUiSettingsDirty();
    if (auto* av = dynamic_cast<AocViewer*>(GetViewer(FileType::Aoc)))
        a = av->ConsumeUiSettingsDirty();
    if (auto* pv = dynamic_cast<PsgViewer*>(GetViewer(FileType::SkillTree)))
        p = pv->ConsumeUiSettingsDirty();
    return m || a || p;
}

float ContentPanel::GetCurrentPlaybackSpeed() const
{
    // Need a non-const path through m_viewers: dynamic_cast the raw pointer.
    auto it = m_viewers.find(FileType::Mesh);
    if (m_currentType == FileType::Mesh && it != m_viewers.end()) {
        if (auto* mv = dynamic_cast<MeshViewer*>(it->second.get()))
            return mv->GetPlaybackSpeed();
    }
    it = m_viewers.find(FileType::Aoc);
    if (m_currentType == FileType::Aoc && it != m_viewers.end()) {
        if (auto* av = dynamic_cast<AocViewer*>(it->second.get()))
            return av->GetPlaybackSpeed();
    }
    return 1.0f;
}

void ContentPanel::SetInitialPlaybackSpeed(float s)
{
    m_initialPlaybackSpeed = s;
    m_hasInitialPlaybackSpeed = true;
    if (auto* mv = dynamic_cast<MeshViewer*>(GetViewer(FileType::Mesh)))
        mv->SetPlaybackSpeed(s);
    if (auto* av = dynamic_cast<AocViewer*>(GetViewer(FileType::Aoc)))
        av->SetPlaybackSpeed(s);
}

void ContentPanel::SetPlaybackSpeedPtr(float* ptr)
{
    m_playbackSpeedPtr = ptr;
    // Propagate to active viewers if they already exist. Pattern mirrors
    // SetAnimPanelState above.
    if (auto* mv = dynamic_cast<MeshViewer*>(GetViewer(FileType::Mesh)))
        mv->SetPlaybackSpeedPtr(ptr);
    if (auto* av = dynamic_cast<AocViewer*>(GetViewer(FileType::Aoc)))
        av->SetPlaybackSpeedPtr(ptr);
}

void ContentPanel::SetUiSettings(GGPKExp::GGPKExplorerUiSettings* settings)
{
    m_uiSettings = settings;
    if (auto* pv = dynamic_cast<PsgViewer*>(GetViewer(FileType::SkillTree))) {
        pv->SetUiSettings(settings);
    }
}

void ContentPanel::SetPluginDir(const std::string& dir)
{
    m_pluginDir = dir;
    if (auto* av = dynamic_cast<AocViewer*>(GetViewer(FileType::Aoc)))
        av->SetPluginDir(m_pluginDir);
}

void ContentPanel::SetDatSchema(DatSchema* schema)
{
    const bool changed = schema != m_schema;
    m_schema = schema;
    // Forward to the viewers that exist (otherwise InitViewers picks up the
    // cached m_schema pointer on first lazy-init). A replaced schema stays
    // alive (GGPKExplorer keeps every one), so a viewer's or a worker's table
    // from the old one stays valid until it reloads.
    if (auto* av = dynamic_cast<AocViewer*>(GetViewer(FileType::Aoc))) {
        av->SetDatSchema(m_schema);
    }
    if (auto* pv = dynamic_cast<PsgViewer*>(GetViewer(FileType::SkillTree))) {
        pv->SetContext(m_manager, m_schema, m_device, m_taskRunner);
    }
    DatViewer* dv = GetDatViewer();
    if (dv) dv->SetSchema(m_schema);
    // Reopen what is on screen with the new schema: the table (unless it holds edits) and the
    // Skill Tree, which reads Stats and PassiveSkills through it and stays failed otherwise.
    if (changed) SchemaGate::OnSchemaChanged(m_viewerLoaded, dv && dv->HasPendingEdits());
}

bool ContentPanel::SchemaReady() const
{
    return m_schema && m_schema->IsLoaded();
}

void ContentPanel::SetFA6Available(bool available)
{
    m_fa6Available = available;
    // Forward to DatViewer if it's already been created. Otherwise InitViewers
    // will pick up the cached m_fa6Available value during lazy creation.
    if (auto* dv = GetDatViewer())
        dv->SetFA6Available(available);
}

// ---------------------------------------------------------------------------
// Phase 7 / T23 — persisted DatViewer editor panel width.
//
// Writer (plugin → viewer): cache value, and if viewer already exists push it
// in live. On the next DatViewer load nothing further is needed — InitViewers
// reads the cached value and calls SetDatEditorPanelWidth directly.
//
// Reader (viewer → plugin): if the viewer exists return its live width; else
// return the cached value. This covers the shutdown case where SaveSettings
// runs before any .datc64 was ever opened.
// ---------------------------------------------------------------------------
void ContentPanel::SetDatEditorPanelWidth(float w)
{
    m_cachedDatEditorPanelWidth = w;
    if (auto* dv = GetDatViewer())
        dv->SetDatEditorPanelWidth(w);
}

float ContentPanel::GetDatEditorPanelWidth() const
{
    if (const auto it = m_viewers.find(FileType::Dat); it != m_viewers.end()) {
        if (auto* dv = dynamic_cast<DatViewer*>(it->second.get()))
            return dv->GetDatEditorPanelWidth();
    }
    return m_cachedDatEditorPanelWidth;
}

// ---------------------------------------------------------------------------
// SetD3DDevice — stores device, initializes viewers if first call
// ---------------------------------------------------------------------------

void ContentPanel::SetD3DDevice(ID3D11Device* device)
{
    m_device = device;
    InitViewers();

    // If viewers already existed (re-init), update device-dependent viewers
    if (auto* iv = dynamic_cast<ImageViewer*>(GetViewer(FileType::Image)))
        iv->Initialize(device);
    if (auto* tv = dynamic_cast<TerrainViewer*>(GetViewer(FileType::Terrain)))
        tv->Initialize(device);
    if (auto* mv = dynamic_cast<MeshViewer*>(GetViewer(FileType::Mesh))) {
        mv->Initialize(device);
        mv->SetContext(m_manager, m_tree);
    }
    if (auto* av = dynamic_cast<AocViewer*>(GetViewer(FileType::Aoc))) {
        av->Initialize(device);
        av->SetContext(m_manager, m_tree);
    }
}

// ---------------------------------------------------------------------------
// SetVirtualTree — stores tree pointer and propagates to tree-aware viewers.
// Called after GGPK load (post-InitViewers), so existing viewers must be
// updated so hyperlink validation works for freshly loaded DATs.
// ---------------------------------------------------------------------------

void ContentPanel::SetVirtualTree(VirtualTree* tree)
{
    m_tree = tree;
    if (auto* dv = GetDatViewer())
        dv->SetTree(tree);
    if (auto* mv = dynamic_cast<MeshViewer*>(GetViewer(FileType::Mesh)))
        mv->SetContext(m_manager, tree);
    if (auto* av = dynamic_cast<AocViewer*>(GetViewer(FileType::Aoc)))
        av->SetContext(m_manager, tree);
}

// ---------------------------------------------------------------------------
// Typed accessors
// ---------------------------------------------------------------------------

TextViewer* ContentPanel::GetTextViewer()
{
    auto it = m_viewers.find(FileType::Text);
    return it != m_viewers.end() ? static_cast<TextViewer*>(it->second.get()) : nullptr;
}

DatViewer* ContentPanel::GetDatViewer()
{
    auto it = m_viewers.find(FileType::Dat);
    return it != m_viewers.end() ? static_cast<DatViewer*>(it->second.get()) : nullptr;
}

IViewer* ContentPanel::GetViewer(FileType type)
{
    // Shader files use the Text viewer
    if (type == FileType::Shader)
        type = FileType::Text;

    auto it = m_viewers.find(type);
    if (it != m_viewers.end())
        return it->second.get();

    // Fallback to hex for unknown types
    it = m_viewers.find(FileType::Binary);
    return it != m_viewers.end() ? it->second.get() : nullptr;
}

// ---------------------------------------------------------------------------
// LoadFile — called when the user selects a new tree entry
// ---------------------------------------------------------------------------

void ContentPanel::LoadFile(const std::string& path, const std::string& name,
                             int64_t size, FileType type, void* nodePtr)
{
    // Check if TextViewer has unsaved changes
    if (auto* tv = GetTextViewer()) {
        if (tv->HasUnsavedChanges()) {
            m_pendingPath = path;
            m_pendingName = name;
            m_pendingSize = size;
            m_pendingType = type;
            m_pendingNodePtr = nodePtr;
            m_showSavePrompt = true;
            return;
        }
    }
    DoLoadFile(path, name, size, type, nodePtr);
}

void ContentPanel::DoLoadFile(const std::string& path, const std::string& name,
                               int64_t size, FileType type, void* nodePtr)
{
    // Phase 4 — stash current → previous if it was Mesh (.sm) or Aoc (.aoc).
    // Requires m_dataLoaded so bytes are already in m_fileData; if not yet
    // loaded (tree node clicked but EnsureDataLoaded hasn't run), skip stashing.
    // Also require non-empty bytes + no load error so AocViewer doesn't
    // receive empty bytes from a prior failed read when Attach Selected
    // Mesh is invoked on the new file.
    if (m_dataLoaded && !m_currentPath.empty() &&
        !m_fileData.empty() && m_loadError.empty() &&
        (m_currentType == FileType::Mesh || m_currentType == FileType::Aoc)) {
        m_previousPath     = m_currentPath;
        m_previousFileType = m_currentType;
        m_previousBytes    = m_fileData;   // copy — m_fileData is cleared below
    }

    m_currentPath    = path;
    m_currentName    = name;
    m_currentSize    = size;
    m_currentType    = type;
    m_currentNodePtr = nodePtr;

    // Discard previous data
    m_fileData.clear();
    m_dataLoaded = false;
    m_loadError.clear();
    m_loadErrorKey = nullptr;

    // Reset all per-viewer loaded flags
    m_viewerLoaded.clear();

    // Clear viewers that hold state
    for (auto& [ft, viewer] : m_viewers)
        viewer->Clear();

    // Populate properties (metadata known without reading the file)
    m_properties             = FileProperties{};
    m_properties.name        = name;
    m_properties.fullPath    = path;
    m_properties.uncompressedSize = size >= 0 ? size : 0;
    m_properties.type        = type;

    m_tabSelectionApplied = false;
    m_isLikelyText = false;
    m_deferredScrollLine = -1;
    m_deferredScrollRow = -1;
    switch (type) {
    case FileType::Image:         m_defaultTab = DefaultTab::Image; break;
    case FileType::Dat:           m_defaultTab = DefaultTab::Dat;   break;
    case FileType::Text:
    case FileType::Shader:        m_defaultTab = DefaultTab::Text;  break;
    case FileType::Audio:         m_defaultTab = DefaultTab::Audio; break;
    case FileType::Mesh:          m_defaultTab = DefaultTab::Mesh; break;
    case FileType::Terrain:       m_defaultTab = DefaultTab::Terrain; break;
    case FileType::TerrainMeta:   m_defaultTab = DefaultTab::TerrainMeta; break;
    case FileType::MatTable:      m_defaultTab = DefaultTab::MatTable; break;
    case FileType::SkillTree:     m_defaultTab = DefaultTab::SkillTree; break;
    case FileType::Aoc:           m_defaultTab = DefaultTab::Aoc; break;
    default:                      m_defaultTab = DefaultTab::Hex;   break;
    }

    // Special case: mat_table.dat is not a standard .dat table
    if (m_currentType == FileType::Dat) {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower == "mat_table.dat") {
            m_currentType = FileType::MatTable;
            m_defaultTab = DefaultTab::MatTable;
        }
    }
}

// ---------------------------------------------------------------------------
// Clear — reset to empty state
// ---------------------------------------------------------------------------

void ContentPanel::Clear()
{
    m_currentPath.clear();
    m_currentName.clear();
    m_currentType    = FileType::Unknown;
    m_currentNodePtr = nullptr;
    m_currentSize    = 0;
    m_fileData.clear();
    m_dataLoaded  = false;
    m_isLikelyText = false;
    m_defaultTab = DefaultTab::None;
    m_tabSelectionApplied = false;
    m_loadError.clear();
    m_loadErrorKey = nullptr;
    m_properties  = FileProperties{};
    pendingNavigatePath.clear();
    pendingNavigateRow = -1;
    navNotFoundPath.clear();

    // Clear all viewers
    for (auto& [ft, viewer] : m_viewers)
        viewer->Clear();
    m_viewerLoaded.clear();

    m_previousPath.clear();
    m_previousFileType = FileType::Unknown;
    m_previousBytes.clear();
}

// ---------------------------------------------------------------------------
// EnsureDataLoaded — lazy read from GGPK
// ---------------------------------------------------------------------------

void ContentPanel::EnsureDataLoaded()
{
    if (m_dataLoaded)
        return;

    if (!m_manager || !m_currentNodePtr) {
        SetOwnLoadError(I18n::Key("content_no_file_data"));
        m_dataLoaded = true;
        return;
    }

    m_isLoading = true;
    auto readResult = m_manager->ReadFile(m_currentNodePtr);
    m_isLoading = false;

    if (!readResult) {
        m_loadError = readResult.error;
    } else {
        m_fileData = std::move(readResult.value);
        if (m_fileData.empty() && m_currentSize > 0)
            SetOwnLoadError(I18n::Key("content_decompress_failed"));
        // size==0 is a legitimately empty file — no error
    }

    m_dataLoaded = true;
}

// ---------------------------------------------------------------------------
// Render — draws the right-panel content for the selected file
// ---------------------------------------------------------------------------

void ContentPanel::Render()
{
    // No file selected: show welcome screen (hotkeys + file formats, two columns).
    if (m_currentPath.empty()) {
        const float winW = ImGui::GetContentRegionAvail().x;
        const float winH = ImGui::GetContentRegionAvail().y;

        // Sizes in em: the translated texts wrap inside their columns.
        const float em      = ImGui::GetFontSize();
        const float kColW   = em * 24.0f;
        const float kColGap = em * 2.0f;
        const float kPanelW = kColW * 2.0f + kColGap;
        const float kPanelH = em * 55.0f;

        float panelX = (winW - kPanelW) * 0.5f;
        float panelY = (winH - kPanelH) * 0.5f;
        if (panelX < 10.0f) panelX = 10.0f;
        if (panelY < 10.0f) panelY = 10.0f;

        ImGui::SetCursorPos(ImVec2(panelX, panelY));
        ImGui::BeginChild("##WelcomePanel",
                          ImVec2(kPanelW, kPanelH),
                          ImGuiChildFlags_None);

        if (ImGui::BeginTable("##welcomecols", 2,
                              ImGuiTableFlags_SizingFixedFit
                              | ImGuiTableFlags_NoBordersInBody)) {
            ImGui::TableSetupColumn("##hotkeys", ImGuiTableColumnFlags_WidthFixed, kColW);
            ImGui::TableSetupColumn("##formats", ImGuiTableColumnFlags_WidthFixed, kColW);
            ImGui::TableNextRow();

            // --- Column 1: Keyboard Shortcuts ---
            ImGui::TableNextColumn();
            {
                const char* title = I18n::Tr("welcome_shortcuts_title");
                const ImVec2 titleSize = ImGui::CalcTextSize(title);
                if (titleSize.x < kColW)
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                                         + (kColW - titleSize.x) * 0.5f);
                ImGui::TextDisabled("%s", title);
                ImGui::Spacing();

                // Each row: the key chord (code, or a translated text) and its translated action.
                auto renderGroup = [&](const char* groupName,
                    std::initializer_list<std::pair<const char*, const char*>> items) {
                    ImGui::SeparatorText(groupName);
                    if (ImGui::BeginTable("##hk_tbl", 2,
                                          ImGuiTableFlags_None,
                                          ImVec2(kColW, 0))) {
                        ImGui::TableSetupColumn("##key", ImGuiTableColumnFlags_WidthFixed, em * 8.5f);
                        ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthStretch);
                        for (auto& [key, action] : items) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextDisabled("%s", key);
                            ImGui::TableNextColumn();
                            ImGui::TextWrapped("%s", action);
                        }
                        ImGui::EndTable();
                    }
                };

                renderGroup(I18n::Tr("welcome_group_navigation"), {
                    {"Ctrl+P",                          I18n::Tr("hotkey_quick_jump")},
                    {"Alt+\xe2\x86\x90",                I18n::Tr("hotkey_navigate_back")},
                    {"Alt+\xe2\x86\x92",                I18n::Tr("hotkey_navigate_forward")},
                    {I18n::Tr("hotkey_ctrl_click"),     I18n::Tr("hotkey_follow_link")},
                });
                renderGroup(I18n::Tr("welcome_group_search"), {
                    {"Ctrl+F",       I18n::Tr("hotkey_find_in_scope")},
                    {"Ctrl+R",       I18n::Tr("hotkey_replace_in_files")},
                    {"Ctrl+Shift+F", I18n::Tr("hotkey_global_search")},
                });
                renderGroup(I18n::Tr("welcome_group_file_operations"), {
                    {"Ctrl+D", I18n::Tr("hotkey_toggle_bookmark")},
                    {"F5",     I18n::Tr("hotkey_reload")},
                    {"Escape", I18n::Tr("hotkey_close_dialog")},
                });
            }

            // --- Column 2: File Formats ---
            ImGui::TableNextColumn();
            {
                const char* title = I18n::Tr("welcome_formats_title");
                const ImVec2 titleSize = ImGui::CalcTextSize(title);
                if (titleSize.x < kColW)
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                                         + (kColW - titleSize.x) * 0.5f);
                ImGui::TextDisabled("%s", title);
                ImGui::Spacing();

                // Rows hold keys (I18n::Key), looked up when drawn.
                struct FmtRow {
                    const char* icon;   // UTF-8 glyph from Font Awesome atlas
                    const char* ext;    // an extension list, or a key when extIsKey
                    bool        extIsKey;
                    ImVec4      color;  // alpha > 0 => use color; else default text
                    const char* descKey;
                };
                auto renderFmtGroup = [&](const char* groupName,
                                           const FmtRow* rows, size_t count) {
                    ImGui::SeparatorText(groupName);
                    if (ImGui::BeginTable("##fmt_tbl", 3,
                                          ImGuiTableFlags_None,
                                          ImVec2(kColW, 0))) {
                        ImGui::TableSetupColumn("##icon", ImGuiTableColumnFlags_WidthFixed, em * 2.0f);
                        ImGui::TableSetupColumn("##ext", ImGuiTableColumnFlags_WidthFixed, em * 8.5f);
                        ImGui::TableSetupColumn("##desc", ImGuiTableColumnFlags_WidthStretch);
                        for (size_t i = 0; i < count; ++i) {
                            const auto& r = rows[i];
                            const char* ext = r.extIsKey ? I18n::Tr(r.ext) : r.ext;
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            if (r.color.w > 0.0f) ImGui::TextColored(r.color, "%s", r.icon);
                            else                  ImGui::Text("%s", r.icon);
                            ImGui::TableNextColumn();
                            if (r.color.w > 0.0f) ImGui::TextColored(r.color, "%s", ext);
                            else                  ImGui::Text("%s", ext);
                            ImGui::TableNextColumn();
                            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                            ImGui::TextWrapped("%s", I18n::Tr(r.descKey));
                            ImGui::PopStyleColor();
                        }
                        ImGui::EndTable();
                    }
                };

                static const FmtRow kCore[] = {
                    {"\xef\x84\xae", ".aoc",         false, ImVec4(0.95f, 0.55f, 0.75f, 1.0f), I18n::Key("format_aoc")},
                    {"\xef\x86\xb2", ".sm",          false, ImVec4(0.75f, 0.65f, 0.95f, 1.0f), I18n::Key("format_sm")},
                    {"\xef\x83\xa8", ".psg",         false, ImVec4(0.70f, 0.85f, 0.55f, 1.0f), I18n::Key("format_psg")},
                    {"\xef\x86\xb2", ".smd",         false, ImVec4(0.55f, 0.80f, 0.95f, 1.0f), I18n::Key("format_smd")},
                    {"\xef\x97\x97", ".ast",         false, ImVec4(0.95f, 0.80f, 0.50f, 1.0f), I18n::Key("format_ast")},
                    {"\xef\x83\x8e", ".dat/.datc64", false, ImVec4(0.50f, 0.75f, 0.90f, 1.0f), I18n::Key("format_dat")},
                };
                static const FmtRow kViewers[] = {
                    {"\xef\x89\xb9", ".amd",       false, ImVec4(0, 0, 0, 0), I18n::Key("format_amd")},
                    {"\xef\x89\xb9", ".mtp",       false, ImVec4(0, 0, 0, 0), I18n::Key("format_mtp")},
                    {"\xef\x80\x8a", ".tdt",       false, ImVec4(0, 0, 0, 0), I18n::Key("format_tdt")},
                    {"\xef\x87\x85", ".dds/.png",  false, ImVec4(0, 0, 0, 0), I18n::Key("format_image")},
                    {"\xef\x87\x87", ".ogg/.wav",  false, ImVec4(0, 0, 0, 0), I18n::Key("format_audio")},
                    {"\xef\x80\x93", ".fx/.hlsl",  false, ImVec4(0, 0, 0, 0), I18n::Key("format_shader")},
                    {"\xef\x85\x9c", ".txt/.json", false, ImVec4(0, 0, 0, 0), I18n::Key("format_text")},
                    {"\xef\x85\x9b", I18n::Key("format_binary_ext"), true, ImVec4(0, 0, 0, 0), I18n::Key("format_binary")},
                };

                renderFmtGroup(I18n::Tr("welcome_group_core"), kCore, sizeof(kCore) / sizeof(kCore[0]));
                renderFmtGroup(I18n::Tr("welcome_group_viewers"), kViewers, sizeof(kViewers) / sizeof(kViewers[0]));
            }

            ImGui::EndTable();
        }

        ImGui::EndChild();
        return;
    }

    // Determine tab visibility based on file type
    bool showTextTab  = (m_currentType == FileType::Text ||
                         m_currentType == FileType::Shader ||
                         m_currentType == FileType::Aoc);

    // .sm files are Mesh type but also readable as text — show Text tab too
    if (m_currentType == FileType::Mesh) {
        std::string ext;
        auto dot = m_currentName.rfind('.');
        if (dot != std::string::npos) {
            ext = m_currentName.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        }
        if (ext == ".sm") showTextTab = true;
    }
    bool showImageTab = (m_currentType == FileType::Image);
    bool showDatTab   = (m_currentType == FileType::Dat);
    bool showAudioTab = (m_currentType == FileType::Audio);

    // Heuristic: check if Binary/Unknown file is actually text.
    // Trigger lazy data load to run the heuristic before building tab bar.
    if (!m_isLikelyText && !m_dataLoaded &&
        (m_currentType == FileType::Binary || m_currentType == FileType::Unknown) &&
        m_currentNodePtr)
    {
        EnsureDataLoaded();
    }
    if (!m_isLikelyText && m_dataLoaded &&
        (m_currentType == FileType::Binary || m_currentType == FileType::Unknown) &&
        !m_fileData.empty())
    {
        m_isLikelyText = GGPKExp::IsLikelyText(m_fileData);
        if (m_isLikelyText)
            m_defaultTab = DefaultTab::Text;  // override Hex default
    }
    if (m_isLikelyText)
        showTextTab = true;

    // (Terrain grid heuristic removed — .tdt files detected by extension)

    bool showMeshTab      = (m_currentType == FileType::Mesh);
    bool showTerrainTab   = (m_currentType == FileType::Terrain);
    bool showTerrainMetaTab = (m_currentType == FileType::TerrainMeta);
    bool showMatTableTab  = (m_currentType == FileType::MatTable);
    bool showSkillTreeTab = (m_currentType == FileType::SkillTree);
    bool showAocTab       = (m_currentType == FileType::Aoc);

    // Build per-tab flags for auto-selection on first render
    ImGuiTabItemFlags textTabFlags  = ImGuiTabItemFlags_None;
    ImGuiTabItemFlags imageTabFlags = ImGuiTabItemFlags_None;
    ImGuiTabItemFlags datTabFlags   = ImGuiTabItemFlags_None;
    ImGuiTabItemFlags hexTabFlags   = ImGuiTabItemFlags_None;
    ImGuiTabItemFlags audioTabFlags = ImGuiTabItemFlags_None;
    ImGuiTabItemFlags meshTabFlags      = ImGuiTabItemFlags_None;
    ImGuiTabItemFlags terrainTabFlags   = ImGuiTabItemFlags_None;
    ImGuiTabItemFlags terrainMetaTabFlags = ImGuiTabItemFlags_None;
    ImGuiTabItemFlags matTableTabFlags  = ImGuiTabItemFlags_None;
    ImGuiTabItemFlags skillTreeTabFlags = ImGuiTabItemFlags_None;
    ImGuiTabItemFlags aocTabFlags       = ImGuiTabItemFlags_None;

    if (!m_tabSelectionApplied) {
        switch (m_defaultTab) {
        case DefaultTab::Text:  textTabFlags  = ImGuiTabItemFlags_SetSelected; break;
        case DefaultTab::Image: imageTabFlags = ImGuiTabItemFlags_SetSelected; break;
        case DefaultTab::Dat:   datTabFlags   = ImGuiTabItemFlags_SetSelected; break;
        case DefaultTab::Audio: audioTabFlags = ImGuiTabItemFlags_SetSelected; break;
        case DefaultTab::Hex:   hexTabFlags   = ImGuiTabItemFlags_SetSelected; break;
        case DefaultTab::Mesh:        meshTabFlags = ImGuiTabItemFlags_SetSelected; break;
        case DefaultTab::Terrain:     terrainTabFlags = ImGuiTabItemFlags_SetSelected; break;
        case DefaultTab::TerrainMeta: terrainMetaTabFlags = ImGuiTabItemFlags_SetSelected; break;
        case DefaultTab::MatTable:    matTableTabFlags = ImGuiTabItemFlags_SetSelected; break;
        case DefaultTab::SkillTree:  skillTreeTabFlags = ImGuiTabItemFlags_SetSelected; break;
        case DefaultTab::Aoc:         aocTabFlags = ImGuiTabItemFlags_SetSelected; break;
        default: break;
        }
        m_tabSelectionApplied = true;
    }

    // Propagate "file not found" signal from GGPKExplorer back to TextViewer
    if (!navNotFoundPath.empty()) {
        if (auto* tv = GetTextViewer())
            tv->ShowNotFound(navNotFoundPath);
        navNotFoundPath.clear();
    }

    // Unsaved changes prompt
    const char* savePromptTitle = I18n::TrId("unsaved_title");
    if (m_showSavePrompt) {
        ImGui::OpenPopup(savePromptTitle);
        m_showSavePrompt = false;
    }

    if (ImGui::BeginPopupModal(savePromptTitle, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextUnformatted(I18n::TrF("unsaved_body", { { "name", m_currentName } }).c_str());
        ImGui::Spacing();
        const char* saveLabel = I18n::TrId("unsaved_save");
        const char* discardLabel = I18n::TrId("unsaved_discard");
        const char* cancelLabel = I18n::TrId("common_cancel");
        float promptBtnW = ButtonWidth(saveLabel, 9.0f);
        if (ButtonWidth(discardLabel, 9.0f) > promptBtnW) promptBtnW = ButtonWidth(discardLabel, 9.0f);
        if (ButtonWidth(cancelLabel, 9.0f) > promptBtnW) promptBtnW = ButtonWidth(cancelLabel, 9.0f);
        if (ImGui::Button(saveLabel, ImVec2(promptBtnW, 0))) {
            if (auto* tv = GetTextViewer()) {
                if (tv->HasUnsavedChanges() && m_manager && m_currentNodePtr) {
                    auto text = tv->GetText();
                    auto data = EncodeFromUtf8(text, tv->GetOriginalEncoding(), tv->OriginalHadBom());
                    m_manager->WriteFile(m_currentNodePtr, data);
                }
            }
            ImGui::CloseCurrentPopup();
            DoLoadFile(m_pendingPath, m_pendingName, m_pendingSize, m_pendingType, m_pendingNodePtr);
        }
        ImGui::SameLine();
        if (ImGui::Button(discardLabel, ImVec2(promptBtnW, 0))) {
            ImGui::CloseCurrentPopup();
            DoLoadFile(m_pendingPath, m_pendingName, m_pendingSize, m_pendingType, m_pendingNodePtr);
        }
        ImGui::SameLine();
        if (ImGui::Button(cancelLabel, ImVec2(promptBtnW, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // The load error line: our own message (a key) or the reader's English detail.
    auto showLoadError = [this]() {
        const std::string detail = m_loadErrorKey ? std::string(I18n::Tr(m_loadErrorKey)) : m_loadError;
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s",
                           I18n::TrF("error_detail", { { "detail", detail } }).c_str());
    };

    if (!ImGui::BeginTabBar("##ContentTabs"))
        return;

    // ------------------------------------------------------------------
    // AOC tab — rendered first for .aoc files so it is the default.
    // Text tab (below) still appears for .aoc, but AOC wins the default
    // selection + leftmost position.
    // ------------------------------------------------------------------
    if (showAocTab) {
        if (ImGui::BeginTabItem("AOC", nullptr, aocTabFlags)) {
            if (!m_viewerLoaded[FileType::Aoc]) {
                EnsureDataLoaded();
                if (m_loadError.empty()) {
                    if (auto* v = GetViewer(FileType::Aoc)) {
                        v->LoadData(std::vector<uint8_t>(m_fileData), m_currentPath);
                        if (auto* av = dynamic_cast<AocViewer*>(v)) {
                            av->SetOnSave([this](const std::vector<uint8_t>& data) {
                                if (m_manager && m_currentNodePtr) {
                                    m_manager->WriteFile(m_currentNodePtr, data);
                                }
                            });
                        }
                    }
                }
                m_viewerLoaded[FileType::Aoc] = true;
            }
            if (!m_loadError.empty())
                showLoadError();
            else {
                if (auto* v = GetViewer(FileType::Aoc)) {
                    v->Render();
                }
            }
            ImGui::EndTabItem();
        }
    }

    // ------------------------------------------------------------------
    // Text tab — for Text/Shader types and heuristic-detected text files
    // ------------------------------------------------------------------
    if (showTextTab) {
        if (ImGui::BeginTabItem(I18n::TrId("tab_text"), nullptr, textTabFlags)) {
            if (!m_viewerLoaded[FileType::Text]) {
                EnsureDataLoaded();
                if (m_loadError.empty()) {
                    if (auto* tv = GetTextViewer()) {
                        try {
                            tv->LoadData(std::vector<uint8_t>(m_fileData), m_currentName);
                            tv->SetOnSave([this, tv](const std::string& text) {
                                if (m_manager && m_currentNodePtr) {
                                    auto data = EncodeFromUtf8(text,
                                                               tv->GetOriginalEncoding(),
                                                               tv->OriginalHadBom());
                                    m_manager->WriteFile(m_currentNodePtr, data);

                                    // Keep in-memory copy fresh so other tabs
                                    // (Hex, AOC reload) see the new bytes
                                    // without a round-trip through GGPK.
                                    m_fileData = data;

                                    // For .aoc: push new bytes to AocViewer so
                                    // the 3D Preview re-parses on next frame.
                                    if (m_currentType == FileType::Aoc) {
                                        if (auto* v = GetViewer(FileType::Aoc)) {
                                            v->LoadData(std::vector<uint8_t>(data),
                                                         m_currentPath);
                                        }
                                        m_viewerLoaded[FileType::Aoc] = true;
                                    }
                                }
                            });
                        } catch (...) {
                            SetOwnLoadError(I18n::Key("content_text_load_failed"));
                        }
                    }
                }
                m_viewerLoaded[FileType::Text] = true;
                // Apply deferred scroll now that data is loaded
                if (m_deferredScrollLine >= 0) {
                    if (auto* tv = GetTextViewer())
                        tv->ScrollToLine(m_deferredScrollLine);
                    m_deferredScrollLine = -1;
                }
            }
            if (!m_loadError.empty()) {
                showLoadError();
            } else {
                if (auto* tv = GetTextViewer()) {
                    tv->Render();
                    // Transfer hyperlink navigation from TextViewer to ContentPanel
                    if (tv->HasPendingNavigation()) {
                        pendingNavigatePath = tv->TakePendingNavigation();
                        pendingNavigateRow = -1;
                    }
                }
            }
            ImGui::EndTabItem();
        }
    }

    // ------------------------------------------------------------------
    // Image tab
    // ------------------------------------------------------------------
    if (showImageTab) {
        if (ImGui::BeginTabItem(I18n::TrId("tab_image"), nullptr, imageTabFlags)) {
            if (!m_viewerLoaded[FileType::Image]) {
                EnsureDataLoaded();
                if (m_loadError.empty()) {
                    if (auto* v = GetViewer(FileType::Image))
                        v->LoadData(std::vector<uint8_t>(m_fileData), m_currentName);
                }
                m_viewerLoaded[FileType::Image] = true;
            }
            if (!m_loadError.empty()) {
                showLoadError();
            } else {
                if (auto* v = GetViewer(FileType::Image))
                    v->Render();
            }
            ImGui::EndTabItem();
        }
    }

    // ------------------------------------------------------------------
    // DAT tab
    // ------------------------------------------------------------------
    if (showDatTab) {
        if (ImGui::BeginTabItem("DAT", nullptr, datTabFlags)) {
            if (!SchemaReady()) {
                // The table waits for its schema (game data the host may still be downloading).
                SchemaNotReadyLine(m_schema ? m_schema->GetError() : std::string());
            } else {
                if (!m_viewerLoaded[FileType::Dat]) {
                    EnsureDataLoaded();
                    if (m_loadError.empty()) {
                        if (auto* dv = GetDatViewer()) {
                            dv->LoadData(std::vector<uint8_t>(m_fileData), m_currentName);
                            dv->SetOnSave([this](const std::vector<uint8_t>& data) {
                                if (m_manager && m_currentNodePtr) {
                                    m_manager->WriteFile(m_currentNodePtr, data);
                                }
                            });
                        }
                    }
                    m_viewerLoaded[FileType::Dat] = true;
                    // Apply deferred scroll now that data is loaded
                    if (m_deferredScrollRow >= 0) {
                        if (auto* dv = GetDatViewer())
                            dv->ScrollToRow(m_deferredScrollRow);
                        m_deferredScrollRow = -1;
                    }
                }
                if (!m_loadError.empty()) {
                    showLoadError();
                } else {
                    if (auto* dv = GetDatViewer()) {
                        dv->Render();

                        // Transfer foreign key navigation from DatViewer to ContentPanel
                        if (dv->HasPendingNavigation()) {
                            pendingNavigatePath = dv->TakePendingNavigation();
                            pendingNavigateRow = dv->TakePendingNavRow();
                        }
                    }
                }
            }
            ImGui::EndTabItem();
        }
    }

    // ------------------------------------------------------------------
    // Schema tab (for .datc64 files — shows type definition)
    // ------------------------------------------------------------------
    if (showDatTab) {
        if (ImGui::BeginTabItem(I18n::TrId("tab_schema"))) {
            if (!SchemaReady()) {
                SchemaNotReadyLine(m_schema ? m_schema->GetError() : std::string());
            } else {
                if (!m_viewerLoaded[FileType::Dat]) {
                    EnsureDataLoaded();
                    if (m_loadError.empty()) {
                        if (auto* v = GetViewer(FileType::Dat))
                            v->LoadData(std::vector<uint8_t>(m_fileData), m_currentName);
                    }
                    m_viewerLoaded[FileType::Dat] = true;
                }
                if (!m_loadError.empty()) {
                    showLoadError();
                } else {
                    if (auto* dv = GetDatViewer())
                        dv->RenderSchemaTab();
                }
            }
            ImGui::EndTabItem();
        }
    }

    // ------------------------------------------------------------------
    // Audio tab
    // ------------------------------------------------------------------
    if (showAudioTab) {
        if (ImGui::BeginTabItem(I18n::TrId("tab_audio"), nullptr, audioTabFlags)) {
            if (!m_viewerLoaded[FileType::Audio]) {
                EnsureDataLoaded();
                if (m_loadError.empty()) {
                    if (auto* v = GetViewer(FileType::Audio))
                        v->LoadData(std::vector<uint8_t>(m_fileData), m_currentName);
                }
                m_viewerLoaded[FileType::Audio] = true;
            }
            if (!m_loadError.empty()) {
                showLoadError();
            } else {
                if (auto* v = GetViewer(FileType::Audio))
                    v->Render();
            }
            ImGui::EndTabItem();
        }
    }

    // ------------------------------------------------------------------
    // Model tab (meshes + skeletons) — MeshViewer via IViewer map
    // ------------------------------------------------------------------
    if (showMeshTab) {
        if (ImGui::BeginTabItem(I18n::TrId("tab_model"), nullptr, meshTabFlags)) {
            if (!m_viewerLoaded[FileType::Mesh]) {
                EnsureDataLoaded();
                if (m_loadError.empty()) {
                    if (auto* mv = dynamic_cast<MeshViewer*>(GetViewer(FileType::Mesh))) {
                        mv->SetContext(m_manager, m_tree);
                        mv->LoadMesh(m_fileData, m_currentName, m_currentPath,
                                     m_manager, m_tree);
                    }
                }
                m_viewerLoaded[FileType::Mesh] = true;
            }
            if (!m_loadError.empty())
                showLoadError();
            else {
                if (auto* v = GetViewer(FileType::Mesh))
                    v->Render();
            }
            ImGui::EndTabItem();
        }
    }

    // ------------------------------------------------------------------
    // Terrain Data tab
    // ------------------------------------------------------------------
    if (showTerrainTab) {
        if (ImGui::BeginTabItem(I18n::TrId("tab_terrain"), nullptr, terrainTabFlags)) {
            if (!m_viewerLoaded[FileType::Terrain]) {
                EnsureDataLoaded();
                if (m_loadError.empty()) {
                    if (auto* v = GetViewer(FileType::Terrain))
                        v->LoadData(std::vector<uint8_t>(m_fileData), m_currentName);
                }
                m_viewerLoaded[FileType::Terrain] = true;
            }
            if (!m_loadError.empty())
                showLoadError();
            else {
                if (auto* v = GetViewer(FileType::Terrain)) {
                    v->Render();
                    if (v->HasPendingNavigation()) {
                        pendingNavigatePath = v->TakePendingNavigation();
                        pendingNavigateRow = -1;
                    }
                }
            }
            ImGui::EndTabItem();
        }
    }

    // ------------------------------------------------------------------
    // Terrain Metadata tab
    // ------------------------------------------------------------------
    if (showTerrainMetaTab) {
        if (ImGui::BeginTabItem(I18n::TrId("tab_metadata"), nullptr, terrainMetaTabFlags)) {
            if (!m_viewerLoaded[FileType::TerrainMeta]) {
                EnsureDataLoaded();
                if (m_loadError.empty()) {
                    if (auto* v = GetViewer(FileType::TerrainMeta))
                        v->LoadData(std::vector<uint8_t>(m_fileData), m_currentName);
                }
                m_viewerLoaded[FileType::TerrainMeta] = true;
            }
            if (!m_loadError.empty())
                showLoadError();
            else {
                if (auto* v = GetViewer(FileType::TerrainMeta)) {
                    v->Render();
                    if (v->HasPendingNavigation()) {
                        pendingNavigatePath = v->TakePendingNavigation();
                        pendingNavigateRow = -1;
                    }
                }
            }
            ImGui::EndTabItem();
        }
    }

    // ------------------------------------------------------------------
    // Material Table tab
    // ------------------------------------------------------------------
    if (showMatTableTab) {
        if (ImGui::BeginTabItem(I18n::TrId("tab_materials"), nullptr, matTableTabFlags)) {
            if (!m_viewerLoaded[FileType::MatTable]) {
                EnsureDataLoaded();
                if (m_loadError.empty()) {
                    if (auto* v = GetViewer(FileType::MatTable))
                        v->LoadData(std::vector<uint8_t>(m_fileData), m_currentName);
                }
                m_viewerLoaded[FileType::MatTable] = true;
            }
            if (!m_loadError.empty())
                showLoadError();
            else {
                if (auto* v = GetViewer(FileType::MatTable))
                    v->Render();
            }
            ImGui::EndTabItem();
        }
    }

    // ------------------------------------------------------------------
    // Skill Tree tab (Visual) + sibling Metadata tab for PSG files
    // ------------------------------------------------------------------
    if (showSkillTreeTab) {
        // Both tabs read Stats and PassiveSkills through the schema: they wait for it (game data
        // the host may still be downloading) and load once it is there (SetDatSchema reopens them).
        if (ImGui::BeginTabItem(I18n::TrId("tab_skill_tree"), nullptr, skillTreeTabFlags)) {
            if (!SchemaReady()) {
                SchemaNotReadyLine(m_schema ? m_schema->GetError() : std::string());
            } else {
                if (!m_viewerLoaded[FileType::SkillTree]) {
                    EnsureDataLoaded();
                    if (m_loadError.empty()) {
                        if (auto* v = GetViewer(FileType::SkillTree))
                            v->LoadData(std::vector<uint8_t>(m_fileData), m_currentName);
                    }
                    m_viewerLoaded[FileType::SkillTree] = true;
                }
                if (!m_loadError.empty())
                    showLoadError();
                else {
                    if (auto* v = GetViewer(FileType::SkillTree)) {
                        if (auto* psg = dynamic_cast<PsgViewer*>(v)) psg->RenderVisual();
                        else v->Render();
                    }
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(I18n::TrId("tab_metadata"))) {
            if (!SchemaReady()) {
                SchemaNotReadyLine(m_schema ? m_schema->GetError() : std::string());
            } else {
                // Shares the Skill Tree load flag — no separate EnsureDataLoaded.
                if (!m_viewerLoaded[FileType::SkillTree]) {
                    EnsureDataLoaded();
                    if (m_loadError.empty()) {
                        if (auto* v = GetViewer(FileType::SkillTree))
                            v->LoadData(std::vector<uint8_t>(m_fileData), m_currentName);
                    }
                    m_viewerLoaded[FileType::SkillTree] = true;
                }
                if (!m_loadError.empty())
                    showLoadError();
                else {
                    if (auto* v = GetViewer(FileType::SkillTree)) {
                        if (auto* psg = dynamic_cast<PsgViewer*>(v)) psg->RenderMetadata();
                    }
                }
            }
            ImGui::EndTabItem();
        }
    }

    // ------------------------------------------------------------------
    // Hex tab — always present
    // ------------------------------------------------------------------
    if (ImGui::BeginTabItem(I18n::TrId("tab_hex"), nullptr, hexTabFlags)) {
        EnsureDataLoaded();
        if (!m_loadError.empty()) {
            showLoadError();
        } else {
            if (auto* hexV = GetViewer(FileType::Binary)) {
                if (!hexV->IsLoaded()) {
                    hexV->LoadData(std::vector<uint8_t>(m_fileData), m_currentName);
                }
                hexV->Render();
            }
        }
        ImGui::EndTabItem();
    }

    // ------------------------------------------------------------------
    // Properties tab — always present
    // ------------------------------------------------------------------
    if (ImGui::BeginTabItem(I18n::TrId("tab_properties"))) {
        RenderPropertiesViewer(m_properties);
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();

    // ---- Loading spinner overlay ----
    if (m_isLoading) {
        ImVec2 winPos = ImGui::GetWindowPos();
        ImVec2 winSize = ImGui::GetWindowSize();
        ImVec2 center(winPos.x + winSize.x * 0.5f, winPos.y + winSize.y * 0.5f);
        float radius = 20.0f;
        float t = (float)ImGui::GetTime();
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(winPos, ImVec2(winPos.x + winSize.x, winPos.y + winSize.y),
            IM_COL32(0, 0, 0, 120));
        int segments = 12;
        for (int i = 0; i < segments; ++i) {
            float angle = t * 5.0f + (float)i * 3.14159265f * 2.0f / (float)segments;
            float alpha = (float)(segments - i) / (float)segments;
            ImVec2 p(center.x + cosf(angle) * radius, center.y + sinf(angle) * radius);
            drawList->AddCircleFilled(p, 3.0f, IM_COL32(255, 255, 255, (int)(alpha * 255)));
        }
        const char* loadingText = I18n::Tr("status_loading");
        ImVec2 textSize = ImGui::CalcTextSize(loadingText);
        drawList->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y + radius + 10.0f),
            IM_COL32(255, 255, 255, 255), loadingText);
    }
}

void ContentPanel::ScrollToLine(int line) {
    m_deferredScrollLine = line;
    // If text is already loaded, apply immediately
    if (m_viewerLoaded[FileType::Text]) {
        if (auto* tv = GetTextViewer())
            tv->ScrollToLine(line);
    }
}

void ContentPanel::ScrollToRow(int row) {
    m_deferredScrollRow = row;
    // If dat is already loaded, apply immediately
    if (m_viewerLoaded[FileType::Dat]) {
        if (auto* dv = GetDatViewer())
            dv->ScrollToRow(row);
    }
}

} // namespace GGPKExp
