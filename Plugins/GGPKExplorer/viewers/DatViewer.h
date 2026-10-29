#pragma once
#include "IViewer.h"
#include "CellEditorPanel.h"
#include "../core/DatParser.h"
#include "../core/DatSchema.h"
#include "../core/DatSearcher.h"
#include "../core/DatImporter.h"
#include "../core/AsyncTaskRunner.h"
#include "../core/Result.h"
#include "../core/VirtualTree.h"
#include "../lib/json.hpp"
#include <string>
#include <vector>
#include <set>
#include <cstdint>
#include <functional>
#include <mutex>
#include <future>
#include <atomic>
#include <unordered_map>

namespace GGPKExp { class GGPKManager; }

namespace GGPKExp {

// Encapsulates inline cell editing state for the DAT table
struct CellEditState {
    int row = -1;
    int col = -1;
    char buf[512] = {};
    bool active = false;

    void Begin(int r, int c, const std::string& value) {
        row = r; col = c; active = true;
        strncpy_s(buf, value.c_str(), sizeof(buf) - 1);
    }
    void Cancel() { row = -1; col = -1; active = false; buf[0] = '\0'; }
    bool IsEditing(int r, int c) const { return active && row == r && col == c; }
};

// Async decode progress states
enum class DecodeState { Idle, Decoding, Ready, Error };

class DatViewer : public IViewer {
public:
    void SetSchema(DatSchema* schema) { m_schema = schema; }
    void SetTaskRunner(AsyncTaskRunner* runner) { m_taskRunner = runner; }
    void SetTree(VirtualTree* tree) { m_tree = tree; }
    void SetGGPKManager(GGPKManager* mgr) { m_ggpkManager = mgr; }

    // Phase 5 / T15 — read-only accessors used by editor sub-panels
    // (e.g. ForeignRowEditor async-loads the target DAT via these).
    DatSchema*   GetSchema() const       { return m_schema; }
    VirtualTree* GetTree() const         { return m_tree; }
    GGPKManager* GetGGPKManager() const  { return m_ggpkManager; }
    void SetOnSave(std::function<void(const std::vector<uint8_t>&)> cb) { m_onSave = std::move(cb); }
    // Phase 7: tell the viewer whether the host atlas has FontAwesome6 merged.
    // Fed from GGPKExplorer::IsFA6Available() via ContentPanel.
    void SetFA6Available(bool available) { m_fa6Available = available; }
    // Phase 7 / T22: editor sub-panels (ArrayEditor, FileRefEditor, ...) read
    // this via their m_owner pointer to pick FA6 glyphs vs text fallbacks.
    bool IsFA6Available() const { return m_fa6Available; }

    // Phase 7 / T23: CellEditorPanel.width is initialised on first open to
    // kDefaultWidth; the plugin pushes the persisted value in here at viewer
    // creation time, and reads the current value on plugin shutdown.
    void SetDatEditorPanelWidth(float w) { m_editor.width = w; }
    float GetDatEditorPanelWidth() const { return m_editor.width; }
    void LoadData(std::vector<uint8_t> data, const std::string& path) override;
    void Clear() override;
    void Render() override;
    void RenderSchemaTab();
    bool HasChanges() const { return !m_modifiedCells.empty(); }
    // Unsaved cell edits, an applied or a previewed import: reloading the table would lose them.
    bool HasPendingEdits() const { return !m_modifiedCells.empty() || m_importMode || m_importConfirmOpen; }

    // Foreign key navigation: set by click, consumed by ContentPanel
    std::string pendingNavPath;
    int pendingNavRow = -1;
    void ScrollToRow(int row);

    // IViewer navigation overrides
    bool HasPendingNavigation() const override { return !pendingNavPath.empty(); }
    std::string TakePendingNavigation() override {
        std::string p = std::move(pendingNavPath);
        pendingNavPath.clear();
        return p;
    }
    int TakePendingNavRow() override {
        int r = pendingNavRow;
        pendingNavRow = -1;
        return r;
    }

    // Phase 4 / T11-T12 — accessors used by the right-side CellEditorPanel to
    // read/write a single cell via the public contract rather than reaching
    // directly into m_rows.
    void ApplyCellEdit(int row, int col, const CellValue& v) {
        if (static_cast<size_t>(row) >= m_rows.size()) return;
        if (static_cast<size_t>(col) >= m_rows[static_cast<size_t>(row)].size()) return;
        m_rows[static_cast<size_t>(row)][static_cast<size_t>(col)] = v;
        uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(row)) << 32)
                     | static_cast<uint32_t>(col);
        m_modifiedCells.insert(key);
    }
    CellValue GetCellValue(int row, int col) const {
        if (static_cast<size_t>(row) >= m_rows.size()) return std::monostate{};
        if (static_cast<size_t>(col) >= m_rows[static_cast<size_t>(row)].size()) return std::monostate{};
        return m_rows[static_cast<size_t>(row)][static_cast<size_t>(col)];
    }
    // Reopens the panel against (row,col), picking the appropriate Mode
    // from the column schema (Array/ForeignRow/FileRef/Primitive).
    void RebindEditorTo(int row, int col);

