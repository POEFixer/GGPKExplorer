#include "CellEditorPanel.h"
#include "DatViewer.h"
#include "editors/IconHelpers.h"
#include <imgui.h>
#include <string>

namespace GGPKExp {

// -----------------------------------------------------------------------------
// Open / Close
// -----------------------------------------------------------------------------

void CellEditorPanel::Open(int row, int col, Mode mode, const CellValue& initial, const ColumnDef& colDef)
{
    m_row = row;
    m_col = col;
    m_mode = mode;
    m_buffer = initial;
    m_colDef = &colDef;
    m_dirty = false;
    m_switchConfirmOpen = false;
    m_pendingSwitchRow = -1;
    m_pendingSwitchCol = -1;
}

void CellEditorPanel::Close()
{
    // Phase 5 / T15 — wait on any in-flight target-table load before dropping
    // state; prevents the async worker from writing into a half-destroyed
    // object if Close() races with a pending load.
    if (m_targetLoadFuture.valid()) {
        m_targetLoadFuture.wait();
        // Drain the result so the future is in a non-valid state afterwards.
        (void)m_targetLoadFuture.get();
    }

    m_mode = Mode::Closed;
    m_row = -1;
    m_col = -1;
    m_colDef = nullptr;
    m_buffer = std::monostate{};
    m_dirty = false;
    m_switchConfirmOpen = false;
    m_pendingSwitchRow = -1;
    m_pendingSwitchCol = -1;

    m_targetRows.clear();
    m_targetTable = nullptr;
    m_targetLoading = false;
    m_targetLastTable.clear();
    m_targetSearchQuery.clear();

    // Phase 5 / T16 — FileRef cleanup. The picker modal has no explicit close
    // API; if it was open it dismisses naturally when the user hits Cancel or
    // clicks outside — but since Close() destroys the editor, we just drop
    // our transient state. If the picker is mid-render we let ImGui's popup
    // stack handle it; next frame Render() won't recurse into us.
    m_fileRefBuf[0] = '\0';
    m_fileRefCompletions.clear();
    m_fileRefExists = false;
    m_fileRefLastBoundRow = -1;
    m_fileRefLastBoundCol = -1;
    m_fileRefNeedsRescan = false;
}

// -----------------------------------------------------------------------------
// Apply / Cancel — Apply pushes the buffered edit back through the owner;
// Cancel re-reads the current on-data value and clears the dirty flag.
// -----------------------------------------------------------------------------

void CellEditorPanel::Apply()
{
    if (!m_owner) { m_dirty = false; return; }
    m_owner->ApplyCellEdit(m_row, m_col, m_buffer);
    m_dirty = false;
}

void CellEditorPanel::Cancel()
{
    if (!m_owner) { m_dirty = false; return; }
    m_buffer = m_owner->GetCellValue(m_row, m_col);
    m_dirty = false;
}

// -----------------------------------------------------------------------------
// PromptDirtyConfirm — returns true when selection may proceed immediately
// (buffer is clean). Returns false when a confirm modal must be shown next
// frame — caller should defer RebindEditorTo until user picks Apply/Discard.
// -----------------------------------------------------------------------------

bool CellEditorPanel::PromptDirtyConfirm(int newRow, int newCol)
{
    if (!m_dirty) return true;
    m_pendingSwitchRow = newRow;
    m_pendingSwitchCol = newCol;
    m_switchConfirmOpen = true;
    return false;
}

// -----------------------------------------------------------------------------
// Per-mode renderers.
//   - RenderPrimitive  — editors/PrimitiveEditor.cpp  (T13)
//   - RenderArray      — editors/ArrayEditor.cpp      (T14)
//   - RenderForeignRow — editors/ForeignRowEditor.cpp (T15)
//   - RenderFileRef    — editors/FileRefEditor.cpp    (T16)
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Render — draws the right-side pane plus the dirty-switch confirm modal.
// The modal is opened OUTSIDE the BeginChild region because ImGui popups live
// at window scope and cannot be opened inside a child.
// -----------------------------------------------------------------------------

void CellEditorPanel::Render(float availWidth)
{
    if (m_mode == Mode::Closed) return;

    // ---- 1. Child region for panel content ----
    ImGui::BeginChild("##CellEditor", ImVec2(availWidth, 0), true);

    // Header: "Row <r> | Col: <name> (<typeStr>)" + close button on the right.
    std::string colName;
    std::string typeStr;
    if (m_colDef) {
        colName = m_colDef->name.empty()
            ? ("Col " + std::to_string(m_colDef->index))
            : m_colDef->name;
        if (!m_colDef->refTable.empty()) {
            typeStr = m_colDef->refTable;
        } else {
            switch (m_colDef->type) {
            case ColumnType::Bool:       typeStr = "bool";     break;
            case ColumnType::I16:        typeStr = "i16";      break;
            case ColumnType::U16:        typeStr = "u16";      break;
            case ColumnType::I32:        typeStr = "i32";      break;
            case ColumnType::U8:         typeStr = "u8";       break;
            case ColumnType::U32:        typeStr = "u32";      break;
            case ColumnType::F32:        typeStr = "f32";      break;
            case ColumnType::String:     typeStr = "string";   break;
            case ColumnType::Row:        typeStr = "row";      break;
            case ColumnType::ForeignRow: typeStr = "rid";      break;
            case ColumnType::EnumRow:    typeStr = "enum";     break;
            default:                     typeStr = "?";        break;
            }
        }
        if (m_colDef->isArray)    typeStr = "array<" + typeStr + ">";
        if (m_colDef->isInterval) typeStr += " interval";
    } else {
        colName = "?";
        typeStr = "?";
    }

    ImGui::Text("Row %d | Col: %s (%s)", m_row, colName.c_str(), typeStr.c_str());

    // Close button (FA6 xmark icon when available, ">>" text fallback otherwise).
    const bool fa6 = m_owner && m_owner->IsFA6Available();
    const std::string closeLbl = fa6 ? std::string(kIconClose) : std::string(">>");
    const float closeBtnW = ImGui::CalcTextSize(closeLbl.c_str()).x
                          + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - closeBtnW + ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button(closeLbl.c_str())) {
        Close();
        ImGui::EndChild();
        return;
    }

