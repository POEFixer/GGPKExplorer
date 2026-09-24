#pragma once
#include "../core/DatParser.h"
#include "../core/DatSchema.h"
#include "editors/GgpkTreePicker.h"
#include <future>
#include <string>
#include <utility>
#include <vector>

namespace GGPKExp {

class DatViewer; // forward

// -----------------------------------------------------------------------------
// Phase 4 / T11-T12 — right-side cell-editor panel.
//
// Lives as an optional right-hand pane in DatViewer. When closed, occupies
// zero space. When open, user can drag the splitter to resize within
// [kMinWidth, kMaxWidth]. The panel dispatches to one of four editor modes:
// Primitive, Array, ForeignRow, FileRef. T11 renders stubs per mode; T13/T14
// will fill in the actual editors.
//
// Sticky selection: when a cell is selected while another cell's buffer is
// dirty, PromptDirtyConfirm() surfaces a modal with Apply/Discard/Stay. The
// modal is rendered at window scope (outside the child region) so ImGui
// popups work correctly.
// -----------------------------------------------------------------------------
class CellEditorPanel {
public:
    enum class Mode { Closed, Primitive, Array, ForeignRow, FileRef };

    void SetOwner(DatViewer* owner) { m_owner = owner; }
    bool IsOpen() const { return m_mode != Mode::Closed; }
    void Open(int row, int col, Mode mode, const CellValue& initial, const ColumnDef& colDef);
    void Close();

    // Called every frame by DatViewer::Render inside the right-hand pane.
    // availWidth is the pane's width (positive) from the splitter to screen edge.
    void Render(float availWidth);

    // Panel width (persisted in settings in Phase 7, for now in-memory only)
    float width = 400.0f;
    static constexpr float kMinWidth = 300.0f;
    static constexpr float kMaxWidth = 800.0f;

    bool IsDirty() const { return m_dirty; }

    // Called when user picks another cell and buffer is dirty.
    // Returns true if selection may proceed immediately (clean buffer).
    // Returns false if caller should defer — the confirm modal will appear
    // next frame to let the user pick Apply/Discard/Stay.
    bool PromptDirtyConfirm(int newRow, int newCol);

    // Which (row,col) is the panel currently editing. Returns -1/-1 if closed.
    int EditingRow() const { return m_row; }
    int EditingCol() const { return m_col; }

private:
    DatViewer* m_owner = nullptr;
    Mode m_mode = Mode::Closed;
    int m_row = -1;
    int m_col = -1;
    const ColumnDef* m_colDef = nullptr;

    CellValue m_buffer;
    bool m_dirty = false;

    // Sticky-selection dirty-switch state
    bool m_switchConfirmOpen = false;
    int m_pendingSwitchRow = -1;
    int m_pendingSwitchCol = -1;

    // T11 renders these as stubs; T13/T14 fill them in per-mode.
    void RenderPrimitive();
    void RenderArray();
    void RenderForeignRow();
    void RenderFileRef();

    void Apply();
    void Cancel();

    // Phase 5 / T15 — ForeignRow editor state (async target-table load).
    // The future resolves on the worker thread; the render loop polls it each
    // frame and commits the result into m_targetRows/m_targetTable. Reload is
    // triggered whenever m_colDef->refTable differs from m_targetLastTable.
    std::future<std::pair<std::vector<std::vector<CellValue>>, const SchemaTable*>> m_targetLoadFuture;
    std::vector<std::vector<CellValue>> m_targetRows;
    const SchemaTable* m_targetTable = nullptr;
    bool m_targetLoading = false;
    std::string m_targetSearchQuery;
    std::string m_targetLastTable; // detects refTable change across re-binds → triggers reload

    // Phase 5 / T16 — FileRef editor state.
    // m_fileRefBuf is the InputText buffer; m_fileRefCompletions is the
    // autocomplete dropdown (≤10). Live-validation flag m_fileRefExists is
    // refreshed alongside the throttled rescan (m_fileRefNeedsRescan). Bound
    // row/col detect re-bind → reset. m_fileRefPicker is the T17 modal; its
    // Render() is called OUTSIDE the BeginChild from CellEditorPanel::Render.
    char m_fileRefBuf[2048] = {};
    std::vector<std::string> m_fileRefCompletions;
    double m_fileRefLastTypeTime = 0.0;
    bool m_fileRefExists = false;
    int m_fileRefLastBoundRow = -1;
    int m_fileRefLastBoundCol = -1;
    bool m_fileRefNeedsRescan = false;
    GgpkTreePicker m_fileRefPicker;
};

} // namespace GGPKExp