private:
    DatSchema* m_schema = nullptr;              // non-owning, lifetime guaranteed by GGPKExplorer
    AsyncTaskRunner* m_taskRunner = nullptr;   // non-owning, lifetime guaranteed by GGPKExplorer
    VirtualTree* m_tree = nullptr;              // non-owning, for path hyperlink validation
    bool m_fa6Available = false;                // Phase 7 gate: FA6 icon glyphs present in atlas
    std::function<void(const std::vector<uint8_t>&)> m_onSave;
    DatFile m_dat;
    const SchemaTable* m_table = nullptr;
    std::string m_tableName;
    std::vector<std::vector<CellValue>> m_rows;
    std::set<uint64_t> m_modifiedCells;

    // Inline edit state
    CellEditState m_editState;
    std::vector<float> m_columnWidths;
    int m_pendingScrollRow = -1;

    // Sorting state
    int m_sortColumn = -1;         // -1 = sort by row index (default), 0..N = data column
    bool m_sortAscending = true;
    std::vector<int> m_sortedIndices; // maps display row -> original row index
    void RebuildSortOrder();

    // Row highlight (blink for 3 seconds after navigation)
    int m_highlightRow = -1;
    float m_highlightTimer = 0.0f;

    // Async decode state
    DecodeState m_decodeState = DecodeState::Idle;
    std::atomic<float> m_decodeProgress{0.0f};
    std::string m_decodeStatus;
    std::mutex m_decodeStatusMutex;    // protects m_decodeStatus during decode
    std::future<Result<void>> m_decodeFuture;

    static bool IsHashColumn(const std::string& colName) {
        std::string lower;
        lower.reserve(colName.size());
        for (unsigned char c : colName)
            lower.push_back(static_cast<char>(std::tolower(c)));
        return lower.find("hash") != std::string::npos;
    }

    // Phase 2 / T5: per-cell array expand/collapse state (key = (row<<32)|col).
    // Cleared on LoadData so state does not leak across file switches.
    std::unordered_map<uint64_t, bool> m_expandedArrays;

    // Phase 3 / T7: selected cell (single-click selection; double-click begins edit).
    struct SelectedCell { int row = -1; int col = -1; };
    SelectedCell m_selectedCell;

    // Phase 4 / T11-T12: right-side cell editor panel + one-time owner wiring.
    CellEditorPanel m_editor;
    bool m_editor_OwnerWasSet = false;

    void RenderTable();
    void DecodeAllRows();
    void FinalizeAfterDecode();
    void ComputeColumnWidths();
    std::string CellToString(const CellValue& val, const ColumnDef* col = nullptr) const;
    void RenderCellArray(const CellArray& arr, const ColumnDef* col, int row, int cIdx);
    void RenderArrayItem(const CellValue& item, const ColumnDef* col, int row, int cIdx, int itemIdx);
    void ExportCSV(const std::string& path) const;
    void ExportJSON(const std::string& path) const;

    // Phase 3 / T8-T9: cell context menu + clipboard helpers.
    void RenderCellContextMenu(int row, int col, const CellValue& val, const ColumnDef& colDef);
    void CopyCellToClipboard(const CellValue& val, const ColumnDef& colDef);
    bool PasteCellFromClipboard(int row, int col, const ColumnDef& colDef);
    void ClearCell(int row, int col, const ColumnDef& colDef);
    void RevertCell(int row, int col);
    void CopyRowAsJson(int row);            // T9
    void OpenFindReferencesModal(int row);  // T10
    void RenderFindReferencesModal();       // T10

    // Shared JSON conversion: used by ExportJSON and CopyRowAsJson (T9 refactor).
    nlohmann::json CellValueToJson(const CellValue& v, bool isHash) const;

    // Diagnostic tooltip for red-✗ missing file-ref cells. Shows query/lowered
    // path + tree index size + nearby keys with the same prefix so users can
    // report the exact lookup failure without needing a Debug build.
    void RenderMissingFileRefTooltip(const std::string& s, const char* schemaHint);

    // T10: Find all references modal state.
    GGPKManager* m_ggpkManager = nullptr;  // non-owning, set by ContentPanel
    bool m_findRefsModalOpen = false;
    bool m_findRefsOpenPopupRequested = false;  // sentinel to call OpenPopup once
    std::vector<GGPKExp::RowReference> m_findRefsResults;
    std::atomic<bool> m_findRefsCancel{false};
    std::atomic<size_t> m_findRefsCurrent{0};
    std::atomic<size_t> m_findRefsTotal{0};
    std::future<void> m_findRefsFuture;
    int m_findRefsTargetRow = -1;
    std::string m_findRefsTargetTable;

    // T21 Import state
    bool m_importMode = false;                  // set after successful Apply → Save uses Serialize
    std::vector<std::string> m_importLog;       // last import's warnings (for viewer)
    bool m_importConfirmOpen = false;           // "Import preview" modal
    bool m_importConfirmOpenRequested = false;  // sentinel
    ImportResult m_pendingImport;               // parsed result awaiting Apply
    std::string m_pendingImportFormat;          // "csv" or "json" for display
    bool m_importShowLog = false;               // toggle warning log visibility in modal

    void RenderImportButtons();    // Import CSV/JSON buttons
    void RenderImportModal();      // confirm dialog after parse
    void ApplyPendingImport();     // user clicked Apply in modal
};

} // namespace GGPKExp