    ImGui::Separator();

    // ---- 2. Body — per-mode renderer ----
    switch (m_mode) {
    case Mode::Primitive:  RenderPrimitive();  break;
    case Mode::Array:      RenderArray();      break;
    case Mode::ForeignRow: RenderForeignRow(); break;
    case Mode::FileRef:    RenderFileRef();    break;
    case Mode::Closed:     break;
    }

    ImGui::Separator();

    // ---- 3. Apply / Cancel buttons (bottom) ----
    // Cancel stays text-only: "Cancel" is common enough that the icon would be
    // noise; the same glyph could also confuse with the panel's Close button.
    const std::string lblApply = IconLabel(fa6, kIconOk, "Apply");
    if (ImGui::Button(lblApply.c_str())) {
        Apply();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        Cancel();
    }
    if (m_dirty) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f), "(modified)");
    }

    ImGui::EndChild();

    // ---- 4a. FileRef picker modal (T17) — must be rendered at window scope,
    //          not inside the BeginChild above. RenderFileRef stores picker
    //          state; we draw the modal here so OpenPopup/BeginPopupModal live
    //          on the window's ID stack rather than the child's.
    m_fileRefPicker.Render();

    // ---- 4. Dirty-switch confirm modal (outside child; window-scope) ----
    if (m_switchConfirmOpen) {
        ImGui::OpenPopup("Unsaved changes");
        m_switchConfirmOpen = false;
    }
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Cell has unsaved edits. Switch to another cell?");
        ImGui::Separator();
        if (ImGui::Button("Apply and switch")) {
            Apply();
            if (m_owner) m_owner->RebindEditorTo(m_pendingSwitchRow, m_pendingSwitchCol);
            m_pendingSwitchRow = -1;
            m_pendingSwitchCol = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard and switch")) {
            m_dirty = false;
            if (m_owner) m_owner->RebindEditorTo(m_pendingSwitchRow, m_pendingSwitchCol);
            m_pendingSwitchRow = -1;
            m_pendingSwitchCol = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Stay")) {
            m_pendingSwitchRow = -1;
            m_pendingSwitchCol = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace GGPKExp
