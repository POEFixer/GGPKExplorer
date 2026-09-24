#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <commdlg.h>

#include "DatViewer.h"
#include "../core/GGPKManager.h"
#include "../core/DatExport.h"
#include "../core/PathUtils.h"
#include "../lib/json.hpp"
#include "../../../POEFixer/imgui/IconsFontAwesome6.h"
#include "editors/IconHelpers.h"
#ifdef _DEBUG
#include "../dev/DatRoundTripTest.h"
#endif
#include <imgui.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <functional>

namespace GGPKExp {

namespace {

// Heuristic: does a string look like a GGPK file path (contains '/' and ends
// with a plausible alphanumeric extension)? Rendered as a hyperlink when the
// tree confirms it resolves to a real GGPK entry (tree lookup is the real
// filter; this heuristic just screens out obvious non-paths).
bool IsLikelyGGPKPath(const std::string& s) {
    if (s.empty() || s.size() > 512) return false;
    if (s.find('/') == std::string::npos) return false;
    auto dot = s.rfind('.');
    if (dot == std::string::npos || dot == s.size() - 1) return false;
    // Extension must be 2-8 chars, alphanumeric (letters + digits)
    size_t extLen = s.size() - dot - 1;
    if (extLen < 2 || extLen > 8) return false;
    for (size_t i = dot + 1; i < s.size(); ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return false;
    }
    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// LoadData
// ---------------------------------------------------------------------------

void DatViewer::LoadData(std::vector<uint8_t> data, const std::string& path)
{
    Clear();
    // Phase 2 / T5: drop any per-cell array expand/collapse state from the
    // previous file so it can't bleed across loads.
    m_expandedArrays.clear();

    m_tableName = ExtractTableName(path);
    m_dat = Parse(data);

    if (!m_dat.valid) {
        m_error = m_dat.error.empty() ? "Failed to parse DAT file" : m_dat.error;
        return;
    }

    m_table = m_schema ? m_schema->FindTable(m_tableName) : nullptr;

    if (!m_table) {
        m_error = "No schema for: " + m_tableName;
        // Note: we still have m_dat.valid == true, so hex fallback message is shown below
        return;
    }

    if (m_taskRunner) {
        // Async decode path
        m_decodeState = DecodeState::Decoding;
        m_decodeFuture = m_taskRunner->Submit(
            [this](std::atomic<bool>& cancelFlag, AsyncTaskRunner::ProgressCallback progress) -> Result<void> {
                const uint32_t totalRows = m_dat.rowCount;
                std::vector<std::vector<CellValue>> decodedRows;
                decodedRows.reserve(totalRows);

                for (uint32_t rowIdx = 0; rowIdx < totalRows; ++rowIdx) {
                    if (cancelFlag.load(std::memory_order_relaxed))
                        return Result<void>::Fail("Cancelled");

                    std::vector<CellValue> row;
                    row.reserve(m_table->columns.size());

                    uint32_t columnOffset = 0;
                    for (const auto& col : m_table->columns) {
                        row.push_back(DecodeCell(m_dat, rowIdx, columnOffset, col));
                        columnOffset += static_cast<uint32_t>(GetColumnByteSize(col));
                    }
                    decodedRows.push_back(std::move(row));

                    if (rowIdx % 1000 == 0) {
                        float pct = static_cast<float>(rowIdx) / static_cast<float>(totalRows);
                        m_decodeProgress.store(pct, std::memory_order_relaxed);
                        {
                            std::lock_guard lock(m_decodeStatusMutex);
                            m_decodeStatus = "Decoding row " + std::to_string(rowIdx) + "/" + std::to_string(totalRows);
                        }
                        progress(pct, "");
                    }
                }

                m_rows = std::move(decodedRows);
                m_decodeProgress.store(1.0f, std::memory_order_relaxed);
                return Result<void>::Ok();
            });
        m_loaded = true;
    } else {
        // Synchronous decode fallback
        DecodeAllRows();
        FinalizeAfterDecode();
    }
}

// ---------------------------------------------------------------------------
// DecodeAllRows
// ---------------------------------------------------------------------------

void DatViewer::DecodeAllRows()
{
    m_rows.clear();
    if (!m_table || !m_dat.valid)
        return;

    m_rows.reserve(m_dat.rowCount);

    for (uint32_t rowIdx = 0; rowIdx < m_dat.rowCount; ++rowIdx) {
        std::vector<CellValue> row;
        row.reserve(m_table->columns.size());

        uint32_t columnOffset = 0;
        for (const auto& col : m_table->columns) {
            row.push_back(DecodeCell(m_dat, rowIdx, columnOffset, col));
            columnOffset += static_cast<uint32_t>(GetColumnByteSize(col));
        }

        m_rows.push_back(std::move(row));
    }
    m_decodeState = DecodeState::Ready;
}

// ---------------------------------------------------------------------------
// CellToString
// ---------------------------------------------------------------------------

std::string DatViewer::CellToString(const CellValue& val, const ColumnDef* col) const
{
    return DatExport::CellToString(val, col);
}

// ---------------------------------------------------------------------------
// RenderArrayItem — render a single array element at (row, cIdx, itemIdx).
// Handles three cases (order preserved from the legacy RenderCellArray):
//   1. ForeignRow element (int64_t + col->refTable non-empty) → clickable
//      hyperlink that sets pendingNavPath/pendingNavRow (plain click, mirrors
//      the top-level foreign-row UX).
//   2. String element → path hyperlink when the tree resolves it (Ctrl+Click
//      navigates). Strings on a schema-declared file-ref column that do NOT
//      resolve are rendered red with a warning prefix (T6).
//   3. Everything else → primitive render via CellToString.
// ---------------------------------------------------------------------------

void DatViewer::RenderArrayItem(const CellValue& item, const ColumnDef* col, int row, int cIdx, int itemIdx)
{
    // Foreign-row reference inside an array is stored as int64_t (see
    // DecodeCell: ForeignRow/Row array elements decoded as int64_t).
    if (std::holds_alternative<int64_t>(item) && col && !col->refTable.empty()) {
        int64_t rowRef = std::get<int64_t>(item);
        if (rowRef >= 0) {
            std::string label = std::to_string(rowRef);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.4f, 0.8f, 1.0f, 0.15f));
            char linkId[256];
            snprintf(linkId, sizeof(linkId), "%s##fkarr_%d_%d_%d",
                     label.c_str(), row, cIdx, itemIdx);
            if (ImGui::Selectable(linkId, false, ImGuiSelectableFlags_None,
                                  ImVec2(ImGui::CalcTextSize(label.c_str()).x, 0)))
            {
                pendingNavPath = col->refTable + ".datc64";
                pendingNavRow = static_cast<int>(rowRef);
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("Navigate to %s row %lld",
                                  col->refTable.c_str(), (long long)rowRef);
            }
            return;
        }
        // rowRef < 0 falls through to primitive render ("-1" / "<null>")
    }

    if (std::holds_alternative<std::string>(item)) {
        const auto& s = std::get<std::string>(item);
        const bool hasSchemaFileRef = col && (!col->refFile.empty() || !col->refFiles.empty());
        const bool isHeuristicPath = IsLikelyGGPKPath(s);

        if (hasSchemaFileRef || isHeuristicPath) {
            // Existence check goes through GGPKManager — it covers BOTH loose
            // GGPK files and bundle2 contents (via the path cache built on
            // first call). VirtualTree only sees loose files, which is why
            // bundle-contained paths (.dds / .aoc / .mesh) were falsely
            // flagged red pre-fix. FindNodeByPath is case-insensitive.
            const bool exists = m_ggpkManager && (m_ggpkManager->FindNodeByPath(s) != nullptr);
            if (exists) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", s.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("Ctrl+Click to navigate to %s", s.c_str());
                }
                if (ImGui::IsItemClicked() && ImGui::GetIO().KeyCtrl) {
                    pendingNavPath = s;
                    pendingNavRow  = -1;
                }
                return;
            }
            if (hasSchemaFileRef) {
                // Schema declares file-ref but GGPK lookup failed → red warn.
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                const char* prefix = m_fa6Available ? ICON_FA_TRIANGLE_EXCLAMATION " " : "[!] ";
                ImGui::Text("%s%s", prefix, s.c_str());
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    const char* hint = !col->refFile.empty()
                        ? col->refFile.c_str()
                        : (!col->refFiles.empty() ? col->refFiles.front().c_str() : "file reference");
                    RenderMissingFileRefTooltip(s, hint);
                }
                return;
            }
            // Heuristic match but not in GGPK → plain text (backward-compat).
        }
        ImGui::TextUnformatted(s.c_str());
        return;
    }

    // Primitive (int, float, bool, monostate, etc.) — render via CellToString.
    ImGui::TextUnformatted(CellToString(item, col).c_str());
}

// ---------------------------------------------------------------------------
// RenderCellArray — inline rendering of array cells with per-cell
// expand/collapse state (T5). Collapsed: toggle + up to 5 items + ", ...+N".
// Expanded: toggle + all items. State persists in m_expandedArrays until a
// new file is loaded. A tooltip with the full list is still offered on the
// collapsed summary (preserves legacy UX).
// ---------------------------------------------------------------------------

void DatViewer::RenderCellArray(const CellArray& arr, const ColumnDef* col, int row, int cIdx)
{
    if (arr.items.empty()) {
        ImGui::TextDisabled("[]");
        return;
    }

    const uint64_t key =
        (static_cast<uint64_t>(static_cast<uint32_t>(row)) << 32)
        | static_cast<uint32_t>(cIdx);
    auto it = m_expandedArrays.find(key);
    const bool expanded = (it != m_expandedArrays.end()) && it->second;

    // Toggle button — FA6 caret glyph if available, ASCII fallback otherwise.
    const char* toggleLabel = expanded
        ? (m_fa6Available ? ICON_FA_CARET_DOWN : "v")
        : (m_fa6Available ? ICON_FA_CARET_RIGHT : ">");
    ImGui::PushID(static_cast<int>(key & 0x7FFFFFFFu));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    if (ImGui::SmallButton(toggleLabel)) {
        m_expandedArrays[key] = !expanded;
    }
    ImGui::PopStyleColor();
    ImGui::PopID();
    ImGui::SameLine(0.0f, 2.0f);

    const size_t total = arr.items.size();
    const size_t kMaxInline = 5;
    const size_t shown = expanded ? total : (std::min)(total, kMaxInline);

    ImGui::TextUnformatted("[");
    ImGui::SameLine(0.0f, 0.0f);

    for (size_t i = 0; i < shown; ++i) {
        if (i > 0) {
            ImGui::TextUnformatted(", ");
            ImGui::SameLine(0.0f, 0.0f);
        }
        RenderArrayItem(arr.items[i], col, row, cIdx, static_cast<int>(i));
        ImGui::SameLine(0.0f, 0.0f);
    }

    bool truncated = false;
    if (!expanded && total > shown) {
        char suffix[32];
        snprintf(suffix, sizeof(suffix), ", ...+%zu", total - shown);
        ImGui::TextDisabled("%s", suffix);
        truncated = true;
        // Preserve legacy tooltip UX: full list on hover while collapsed.
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            for (size_t i = 0; i < arr.items.size(); ++i) {
                ImGui::TextUnformatted(CellToString(arr.items[i], col).c_str());
            }
            ImGui::EndTooltip();
        }
        ImGui::SameLine(0.0f, 0.0f);
    }
    (void)truncated;

    ImGui::TextUnformatted("]");
}

// ---------------------------------------------------------------------------
// ExportCSV
// ---------------------------------------------------------------------------

void DatViewer::ExportCSV(const std::string& path) const
{
    if (!m_table) return;
    DatExport::WriteCsv(PathUtils::FromUtf8(path), m_dat, *m_table, m_rows);
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

void DatViewer::Clear()
{
    // Wait for any pending async decode to finish before clearing state
    if (m_decodeFuture.valid()) {
        m_decodeFuture.wait();
    }
    // T10: cancel + join any in-flight reference scan so the worker cannot
    // touch 'this' after Clear() returns.
    if (m_findRefsFuture.valid()) {
        m_findRefsCancel.store(true);
        m_findRefsFuture.wait();
    }
    m_findRefsResults.clear();
    m_findRefsModalOpen = false;
    m_findRefsOpenPopupRequested = false;
    m_findRefsTargetTable.clear();
    m_findRefsTargetRow = -1;
    m_findRefsCurrent.store(0);
    m_findRefsTotal.store(0);
    m_findRefsCancel.store(false);

    m_decodeState = DecodeState::Idle;
    m_decodeProgress.store(0.0f, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_decodeStatusMutex);
        m_decodeStatus.clear();
    }
    m_dat = DatFile{};
    m_table = nullptr;
    m_tableName.clear();
    m_error.clear();
    m_rows.clear();
    m_modifiedCells.clear();
    m_editState.Cancel();
    m_columnWidths.clear();
    m_sortColumn = -1;
    m_sortAscending = true;
    m_sortedIndices.clear();
    pendingNavPath.clear();
    pendingNavRow = -1;
    m_selectedCell = {};
    // Phase 4 / T11: close the editor panel and reset the owner-set sentinel
    // so the next LoadData() re-wires it (defensive; owner pointer is stable
    // but the sentinel is cheap to reset).
    m_editor.Close();
    m_editor_OwnerWasSet = false;
    m_loaded = false;

    // T21: clear any pending/active Import state so it can't bleed across loads.
    m_importMode = false;
    m_importLog.clear();
    m_pendingImport = {};
    m_importConfirmOpen = false;
    m_importConfirmOpenRequested = false;
    m_importShowLog = false;
    m_pendingImportFormat.clear();
}

// ---------------------------------------------------------------------------
// FinalizeAfterDecode — called after rows are decoded (sync or async)
// ---------------------------------------------------------------------------

void DatViewer::FinalizeAfterDecode()
{
    ComputeColumnWidths();
    RebuildSortOrder();
    m_loaded = true;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void DatViewer::Render()
{
    // Check async decode completion
    if (m_decodeState == DecodeState::Decoding && m_decodeFuture.valid()) {
        if (m_decodeFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto result = m_decodeFuture.get();
            if (result) {
                m_decodeState = DecodeState::Ready;
                FinalizeAfterDecode();
            } else {
                m_decodeState = DecodeState::Error;
                m_error = result.error;
            }
        }
    }

    if (m_decodeState == DecodeState::Decoding) {
        float pct = m_decodeProgress.load(std::memory_order_relaxed);
        ImGui::ProgressBar(pct, ImVec2(-1, 0));
        {
            std::lock_guard lock(m_decodeStatusMutex);
            ImGui::TextUnformatted(m_decodeStatus.c_str());
        }
        return;
    }

    if (m_decodeState == DecodeState::Error) {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Decode error: %s", m_error.c_str());
        return;
    }

    // Case 1: parse error (file itself is corrupt/unreadable)
    if (!m_dat.valid) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Parse error: %s", m_error.c_str());
        return;
    }

    // Info bar
    if (m_table) {
        ImGui::Text("Table: %s | %u rows | %zu columns",
                    m_tableName.c_str(),
                    m_dat.rowCount,
                    m_table->columns.size());
    } else {
        ImGui::Text("Table: %s | %u rows | no schema", m_tableName.c_str(), m_dat.rowCount);
    }

    ImGui::SameLine();

    // Export CSV button (only when schema is available)
    if (m_table) {
        const std::string lblExportCsv = IconLabel(m_fa6Available, kIconExport, "Export CSV");
        if (ImGui::Button(lblExportCsv.c_str())) {
            wchar_t filePath[MAX_PATH] = {};
            // Pre-fill with table name
            std::string defaultName = m_tableName + ".csv";
            for (size_t i = 0; i < defaultName.size() && i < MAX_PATH - 1; ++i)
                filePath[i] = static_cast<wchar_t>(defaultName[i]);

            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFilter = L"CSV Files (*.csv)\0*.csv\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = filePath;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
            ofn.lpstrDefExt = L"csv";

            if (GetSaveFileNameW(&ofn)) {
                int needed = WideCharToMultiByte(CP_UTF8, 0, filePath, -1, nullptr, 0, nullptr, nullptr);
                if (needed > 0) {
                    std::string utf8(static_cast<size_t>(needed - 1), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, filePath, -1, utf8.data(), needed, nullptr, nullptr);
                    ExportCSV(utf8);
                }
            }
        }

        ImGui::SameLine();
        const std::string lblExportJson = IconLabel(m_fa6Available, kIconExport, "Export JSON");
        if (ImGui::Button(lblExportJson.c_str())) {
            wchar_t filePath2[MAX_PATH] = {};
            std::string defaultName2 = m_tableName + ".json";
            for (size_t i = 0; i < defaultName2.size() && i < MAX_PATH - 1; ++i)
                filePath2[i] = static_cast<wchar_t>(defaultName2[i]);

            OPENFILENAMEW ofn2 = {};
            ofn2.lStructSize = sizeof(ofn2);
            ofn2.lpstrFilter = L"JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0";
            ofn2.lpstrFile = filePath2;
            ofn2.nMaxFile = MAX_PATH;
            ofn2.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
            ofn2.lpstrDefExt = L"json";

            if (GetSaveFileNameW(&ofn2)) {
                int needed2 = WideCharToMultiByte(CP_UTF8, 0, filePath2, -1, nullptr, 0, nullptr, nullptr);
                if (needed2 > 0) {
                    std::string utf8_2(static_cast<size_t>(needed2 - 1), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, filePath2, -1, utf8_2.data(), needed2, nullptr, nullptr);
                    ExportJSON(utf8_2);
                }
            }
        }

        // Import CSV
        ImGui::SameLine();
        const std::string lblImportCsv = IconLabel(m_fa6Available, kIconImport, "Import CSV");
        if (ImGui::Button(lblImportCsv.c_str())) {
            wchar_t filePath[MAX_PATH] = {};
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFilter = L"CSV Files (*.csv)\0*.csv\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = filePath;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            ofn.lpstrDefExt = L"csv";
            if (GetOpenFileNameW(&ofn)) {
                int needed = WideCharToMultiByte(CP_UTF8, 0, filePath, -1, nullptr, 0, nullptr, nullptr);
                if (needed > 0) {
                    std::string utf8(static_cast<size_t>(needed - 1), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, filePath, -1, utf8.data(), needed, nullptr, nullptr);
                    m_pendingImport = DatImporter::ImportCSV(utf8, *m_table);
                    m_pendingImportFormat = "csv";
                    m_importConfirmOpen = true;
                    m_importConfirmOpenRequested = true;
                    m_importShowLog = false;
                }
            }
        }

        // Import JSON
        ImGui::SameLine();
        const std::string lblImportJson = IconLabel(m_fa6Available, kIconImport, "Import JSON");
        if (ImGui::Button(lblImportJson.c_str())) {
            wchar_t filePath[MAX_PATH] = {};
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFilter = L"JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = filePath;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            ofn.lpstrDefExt = L"json";
            if (GetOpenFileNameW(&ofn)) {
                int needed = WideCharToMultiByte(CP_UTF8, 0, filePath, -1, nullptr, 0, nullptr, nullptr);
                if (needed > 0) {
                    std::string utf8(static_cast<size_t>(needed - 1), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, filePath, -1, utf8.data(), needed, nullptr, nullptr);
                    m_pendingImport = DatImporter::ImportJSON(utf8, *m_table);
                    m_pendingImportFormat = "json";
                    m_importConfirmOpen = true;
                    m_importConfirmOpenRequested = true;
                    m_importShowLog = false;
                }
            }
        }

#ifdef _DEBUG
        ImGui::SameLine();
        if (ImGui::Button("[Dev] Run Round-Trip Tests")) {
            // The host ships the schema next to its exe, and pins the working
            // directory to the exe folder.
            const std::string schemaPath = "Resources/dat-schema/schema.min.json";
            auto results = GGPKExp::Dev::RunAllRoundTrips(schemaPath);
            for (const auto& r : results) {
                char buf[512];
                snprintf(buf, sizeof(buf),
                         "[RoundTrip] %s: parsed=%d ser=%d reparsed=%d rowCount=%d allCells=%d | %s\n",
                         r.fileName.c_str(), r.parsed, r.serialized, r.reparsed,
                         r.rowCountMatches, r.allCellsMatch, r.detail.c_str());
                OutputDebugStringA(buf);
            }
        }
#endif

        // Save Changes button — shown when cells have been modified OR when an import is pending.
        if (!m_modifiedCells.empty() || m_importMode) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
            const std::string lblSave = IconLabel(m_fa6Available, kIconSave, "Save Changes");
            if (ImGui::Button(lblSave.c_str()) && m_onSave && m_table) {
                std::vector<uint8_t> serialized;
                if (m_importMode) {
                    // Full rebuild after Import
                    serialized = Serialize(m_rows, m_table->columns);
                    // ApplyPendingImport already retained interval second halves
                    // in m_dat; rebuilding rows alone would zero them again.
                    PatchIntervalSecondHalves(serialized, static_cast<uint32_t>(m_rows.size()),
                        m_table->columns, ExtractIntervalSeconds(m_dat, m_table->columns));
                } else {
                    // Patch based (existing)
                    std::vector<GGPKExp::DatEdit> edits;
                    edits.reserve(m_modifiedCells.size());
                    for (uint64_t key : m_modifiedCells) {
                        uint32_t r = static_cast<uint32_t>(key >> 32);
                        uint32_t c = static_cast<uint32_t>(key & 0xFFFFFFFFu);
                        if (r >= m_rows.size() || c >= m_rows[r].size()) continue;
                        GGPKExp::DatEdit e;
                        e.rowIndex = r;
                        e.columnIndex = c;
                        e.newValue = m_rows[r][c];
                        edits.push_back(std::move(e));
                    }
                    serialized = GGPKExp::ApplyEdits(m_dat, m_table->columns, edits);
                }
                if (!serialized.empty()) {
                    m_onSave(std::move(serialized));
                    m_modifiedCells.clear();
                    m_importMode = false;  // after save, next Save uses patch path again
                } else {
                    // Schema mismatch or invalid dat — leave m_modifiedCells intact so
                    // the yellow "modified" highlight remains visible.
                }
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (m_importMode) {
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f),
                                   "(imported %zu rows - Save to write)", m_rows.size());
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f),
                                   "(%zu cells modified)", m_modifiedCells.size());
            }
        }
    }

    ImGui::Separator();

    // Case 2: no schema — show fallback message
    if (!m_table) {
        ImGui::TextDisabled("No schema found for '%s' — use the Hex tab to inspect raw bytes.", m_tableName.c_str());
        return;
    }

    // ---- Phase 4 / T11 — ensure the editor panel has its owner wired.
    // Done once per DatViewer instance; LoadData() resets the sentinel.
    if (!m_editor_OwnerWasSet) {
        m_editor.SetOwner(this);
        m_editor_OwnerWasSet = true;
    }

    // ---- Phase 4 / T11 — two-pane layout: table (left) + optional editor (right).
    const float avail = ImGui::GetContentRegionAvail().x;
    const bool  panelOpen = m_editor.IsOpen();
    const float panelW    = panelOpen ? m_editor.width : 0.0f;
    const float splitterW = panelOpen ? 4.0f : 0.0f;
    float tableW = avail - panelW - splitterW;
    if (tableW < 200.0f) tableW = 200.0f;

    ImGui::BeginChild("##DatTableLeft", ImVec2(tableW, 0), false);
    RenderTable();
    ImGui::EndChild();

    if (panelOpen) {
        ImGui::SameLine();
        ImGui::InvisibleButton("##splitter", ImVec2(splitterW, -1));
        if (ImGui::IsItemActive()) {
            const float delta = ImGui::GetIO().MouseDelta.x;
            m_editor.width = m_editor.width - delta;
            if (m_editor.width < CellEditorPanel::kMinWidth) m_editor.width = CellEditorPanel::kMinWidth;
            if (m_editor.width > CellEditorPanel::kMaxWidth) m_editor.width = CellEditorPanel::kMaxWidth;
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        const ImVec2 p0 = ImGui::GetItemRectMin();
        const ImVec2 p1 = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(p0.x + 2.0f, p0.y),
            ImVec2(p0.x + 2.0f, p1.y),
            IM_COL32(100, 100, 100, 255));

        ImGui::SameLine();
        m_editor.Render(m_editor.width);
    }

    // Tick highlight timer (previously at end of table block)
    if (m_highlightTimer > 0.0f) {
        m_highlightTimer -= ImGui::GetIO().DeltaTime;
        if (m_highlightTimer <= 0.0f) {
            m_highlightRow = -1;
            m_highlightTimer = 0.0f;
        }
    }

    // T10: render Find references modal (no-op when not open).
    RenderFindReferencesModal();

    // T21: render Import preview modal (no-op when not open).
    RenderImportModal();
}

// ---------------------------------------------------------------------------
// RenderTable — Phase 4 / T11 extracted body of the ImGui table. Runs inside
// the left-side child region laid out by Render().
// ---------------------------------------------------------------------------

void DatViewer::RenderTable()
{
    if (!m_table) return;

    // Case 3: table with schema — render ImGui table
    const int colCount = static_cast<int>(m_table->columns.size());
    const int totalCols = colCount + 1; // +1 for row index

    ImGuiTableFlags flags =
        ImGuiTableFlags_SizingFixedFit |
        ImGuiTableFlags_Borders       |
        ImGuiTableFlags_RowBg         |
        ImGuiTableFlags_Resizable     |
        ImGuiTableFlags_ScrollX       |
        ImGuiTableFlags_ScrollY       |
        ImGuiTableFlags_Sortable;

    ImVec2 tableSize(0.0f, ImGui::GetContentRegionAvail().y);
    if (!ImGui::BeginTable("##DatTable", totalCols, flags, tableSize))
        return;

    // Column headers
    ImGui::TableSetupScrollFreeze(1, 1); // freeze row index column + header row
    ImGui::TableSetupColumn("Row", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 50.0f);
    for (int c = 0; c < colCount; ++c) {
        const auto& col = m_table->columns[static_cast<size_t>(c)];

        // Build label with type suffix: "Name  (type)" or "Col N  (type)"
        std::string baseName = col.name.empty()
            ? ("Col " + std::to_string(c))
            : col.name;

        std::string typeSuffix;
        if (!col.refTable.empty()) {
            typeSuffix = col.refTable;
        } else {
            switch (col.type) {
            case ColumnType::Bool:       typeSuffix = "bool";     break;
            case ColumnType::I16:        typeSuffix = "i16";      break;
            case ColumnType::U16:        typeSuffix = "u16";      break;
            case ColumnType::I32:        typeSuffix = "i32";      break;
            case ColumnType::U8:         typeSuffix = "u8";      break;
            case ColumnType::U32:        typeSuffix = "u32";      break;
            case ColumnType::F32:        typeSuffix = "f32";      break;
            case ColumnType::String:     typeSuffix = "string";   break;
            case ColumnType::Row:        typeSuffix = "row";      break;
            case ColumnType::ForeignRow: typeSuffix = "rid";      break;
            case ColumnType::EnumRow:    typeSuffix = "enum";     break;
            default:                     typeSuffix = "?";        break;
            }
        }
        if (col.isArray)    typeSuffix = "array<" + typeSuffix + ">";
        if (col.isInterval) typeSuffix += " interval";

        std::string label = baseName + "  (" + typeSuffix + ")";

        float width = (static_cast<size_t>(c) < m_columnWidths.size()) ? m_columnWidths[static_cast<size_t>(c)] : 100.0f;
        ImGui::TableSetupColumn(label.c_str(), ImGuiTableColumnFlags_WidthFixed, width);
    }
    ImGui::TableHeadersRow();

    // Handle sort specs from ImGui
    if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
        if (sortSpecs->SpecsDirty) {
            if (sortSpecs->SpecsCount > 0) {
                int specCol = sortSpecs->Specs[0].ColumnIndex;
                if (specCol == 0) {
                    // Row index column
                    m_sortColumn = -1;
                } else {
                    m_sortColumn = specCol - 1; // offset by 1 for the "Row" column
                }
                m_sortAscending = (sortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending);
            } else {
                m_sortColumn = -1;
                m_sortAscending = true;
            }
            RebuildSortOrder();
            sortSpecs->SpecsDirty = false;
        }
    }

    // Ensure sorted indices are populated
    if (m_sortedIndices.size() != m_rows.size())
        RebuildSortOrder();

    // Use clipper for performance with large tables
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_dat.rowCount));

    // Force clipper to include the display index for the target row so we can scroll to it
    if (m_pendingScrollRow >= 0 && m_pendingScrollRow < static_cast<int>(m_dat.rowCount)) {
        // Find display index for this actual row
        for (size_t di = 0; di < m_sortedIndices.size(); ++di) {
            if (m_sortedIndices[di] == m_pendingScrollRow) {
                clipper.IncludeItemByIndex(static_cast<int>(di));
                break;
            }
        }
    }

    while (clipper.Step()) {
        for (int displayIdx = clipper.DisplayStart; displayIdx < clipper.DisplayEnd; ++displayIdx) {
            // Map display index to actual data row
            int row = (static_cast<size_t>(displayIdx) < m_sortedIndices.size())
                ? m_sortedIndices[static_cast<size_t>(displayIdx)]
                : displayIdx;

            ImGui::TableNextRow();

            // Blink highlight for navigated row (compare against actual row index)
            if (m_highlightRow == row && m_highlightTimer > 0.0f) {
                float t = 3.0f - m_highlightTimer;
                float alpha = (0.3f + 0.2f * sinf(t * 6.0f)) * (m_highlightTimer / 3.0f);
                ImU32 hlColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 0.0f, alpha));
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, hlColor);
            }

            // Row index column — always shows the actual row index
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%d", row);

            // Data columns
            for (int c = 0; c < colCount; ++c) {
                if (!ImGui::TableSetColumnIndex(c + 1))
                    continue;

                const auto& col = m_table->columns[static_cast<size_t>(c)];
                static CellValue nullCell{std::monostate{}};
                bool cellInRange = static_cast<size_t>(c) < m_rows[static_cast<size_t>(row)].size();
                CellValue& cell = cellInRange
                    ? m_rows[static_cast<size_t>(row)][static_cast<size_t>(c)]
                    : nullCell;

                std::string cellStr = CellToString(cell, &col);

                // ForeignRow: clickable colored link if refTable is set
                bool isForeignRow = (col.type == ColumnType::ForeignRow) && !col.refTable.empty();
                bool isRow = (col.type == ColumnType::Row);

                // Check if this cell is being edited (use actual row index for edit tracking)
                bool isEditing = m_editState.IsEditing(row, c);

                if (isEditing) {
                    // Inline editing widget
                    if (std::holds_alternative<bool>(cell)) {
                        bool bval = std::get<bool>(cell);
                        if (ImGui::Checkbox("##edit", &bval)) {
                            m_rows[static_cast<size_t>(row)][static_cast<size_t>(c)] = bval;
                            uint64_t key = (static_cast<uint64_t>(row) << 32) | static_cast<uint32_t>(c);
                            m_modifiedCells.insert(key);
                            m_editState.Cancel();
                        }
                        // Cancel on Escape
                        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                            m_editState.Cancel();
                    } else if (!std::holds_alternative<CellArray>(cell)) {
                        ImGui::SetNextItemWidth(-1);
                        // Auto-focus the input on first frame
                        if (!ImGui::IsAnyItemActive())
                            ImGui::SetKeyboardFocusHere();
                        bool committed = ImGui::InputText("##editcell", m_editState.buf, sizeof(m_editState.buf),
                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                        bool cancelled = ImGui::IsKeyPressed(ImGuiKey_Escape);
                        // Click-away: commit when the input loses focus
                        if (ImGui::IsItemDeactivated() && !committed && !cancelled)
                            committed = true;

                        if (committed) {
                            try {
                                std::string s(m_editState.buf);
                                CellValue newVal = cell;
                                if (col.type == ColumnType::U8) newVal = ParseU8Value(s);
                                else if (std::holds_alternative<int16_t>(cell)) newVal = static_cast<int16_t>(std::stoi(s));
                                else if (std::holds_alternative<uint16_t>(cell)) newVal = static_cast<uint16_t>(std::stoul(s, nullptr, 0));
                                else if (std::holds_alternative<int32_t>(cell)) newVal = static_cast<int32_t>(std::stol(s, nullptr, 0));
                                else if (std::holds_alternative<uint32_t>(cell)) newVal = static_cast<uint32_t>(std::stoul(s, nullptr, 0));
                                else if (std::holds_alternative<float>(cell)) newVal = std::stof(s);
                                else if (std::holds_alternative<std::string>(cell)) newVal = s;
                                else if (std::holds_alternative<int64_t>(cell)) newVal = static_cast<int64_t>(std::stoll(s));
                                m_rows[static_cast<size_t>(row)][static_cast<size_t>(c)] = newVal;
                                uint64_t key = (static_cast<uint64_t>(row) << 32) | static_cast<uint32_t>(c);
                                m_modifiedCells.insert(key);
                            } catch (...) { /* invalid input, keep old value */ }
                            m_editState.Cancel();
                        }
                        if (cancelled) m_editState.Cancel();
                    }
                } else {
                    // Modified cell highlight
                    uint64_t key = (static_cast<uint64_t>(row) << 32) | static_cast<uint32_t>(c);
                    if (m_modifiedCells.count(key)) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                            ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 0.0f, 0.15f)));
                    }

                    // Bug fix (Phase 7 UX): capture the full cell rect up-front so
                    // we can hit-test single/double/right-click against the ENTIRE
                    // cell area — not just the last-submitted content widget (which
                    // was causing Bug 1 where only the trailing ']' of an array was
                    // clickable for the context menu, and Bug 2 where right-click
                    // only worked on string/primitive text glyphs, not whitespace).
                    // Content widgets render normally and retain their own click
                    // handling (ForeignRow link, hyperlink Ctrl+Click, array '>'
                    // toggle) because we do the cell-wide hit-test AFTER content
                    // submission via IsMouseHoveringRect against the computed cell
                    // rect, gated by IsWindowHovered to avoid phantom hits through
                    // overlaying windows/popups.
                    const ImVec2 cellStartPos = ImGui::GetCursorScreenPos();
                    const float cellAvailW = ImGui::GetContentRegionAvail().x;

                    // Detect path-valued string cells that resolve to a real
                    // GGPK entry — rendered as a clickable blue hyperlink.
                    // Additionally (T6), detect cells whose schema declares a
                    // file-reference (col.refFile / col.refFiles) but whose
                    // value does NOT resolve in the tree → red warning render.
                    bool isPathLink = false;
                    bool hasMissingFileRef = false;
                    const bool hasSchemaFileRef = !col.refFile.empty() || !col.refFiles.empty();
                    if (!isForeignRow && !isRow &&
                        std::holds_alternative<std::string>(cell)) {
                        const auto& s = std::get<std::string>(cell);
                        if (s != "null") {
                            const bool heuristic = IsLikelyGGPKPath(s);
                            if (hasSchemaFileRef || heuristic) {
                                // See note at RenderArrayItem's existence
                                // check: we use GGPKManager because its path
                                // cache covers bundle contents (VirtualTree
                                // does not).
                                const bool exists = m_ggpkManager
                                    && (m_ggpkManager->FindNodeByPath(s) != nullptr);
                                if (exists) {
                                    isPathLink = true;
                                } else if (hasSchemaFileRef) {
                                    hasMissingFileRef = true;
                                }
                            }
                        }
                    }

                    // Display logic
                    if (isForeignRow && std::holds_alternative<int64_t>(cell)) {
                        int64_t rowRef = std::get<int64_t>(cell);
                        if (rowRef >= 0) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
                            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.4f, 0.8f, 1.0f, 0.15f));
                            char linkId[256];
                            snprintf(linkId, sizeof(linkId), "%s##fk_%d_%d", cellStr.c_str(), row, c);
                            if (ImGui::Selectable(linkId, false, ImGuiSelectableFlags_None)) {
                                pendingNavPath = col.refTable + ".datc64";
                                pendingNavRow = static_cast<int>(rowRef);
                            }
                            ImGui::PopStyleColor(3);

                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("Navigate to %s row %lld", col.refTable.c_str(), (long long)rowRef);
                                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            }
                        } else {
                            ImGui::TextDisabled("<null>");
                        }
                    } else if (isRow && std::holds_alternative<int64_t>(cell)) {
                        // Self-referencing row index — show in a slightly different color
                        int64_t rowRef = std::get<int64_t>(cell);
                        if (rowRef >= 0) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.8f, 0.5f, 1.0f));
                            ImGui::TextUnformatted(cellStr.c_str());
                            ImGui::PopStyleColor();
                        } else {
                            ImGui::TextDisabled("%s", cellStr.c_str());
                        }
                    } else if (std::holds_alternative<std::monostate>(cell)) {
                        ImGui::TextDisabled("<null>");
                    } else if (std::holds_alternative<std::string>(cell) &&
                               std::get<std::string>(cell) == "null") {
                        ImGui::TextDisabled("null");
                    } else if (isPathLink) {
                        const auto& s = std::get<std::string>(cell);
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", s.c_str());
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            ImGui::SetTooltip("Ctrl+Click to navigate to %s", s.c_str());
                        }
                        if (ImGui::IsItemClicked() && ImGui::GetIO().KeyCtrl) {
                            pendingNavPath = s;
                            pendingNavRow  = -1;
                        }
                    } else if (hasMissingFileRef) {
                        // T6: schema declares file-ref but value does not
                        // resolve in the GGPK → red warning render.
                        const auto& s = std::get<std::string>(cell);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                        const char* prefix = m_fa6Available ? ICON_FA_TRIANGLE_EXCLAMATION " " : "[!] ";
                        ImGui::Text("%s%s", prefix, s.c_str());
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered()) {
                            const char* hint = !col.refFile.empty()
                                ? col.refFile.c_str()
                                : (!col.refFiles.empty() ? col.refFiles.front().c_str() : "file reference");
                            RenderMissingFileRefTooltip(s, hint);
                        }
                    } else if (std::holds_alternative<CellArray>(cell)) {
                        RenderCellArray(std::get<CellArray>(cell), &col, row, c);
                    } else {
                        ImGui::TextUnformatted(cellStr.c_str());
                    }

                    // Build the full cell rect from the start pos + available width,
                    // using the end cursor Y to cover vertical content extent. This
                    // is the hit-test area for Bug 1 / Bug 2 fixes.
                    const float cellEndY = ImGui::GetCursorScreenPos().y;
                    const float cellRectY1 = (std::max)(cellEndY,
                        cellStartPos.y + ImGui::GetTextLineHeightWithSpacing());
                    const ImVec2 cellRectMin = cellStartPos;
                    const ImVec2 cellRectMax(cellStartPos.x + cellAvailW, cellRectY1);

                    // T8 (fixed): right-click context menu now opens whenever the
                    // mouse is inside the cell rect — covers empty whitespace AND
                    // any non-interactive text glyphs. Sub-widgets (ForeignRow link,
                    // array '>' toggle) only consume LEFT clicks; right-click is not
                    // swallowed by any existing child widget, so a plain rect test
                    // is sufficient. IsMouseHoveringRect alone disregards window
                    // ordering / popup-blocking, so we AND it with IsWindowHovered()
                    // to prevent phantom hits through an overlaying popup / another
                    // ImGui window.
                    const bool cellHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
                        && ImGui::IsMouseHoveringRect(cellRectMin, cellRectMax);

                    char popupId[64];
                    snprintf(popupId, sizeof(popupId), "##cellctx_%d_%d", row, c);
                    if (cellHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)
                        && !ImGui::IsPopupOpen(popupId))
                    {
                        ImGui::OpenPopup(popupId);
                    }
                    // Bind the popup body (menu items) to the per-cell ID.
                    RenderCellContextMenu(row, c, cell, col);

                    // T7: single left-click selects. The hyperlink Ctrl+Click still
                    // runs above via its own IsItemClicked && KeyCtrl path; we guard
                    // on !KeyCtrl here so Ctrl+Click on a hyperlink only navigates
                    // (doesn't accidentally also change selection). Non-Ctrl clicks
                    // anywhere in the cell (including on top of a ForeignRow link
                    // or array toggle) also select — which is desirable behavior.
                    if (cellHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                        && !ImGui::GetIO().KeyCtrl)
                    {
                        m_selectedCell = { row, c };
                        if (m_editor.IsOpen()) {
                            if (m_editor.PromptDirtyConfirm(row, c)) {
                                RebindEditorTo(row, c);
                            }
                        }
                    }
                    // T7: double-click edits primitives / opens panel for complex.
                    if (cellHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        const bool isFileRefString =
                            (col.type == ColumnType::String &&
                             (!col.refFile.empty() || !col.refFiles.empty()));
                        const bool isPrimitive =
                            !std::holds_alternative<CellArray>(cell) &&
                            col.type != ColumnType::ForeignRow &&
                            col.type != ColumnType::Row &&
                            !isFileRefString;
                        if (isPrimitive) {
                            m_editState.Begin(row, c, cellStr);
                        } else {
                            if (!m_editor.IsOpen() || m_editor.PromptDirtyConfirm(row, c)) {
                                RebindEditorTo(row, c);
                            }
                        }
                    }

                    // T7: selection highlight — blue border drawn over the cell rect.
                    if (m_selectedCell.row == row && m_selectedCell.col == c) {
                        ImGui::GetWindowDrawList()->AddRect(
                            cellRectMin, cellRectMax,
                            IM_COL32(120, 180, 255, 255),
                            0.0f, 0, 2.0f);
                    }
                }
            }

            // Scroll to target row AFTER row content is laid out (compare actual row index)
            if (m_pendingScrollRow == row) {
                ImGui::SetScrollHereY(0.5f);
                m_pendingScrollRow = -1;
            }
        }
    }

    clipper.End();
    ImGui::EndTable();
}

// ---------------------------------------------------------------------------
// ComputeColumnWidths
// ---------------------------------------------------------------------------

void DatViewer::ComputeColumnWidths()
{
    if (!m_table) return;
    m_columnWidths.resize(m_table->columns.size());
    for (size_t col = 0; col < m_table->columns.size(); ++col) {
        const auto& colDef = m_table->columns[col];

        // Build the same header text used in Render (name + type suffix)
        std::string baseName = colDef.name.empty() ? ("Col " + std::to_string(col)) : colDef.name;
        std::string typeSuffix;
        if (!colDef.refTable.empty()) {
            typeSuffix = colDef.refTable;
        } else {
            switch (colDef.type) {
            case ColumnType::Bool:       typeSuffix = "bool";   break;
            case ColumnType::I16:        typeSuffix = "i16";    break;
            case ColumnType::U16:        typeSuffix = "u16";    break;
            case ColumnType::I32:        typeSuffix = "i32";    break;
            case ColumnType::U8:         typeSuffix = "u8";    break;
            case ColumnType::U32:        typeSuffix = "u32";    break;
            case ColumnType::F32:        typeSuffix = "f32";    break;
            case ColumnType::String:     typeSuffix = "string"; break;
            case ColumnType::Row:        typeSuffix = "row";    break;
            case ColumnType::ForeignRow: typeSuffix = "rid";    break;
            case ColumnType::EnumRow:    typeSuffix = "enum";   break;
            default:                     typeSuffix = "?";      break;
            }
        }
        if (colDef.isArray)    typeSuffix = "array<" + typeSuffix + ">";
        if (colDef.isInterval) typeSuffix += " interval";
        std::string headerText = baseName + "  (" + typeSuffix + ")";

        // Header width + sort arrow space
        float maxW = ImGui::CalcTextSize(headerText.c_str()).x + 32.0f;

        // Sample data rows (cap at 200 for performance)
        size_t sampleLimit = std::min(m_rows.size(), static_cast<size_t>(200));
        for (size_t row = 0; row < sampleLimit; ++row) {
            if (col < m_rows[row].size()) {
                std::string text = CellToString(m_rows[row][col], &colDef);
                float w = ImGui::CalcTextSize(text.c_str()).x + 16.0f;
                if (w > maxW) maxW = w;
            }
        }
        m_columnWidths[col] = std::clamp(maxW, 60.0f, 400.0f);
    }
}

// ---------------------------------------------------------------------------
// ExportJSON
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// CellValueToJson — shared helper used by ExportJSON and CopyRowAsJson (T9).
// Hash columns (name contains "hash") are serialized as hex strings so that
// signed/unsigned interpretation is unambiguous in the exported JSON.
// ---------------------------------------------------------------------------

nlohmann::json DatViewer::CellValueToJson(const CellValue& val, bool isHash) const
{
    return DatExport::CellValueToJson(val, isHash);
}

void DatViewer::ExportJSON(const std::string& path) const
{
    if (!m_table) return;
    DatExport::WriteJson(PathUtils::FromUtf8(path), m_dat, *m_table, m_rows);
}

void DatViewer::ScrollToRow(int row) {
    if (row >= 0 && row < static_cast<int>(m_dat.rowCount)) {
        m_pendingScrollRow = row;
        m_highlightRow = row;
        m_highlightTimer = 3.0f;
    }
}

// ---------------------------------------------------------------------------
// RebuildSortOrder — creates sorted index mapping
// ---------------------------------------------------------------------------

void DatViewer::RebuildSortOrder()
{
    m_sortedIndices.resize(m_rows.size());
    for (size_t i = 0; i < m_rows.size(); ++i)
        m_sortedIndices[i] = static_cast<int>(i);

    if (m_sortColumn < 0 || !m_table || static_cast<size_t>(m_sortColumn) >= m_table->columns.size()) {
        // Default: sort by row index
        if (!m_sortAscending)
            std::reverse(m_sortedIndices.begin(), m_sortedIndices.end());
        return;
    }

    const int col = m_sortColumn;
    const bool asc = m_sortAscending;

    std::stable_sort(m_sortedIndices.begin(), m_sortedIndices.end(),
        [&](int a, int b) -> bool {
            const auto& rowA = m_rows[static_cast<size_t>(a)];
            const auto& rowB = m_rows[static_cast<size_t>(b)];
            size_t ci = static_cast<size_t>(col);

            bool aHas = ci < rowA.size();
            bool bHas = ci < rowB.size();
            if (!aHas && !bHas) return false;
            if (!aHas) return asc;
            if (!bHas) return !asc;

            const auto& va = rowA[ci];
            const auto& vb = rowB[ci];

            // monostate (null) sorts last
            bool aNull = std::holds_alternative<std::monostate>(va);
            bool bNull = std::holds_alternative<std::monostate>(vb);
            if (aNull && bNull) return false;
            if (aNull) return false; // nulls always last
            if (bNull) return true;

            // Compare by converting to string for simplicity (handles all types)
            std::string sa = CellToString(va, &m_table->columns[ci]);
            std::string sb = CellToString(vb, &m_table->columns[ci]);

            // Try numeric comparison first
            try {
                double da = std::stod(sa);
                double db = std::stod(sb);
                return asc ? (da < db) : (da > db);
            } catch (...) {}

            // Fall back to lexicographic
            int cmp = sa.compare(sb);
            return asc ? (cmp < 0) : (cmp > 0);
        });
}

// ---------------------------------------------------------------------------
// RenderSchemaTab — color-coded type definition display
// ---------------------------------------------------------------------------

void DatViewer::RenderSchemaTab()
{
    if (!m_table) {
        ImGui::TextDisabled("No schema available for %s", m_tableName.c_str());
        return;
    }

    static const ImVec4 colKeyword  = ImVec4(0.88f, 0.42f, 0.46f, 1.0f); // "type" keyword — red
    static const ImVec4 colTypeName = ImVec4(0.38f, 0.69f, 0.94f, 1.0f); // type name — blue
    static const ImVec4 colRef      = ImVec4(0.90f, 0.75f, 0.48f, 1.0f); // type references — yellow
    static const ImVec4 colPunct    = ImVec4(0.42f, 0.45f, 0.52f, 1.0f); // punctuation — grey
    static const ImVec4 colDimmed   = ImVec4(0.50f, 0.50f, 0.50f, 1.0f); // (interval) — dimmed

    // "type TableName {"
    ImGui::TextColored(colKeyword, "type");
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextColored(colTypeName, "%s", m_table->name.c_str());
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextColored(colPunct, "{");

    ImGui::Indent(16.0f);

    for (const auto& col : m_table->columns) {
        // Resolve type string
        std::string typeStr;
        if (!col.refTable.empty()) {
            typeStr = col.refTable;
        } else {
            switch (col.type) {
            case ColumnType::Bool:       typeStr = "bool";     break;
            case ColumnType::I16:        typeStr = "i16";      break;
            case ColumnType::U16:        typeStr = "u16";      break;
            case ColumnType::I32:        typeStr = "i32";      break;
            case ColumnType::U8:         typeStr = "u8";      break;
            case ColumnType::U32:        typeStr = "u32";      break;
            case ColumnType::F32:        typeStr = "f32";      break;
            case ColumnType::String:     typeStr = "string";   break;
            case ColumnType::Row:        typeStr = "row";      break;
            case ColumnType::ForeignRow: typeStr = "rid";      break;
            case ColumnType::EnumRow:    typeStr = "enumrow";  break;
            default:                     typeStr = "unknown";  break;
            }
        }

        // Field name (default text color)
        const std::string& fieldName = col.name.empty()
            ? ("_col" + std::to_string(col.index))
            : col.name;
        ImGui::TextUnformatted(fieldName.c_str());
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(colPunct, ":");
        ImGui::SameLine(0.0f, 4.0f);

        // Type — array<type> or plain type
        if (col.isArray) {
            ImGui::TextColored(colPunct, "array<");
            ImGui::SameLine(0.0f, 0.0f);
            // If refTable set, treat type as a reference (yellow); otherwise primitive (blue)
            if (!col.refTable.empty())
                ImGui::TextColored(colRef, "%s", typeStr.c_str());
            else
                ImGui::TextColored(colTypeName, "%s", typeStr.c_str());
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextColored(colPunct, ">");
        } else {
            if (!col.refTable.empty())
                ImGui::TextColored(colRef, "%s", typeStr.c_str());
            else
                ImGui::TextColored(colTypeName, "%s", typeStr.c_str());
        }

        // Optional (interval) annotation
        if (col.isInterval) {
            ImGui::SameLine(0.0f, 6.0f);
            ImGui::TextColored(colDimmed, "(interval)");
        }
    }

    ImGui::Unindent(16.0f);
    ImGui::TextColored(colPunct, "}");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Row size: %d bytes", m_table->totalRowSize);
    ImGui::Text("Columns: %zu", m_table->columns.size());
}

// ---------------------------------------------------------------------------
// Phase 3 / T8-T9 — Cell clipboard & context-menu helpers
// ---------------------------------------------------------------------------

void DatViewer::CopyCellToClipboard(const CellValue& val, const ColumnDef& colDef)
{
    // Wire format: "dat:<type>:<payload>". Array encoding is a simple
    // "[a,b,c]" — no JSON escaping (v1). Paste only accepts primitives.
    std::string out;
    if (std::holds_alternative<std::monostate>(val)) {
        out = "dat:null:";
    } else if (std::holds_alternative<bool>(val)) {
        out = std::string("dat:bool:") + (std::get<bool>(val) ? "true" : "false");
    } else if (std::holds_alternative<int16_t>(val)) {
        out = "dat:i16:" + std::to_string(std::get<int16_t>(val));
    } else if (std::holds_alternative<uint16_t>(val)) {
        out = "dat:u16:" + std::to_string(std::get<uint16_t>(val));
    } else if (std::holds_alternative<int32_t>(val)) {
        out = "dat:i32:" + std::to_string(std::get<int32_t>(val));
    } else if (std::holds_alternative<uint32_t>(val)) {
        out = std::string(colDef.type == ColumnType::U8 ? "dat:u8:" : "dat:u32:")
            + std::to_string(std::get<uint32_t>(val));
    } else if (std::holds_alternative<float>(val)) {
        char fb[64];
        snprintf(fb, sizeof(fb), "%.9g", std::get<float>(val));
        out = std::string("dat:f32:") + fb;
    } else if (std::holds_alternative<std::string>(val)) {
        out = "dat:string:" + std::get<std::string>(val);
    } else if (std::holds_alternative<int64_t>(val)) {
        if (colDef.type == ColumnType::ForeignRow) {
            out = "dat:foreignrow:table=" + colDef.refTable +
                  ";row=" + std::to_string(std::get<int64_t>(val));
        } else {
            out = "dat:row:" + std::to_string(std::get<int64_t>(val));
        }
    } else if (std::holds_alternative<CellArray>(val)) {
        // Determine inner type label once for the header.
        std::string innerType;
        switch (colDef.type) {
        case ColumnType::Bool:       innerType = "bool";   break;
        case ColumnType::I16:        innerType = "i16";    break;
        case ColumnType::U16:        innerType = "u16";    break;
        case ColumnType::I32: case ColumnType::EnumRow: innerType = "i32"; break;
        case ColumnType::U8:         innerType = "u8";    break;
        case ColumnType::U32:        innerType = "u32";    break;
        case ColumnType::F32:        innerType = "f32";    break;
        case ColumnType::String:     innerType = "string"; break;
        case ColumnType::Row:        innerType = "row";    break;
        case ColumnType::ForeignRow: innerType = "foreignrow"; break;
        default:                     innerType = "unknown"; break;
        }
        const auto& arr = std::get<CellArray>(val);
        out = "dat:array<" + innerType + ">:[";
        for (size_t i = 0; i < arr.items.size(); ++i) {
            if (i > 0) out += ",";
            out += CellToString(arr.items[i], &colDef);
        }
        out += "]";
    }
    ImGui::SetClipboardText(out.c_str());
}

bool DatViewer::PasteCellFromClipboard(int row, int col, const ColumnDef& colDef)
{
    const char* raw = ImGui::GetClipboardText();
    if (!raw) return false;
    std::string s(raw);
    if (s.rfind("dat:", 0) != 0) return false;

    // "dat:<type>:<payload>"
    auto firstColon = s.find(':', 4);
    if (firstColon == std::string::npos) return false;
    const std::string type    = s.substr(4, firstColon - 4);
    const std::string payload = s.substr(firstColon + 1);

    if (static_cast<size_t>(row) >= m_rows.size()) return false;
    if (static_cast<size_t>(col) >= m_rows[static_cast<size_t>(row)].size()) return false;

    // Array paste not supported in v1 — refuse with a log.
    if (type.rfind("array<", 0) == 0 || colDef.isArray) {
        OutputDebugStringA("[GGPKExp/DatViewer] Paste rejected: array types not supported in v1\n");
        return false;
    }

    CellValue newVal;
    try {
        if (type == "null") {
            newVal = std::monostate{};
        } else if (type == "bool" && colDef.type == ColumnType::Bool) {
            newVal = (payload == "true" || payload == "1");
        } else if (type == "i16" && colDef.type == ColumnType::I16) {
            newVal = static_cast<int16_t>(std::stoi(payload));
        } else if (type == "u16" && colDef.type == ColumnType::U16) {
            newVal = static_cast<uint16_t>(std::stoul(payload, nullptr, 0));
        } else if (type == "i32" &&
                   (colDef.type == ColumnType::I32 || colDef.type == ColumnType::EnumRow)) {
            newVal = static_cast<int32_t>(std::stol(payload, nullptr, 0));
        } else if (type == "u32" && colDef.type == ColumnType::U32) {
            newVal = static_cast<uint32_t>(std::stoul(payload, nullptr, 0));
        } else if (type == "u8" && colDef.type == ColumnType::U8) {
            newVal = ParseU8Value(payload);
        } else if (type == "f32" && colDef.type == ColumnType::F32) {
            newVal = std::stof(payload);
        } else if (type == "string" && colDef.type == ColumnType::String) {
            newVal = payload;
        } else if (type == "row" && colDef.type == ColumnType::Row) {
            newVal = static_cast<int64_t>(std::stoll(payload));
        } else if (type == "foreignrow" && colDef.type == ColumnType::ForeignRow) {
            auto rowPos = payload.find("row=");
            if (rowPos == std::string::npos) return false;
            newVal = static_cast<int64_t>(std::stoll(payload.substr(rowPos + 4)));
        } else {
            return false; // type mismatch
        }
    } catch (...) {
        return false; // malformed payload
    }

    m_rows[static_cast<size_t>(row)][static_cast<size_t>(col)] = std::move(newVal);
    uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(row)) << 32)
                 | static_cast<uint32_t>(col);
    m_modifiedCells.insert(key);
    return true;
}

void DatViewer::ClearCell(int row, int col, const ColumnDef& colDef)
{
    if (static_cast<size_t>(row) >= m_rows.size()) return;
    if (static_cast<size_t>(col) >= m_rows[static_cast<size_t>(row)].size()) return;

    CellValue& v = m_rows[static_cast<size_t>(row)][static_cast<size_t>(col)];
    if (colDef.isArray) {
        v = CellArray{};
    } else {
        switch (colDef.type) {
        case ColumnType::Bool:       v = false; break;
        case ColumnType::I16:        v = int16_t(0); break;
        case ColumnType::U16:        v = uint16_t(0); break;
        case ColumnType::I32: case ColumnType::EnumRow: v = int32_t(0); break;
        case ColumnType::U8:
        case ColumnType::U32:        v = uint32_t(0); break;
        case ColumnType::F32:        v = 0.0f; break;
        case ColumnType::String:     v = std::string{}; break;
        case ColumnType::Row: case ColumnType::ForeignRow: v = int64_t(-1); break;
        default: break;
        }
    }
    uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(row)) << 32)
                 | static_cast<uint32_t>(col);
    m_modifiedCells.insert(key);
}

void DatViewer::RevertCell(int row, int col)
{
    if (!m_table) return;
    if (static_cast<size_t>(col) >= m_table->columns.size()) return;
    if (static_cast<size_t>(row) >= m_rows.size()) return;

    // Re-decode the original value from m_dat.rawData (raw bytes are not
    // modified until Save, so this restores the on-disk value).
    uint32_t colOff = 0;
    for (size_t k = 0; k < static_cast<size_t>(col); ++k)
        colOff += static_cast<uint32_t>(GetColumnByteSize(m_table->columns[k]));

    m_rows[static_cast<size_t>(row)][static_cast<size_t>(col)] =
        DecodeCell(m_dat, static_cast<uint32_t>(row), colOff,
                   m_table->columns[static_cast<size_t>(col)]);

    uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(row)) << 32)
                 | static_cast<uint32_t>(col);
    m_modifiedCells.erase(key);
}

// Diagnostic tooltip for missing file-ref cells (Release-visible).
// Shows the exact query/lowered path and up to 3 similar keys so users can
// report what's happening without needing a Debug build. Similar-key scan
// goes through GGPKManager::ForEachFile so it covers BOTH loose GGPK files
// AND bundle2 contents — matching the existence check above.
void DatViewer::RenderMissingFileRefTooltip(const std::string& s, const char* schemaHint)
{
    std::string lowered;
    lowered.reserve(s.size());
    for (char ch : s)
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));

    const size_t prefixLen = (std::min)(static_cast<size_t>(12), lowered.size());
    const std::string prefix = lowered.substr(0, prefixLen);

    // Scan the full GGPKManager index (loose + bundles) for up to 3 keys
    // sharing the lowered prefix. O(N) one-shot scan — acceptable on a
    // user-triggered tooltip; still aborts early after 3 hits.
    std::vector<std::string> similar;
    size_t totalPaths = 0;
    if (m_ggpkManager) {
        m_ggpkManager->ForEachFile(
            [&](const std::string& fullPath, int64_t /*fileSize*/, void* /*nodePtr*/) {
                ++totalPaths;
                if (similar.size() >= 3) return;
                if (fullPath.size() < prefix.size()) return;
                // Case-insensitive prefix match.
                for (size_t i = 0; i < prefix.size(); ++i) {
                    char c = static_cast<char>(std::tolower(static_cast<unsigned char>(fullPath[i])));
                    if (c != prefix[i]) return;
                }
                similar.push_back(fullPath);
            });
    }

    ImGui::BeginTooltip();
    ImGui::TextUnformatted("File not found in GGPK");
    ImGui::Text("(schema expects: %s)", schemaHint ? schemaHint : "file reference");
    ImGui::Separator();
    ImGui::TextDisabled("Query:      %s", s.c_str());
    ImGui::TextDisabled("Lowercased: %s", lowered.c_str());
    ImGui::TextDisabled("Indexed paths: %zu", totalPaths);
    if (!similar.empty()) {
        ImGui::TextDisabled("Similar keys (prefix '%s'):", prefix.c_str());
        for (const auto& k : similar) ImGui::TextDisabled("  %s", k.c_str());
    } else if (totalPaths > 0) {
        ImGui::TextDisabled("No keys with prefix '%s'.", prefix.c_str());
    } else {
        ImGui::TextDisabled("GGPK index is empty.");
    }
    ImGui::EndTooltip();
}

// ---------------------------------------------------------------------------
// Phase 4 / T12 — RebindEditorTo: opens the side panel against (row,col),
// picking a mode from the column schema. Bounds-checked — bails silently on
// out-of-range indices so the table render loop can fire this safely.
// ---------------------------------------------------------------------------

void DatViewer::RebindEditorTo(int row, int col)
{
    if (!m_table) return;
    if (row < 0 || col < 0) return;
    if (static_cast<size_t>(col) >= m_table->columns.size()) return;
    if (static_cast<size_t>(row) >= m_rows.size()) return;
    if (static_cast<size_t>(col) >= m_rows[static_cast<size_t>(row)].size()) return;

    const ColumnDef& cd = m_table->columns[static_cast<size_t>(col)];
    CellValue v = m_rows[static_cast<size_t>(row)][static_cast<size_t>(col)];

    CellEditorPanel::Mode mode;
    if (cd.isArray) {
        mode = CellEditorPanel::Mode::Array;
    } else if (cd.type == ColumnType::ForeignRow || cd.type == ColumnType::Row) {
        mode = CellEditorPanel::Mode::ForeignRow;
    } else if (cd.type == ColumnType::String &&
               (!cd.refFile.empty() || !cd.refFiles.empty())) {
        mode = CellEditorPanel::Mode::FileRef;
    } else {
        mode = CellEditorPanel::Mode::Primitive;
    }
    m_editor.Open(row, col, mode, v, cd);
}

void DatViewer::RenderCellContextMenu(int row, int col, const CellValue& val,
                                       const ColumnDef& colDef)
{
    // Unique popup ID per cell so right-click binds to the correct cell.
    char popupId[64];
    snprintf(popupId, sizeof(popupId), "##cellctx_%d_%d", row, col);

    if (!ImGui::BeginPopupContextItem(popupId)) return;

    // Edit — primitives use the inline editor; complex types (array/foreign/
    // file-ref) route to the right-side CellEditorPanel (Phase 4 / T12).
    const bool isFileRefString =
        (colDef.type == ColumnType::String &&
         (!colDef.refFile.empty() || !colDef.refFiles.empty()));
    const bool isPrimitive =
        !std::holds_alternative<CellArray>(val) &&
        colDef.type != ColumnType::ForeignRow &&
        colDef.type != ColumnType::Row &&
        !isFileRefString;
    if (ImGui::MenuItem(IconLabel(m_fa6Available, kIconEdit, "Edit").c_str())) {
        if (isPrimitive) {
            std::string cellStr = CellToString(val, &colDef);
            m_editState.Begin(row, col, cellStr);
        } else {
            if (!m_editor.IsOpen() || m_editor.PromptDirtyConfirm(row, col)) {
                RebindEditorTo(row, col);
            }
        }
    }

    // Copy value
    if (ImGui::MenuItem(IconLabel(m_fa6Available, kIconCopy, "Copy value").c_str())) {
        CopyCellToClipboard(val, colDef);
    }

    // Paste value — only if clipboard starts with "dat:".
    const char* cb = ImGui::GetClipboardText();
    const bool pastable = cb && std::string(cb).rfind("dat:", 0) == 0;
    if (ImGui::MenuItem(IconLabel(m_fa6Available, kIconPaste, "Paste value").c_str(),
                        nullptr, false, pastable)) {
        if (!PasteCellFromClipboard(row, col, colDef)) {
            OutputDebugStringA("[GGPKExp/DatViewer] Paste rejected: type mismatch\n");
        }
    }

    // Clear — all supported scalar/string/array column types are nullable for our purposes.
    const bool nullable =
        colDef.isArray ||
        colDef.type == ColumnType::String ||
        colDef.type == ColumnType::Row ||
        colDef.type == ColumnType::ForeignRow ||
        colDef.type == ColumnType::Bool ||
        colDef.type == ColumnType::U8 ||
        colDef.type == ColumnType::I16 || colDef.type == ColumnType::U16 ||
        colDef.type == ColumnType::I32 || colDef.type == ColumnType::U32 ||
        colDef.type == ColumnType::F32 || colDef.type == ColumnType::EnumRow;
    if (ImGui::MenuItem(IconLabel(m_fa6Available, kIconClear, "Clear").c_str(),
                        nullptr, false, nullable)) {
        ClearCell(row, col, colDef);
    }

    ImGui::Separator();

    // Navigate to — valid ForeignRow (non-negative id + non-empty refTable)
    // or String cell resolving to a real GGPK entry.
    bool canNav = false;
    if (colDef.type == ColumnType::ForeignRow && std::holds_alternative<int64_t>(val)) {
        int64_t r = std::get<int64_t>(val);
        canNav = (r >= 0 && !colDef.refTable.empty());
    } else if (std::holds_alternative<std::string>(val)) {
        const auto& sref = std::get<std::string>(val);
        // Same fix as the cell-render existence checks — go through
        // GGPKManager so bundle-contained paths are recognised as navigable.
        canNav = !sref.empty() && m_ggpkManager &&
                 m_ggpkManager->FindNodeByPath(sref) != nullptr;
    }
    if (ImGui::MenuItem(IconLabel(m_fa6Available, kIconNavigate, "Navigate to...").c_str(),
                        nullptr, false, canNav)) {
        if (colDef.type == ColumnType::ForeignRow && std::holds_alternative<int64_t>(val)) {
            pendingNavPath = colDef.refTable + ".datc64";
            pendingNavRow  = static_cast<int>(std::get<int64_t>(val));
        } else if (std::holds_alternative<std::string>(val)) {
            pendingNavPath = std::get<std::string>(val);
            pendingNavRow  = -1;
        }
    }

    ImGui::Separator();

    // Revert — only when the cell is in m_modifiedCells.
    const uint64_t key =
        (static_cast<uint64_t>(static_cast<uint32_t>(row)) << 32)
        | static_cast<uint32_t>(col);
    const bool modified = m_modifiedCells.count(key) > 0;
    if (ImGui::MenuItem(IconLabel(m_fa6Available, kIconRevert, "Revert cell").c_str(),
                        nullptr, false, modified)) {
        RevertCell(row, col);
    }

    ImGui::Separator();

    // Copy row as JSON (T9) — always available when a row exists.
    if (ImGui::MenuItem(IconLabel(m_fa6Available, kIconRowJson, "Copy row as JSON").c_str())) {
        CopyRowAsJson(row);
    }

    // Find all references (T10).
    if (ImGui::MenuItem(IconLabel(m_fa6Available, kIconFindRef, "Find all references to this row").c_str())) {
        OpenFindReferencesModal(row);
    }

#ifdef _DEBUG
    // Debug: inspect GGPK path resolution for string cells. Lets the user
    // log the lookup result + a sample of indexed keys to DebugView for a
    // failing "File not found in GGPK" cell. Reports BOTH the loose-file
    // tree result (VirtualTree) and the authoritative GGPKManager result so
    // any discrepancy (bundle-only files) is obvious.
    if (colDef.type == ColumnType::String && std::holds_alternative<std::string>(val)) {
        ImGui::Separator();
        if (ImGui::MenuItem("[Dev] Inspect path resolution")) {
            const auto& s = std::get<std::string>(val);
            std::string lowered;
            lowered.reserve(s.size());
            for (char c : s)
                lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

            const uint32_t treeFound = m_tree
                ? m_tree->FindByPathInsensitive(s)
                : UINT32_MAX;
            const bool mgrFound = m_ggpkManager
                && m_ggpkManager->FindNodeByPath(s) != nullptr;

            const size_t prefixLen = (std::min)(static_cast<size_t>(12), lowered.size());
            const std::string prefix = lowered.substr(0, prefixLen);
            std::vector<std::string> keys;
            if (m_tree) keys = m_tree->DebugFindKeysWithPrefix(prefix, 5);

            char buf[1024];
            snprintf(buf, sizeof(buf),
                "[PathInspect] query='%s' lowered='%s' tree=%s mgr=%s tree_index=%zu prefix='%s' tree_matches=%zu\n",
                s.c_str(), lowered.c_str(),
                (treeFound == UINT32_MAX ? "NO" : "YES"),
                (mgrFound ? "YES" : "NO"),
                m_tree ? m_tree->GetPathIndexSize() : static_cast<size_t>(0),
                prefix.c_str(),
                keys.size());
            OutputDebugStringA(buf);
            for (const auto& k : keys) {
                OutputDebugStringA("  tree key: ");
                OutputDebugStringA(k.c_str());
                OutputDebugStringA("\n");
            }
        }
    }
#endif

    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Task 9: CopyRowAsJson — serialize one row to clipboard using the same
// schema/layout as ExportJSON (extracted CellValueToJson helper).
// ---------------------------------------------------------------------------

void DatViewer::CopyRowAsJson(int row)
{
    if (!m_table) return;
    if (row < 0 || static_cast<size_t>(row) >= m_rows.size()) return;

    nlohmann::ordered_json obj;
    obj["_rid"] = row;

    const auto& r = m_rows[static_cast<size_t>(row)];
    for (size_t c = 0; c < m_table->columns.size(); ++c) {
        const auto& cd = m_table->columns[c];
        std::string name = cd.name.empty() ? ("Col" + std::to_string(c)) : cd.name;
        const bool isHash = IsHashColumn(name);

        if (c >= r.size()) { obj[name] = nullptr; continue; }
        const auto& v = r[c];

        // Interval columns: write Name + Name_range (second half from raw bytes).
        if (cd.isInterval && !cd.isArray) {
            obj[name] = CellValueToJson(v, isHash);

            uint32_t colOff = 0;
            for (size_t k = 0; k < c; ++k)
                colOff += static_cast<uint32_t>(GetColumnByteSize(m_table->columns[k]));
            const int baseSize = GetTypeSize(cd.type);
            const size_t rowStart = m_dat.fixedOffset +
                                     static_cast<size_t>(row) * m_dat.rowLength;
            const size_t secondAbsOff = rowStart + colOff + static_cast<size_t>(baseSize);
            CellValue second = std::monostate{};
            if (secondAbsOff + static_cast<size_t>(baseSize) <= m_dat.rawData.size()) {
                const uint8_t* p = m_dat.rawData.data() + secondAbsOff;
                switch (cd.type) {
                case ColumnType::U8: { second = static_cast<uint32_t>(*p); break; }
                case ColumnType::I16: { int16_t vv; std::memcpy(&vv, p, 2); second = vv; break; }
                case ColumnType::U16: { uint16_t vv; std::memcpy(&vv, p, 2); second = vv; break; }
                case ColumnType::I32: case ColumnType::EnumRow:
                    { int32_t vv; std::memcpy(&vv, p, 4); second = vv; break; }
                case ColumnType::U32: { uint32_t vv; std::memcpy(&vv, p, 4); second = vv; break; }
                case ColumnType::F32: { float vv; std::memcpy(&vv, p, 4); second = vv; break; }
                default: break;
                }
            }
            obj[name + "_range"] = CellValueToJson(second, isHash);
            continue;
        }

        if (std::holds_alternative<CellArray>(v)) {
            nlohmann::ordered_json jArr = nlohmann::ordered_json::array();
            for (const auto& item : std::get<CellArray>(v).items)
                jArr.push_back(CellValueToJson(item, isHash));
            obj[name] = jArr;
        } else {
            obj[name] = CellValueToJson(v, isHash);
        }
    }

    const std::string text = obj.dump(2);
    ImGui::SetClipboardText(text.c_str());
}

// ---------------------------------------------------------------------------
// Task 10: Find-all-references modal — async scan over every .datc64 in the
// tree, reports progress, and allows cancellation. Clicking a result sets
// pendingNavPath + pendingNavRow so ContentPanel can switch tabs.
// ---------------------------------------------------------------------------

void DatViewer::OpenFindReferencesModal(int row)
{
    if (!m_schema || !m_tree || !m_ggpkManager || row < 0 || m_tableName.empty())
        return;

    // If a previous scan is still running, cancel it and wait before reusing state.
    if (m_findRefsFuture.valid()) {
        m_findRefsCancel.store(true);
        m_findRefsFuture.wait();
    }

    // Derive target table name from current DAT file (strip .datc64 suffix).
    std::string target = m_tableName;
    constexpr const char* kSfx = ".datc64";
    if (target.size() > 7 && target.compare(target.size() - 7, 7, kSfx) == 0)
        target.erase(target.size() - 7);

    m_findRefsTargetRow = row;
    m_findRefsTargetTable = target;
    m_findRefsResults.clear();
    m_findRefsCancel = false;
    m_findRefsCurrent = 0;
    m_findRefsTotal = 0;
    m_findRefsModalOpen = true;
    m_findRefsOpenPopupRequested = true;

    GGPKExp::VirtualTree* tree = m_tree;
    GGPKExp::GGPKManager* mgr = m_ggpkManager;
    GGPKExp::DatSchema* schema = m_schema;
    std::string tt = target;
    uint32_t tr = static_cast<uint32_t>(row);

    m_findRefsFuture = std::async(std::launch::async, [this, tree, mgr, schema, tt, tr]() {
        auto res = GGPKExp::FindReferencesToRow(
            tree, mgr, schema, tt, tr,
            [this](size_t cur, size_t total) {
                m_findRefsCurrent.store(cur);
                m_findRefsTotal.store(total);
            },
            [this]() { return m_findRefsCancel.load(); });
        m_findRefsResults = std::move(res);
    });
}

void DatViewer::RenderFindReferencesModal()
{
    if (!m_findRefsModalOpen)
        return;

    if (m_findRefsOpenPopupRequested) {
        ImGui::OpenPopup("Find references");
        m_findRefsOpenPopupRequested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_Appearing);

    bool stayOpen = true;
    if (ImGui::BeginPopupModal("Find references", &stayOpen)) {
        const bool done = m_findRefsFuture.valid() &&
                          m_findRefsFuture.wait_for(std::chrono::seconds(0))
                              == std::future_status::ready;

        ImGui::Text("Target: %s.datc64  |  row %d",
                    m_findRefsTargetTable.c_str(), m_findRefsTargetRow);
        ImGui::Separator();

        if (!done) {
            size_t cur = m_findRefsCurrent.load();
            size_t total = m_findRefsTotal.load();
            float frac = total > 0
                ? static_cast<float>(cur) / static_cast<float>(total)
                : 0.0f;
            ImGui::ProgressBar(frac, ImVec2(-1, 0));
            ImGui::Text("Scanning %zu / %zu files...", cur, total);
            if (ImGui::Button("Cancel")) {
                m_findRefsCancel.store(true);
            }
        } else {
            ImGui::Text("Found %zu references", m_findRefsResults.size());
            ImGui::BeginChild("##refs", ImVec2(0, -40), true);
            for (size_t i = 0; i < m_findRefsResults.size(); ++i) {
                const auto& r = m_findRefsResults[i];
                char line[512];
                snprintf(line, sizeof(line),
                         "%s.datc64  |  %s  |  row %u  |  %s",
                         r.table.c_str(), r.column.c_str(),
                         r.row, r.preview.c_str());
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(line)) {
                    pendingNavPath = r.table + ".datc64";
                    pendingNavRow = static_cast<int>(r.row);
                    m_findRefsModalOpen = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();

            const std::string lblCloseRef = IconLabel(m_fa6Available, kIconClose, "Close");
            if (ImGui::Button(lblCloseRef.c_str())) {
                m_findRefsModalOpen = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            const std::string lblCopyAll = IconLabel(m_fa6Available, kIconCopy, "Copy all");
            if (ImGui::Button(lblCopyAll.c_str()) && !m_findRefsResults.empty()) {
                std::string all;
                all.reserve(m_findRefsResults.size() * 64);
                for (const auto& r : m_findRefsResults) {
                    all += r.table + ".datc64\t" + r.column + "\t"
                         + std::to_string(r.row) + "\t" + r.preview + "\n";
                }
                ImGui::SetClipboardText(all.c_str());
            }
        }

        ImGui::EndPopup();
    }

    // User dismissed via X button or click-outside: cancel any ongoing scan.
    if (!stayOpen) {
        m_findRefsModalOpen = false;
        m_findRefsCancel.store(true);
    }
}

// ---------------------------------------------------------------------------
// T21 — Import preview modal + Apply implementation
// ---------------------------------------------------------------------------

void DatViewer::RenderImportModal() {
    if (!m_importConfirmOpen) return;

    if (m_importConfirmOpenRequested) {
        ImGui::OpenPopup("Import preview");
        m_importConfirmOpenRequested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(600, 450), ImGuiCond_Appearing);

    bool stayOpen = true;
    if (ImGui::BeginPopupModal("Import preview", &stayOpen)) {
        // Title line
        ImGui::Text("Format: %s  |  File analyzed", m_pendingImportFormat.c_str());
        ImGui::Separator();

        if (!m_pendingImport.errors.empty()) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
                               "Import FAILED (%zu structural errors)",
                               m_pendingImport.errors.size());
            ImGui::Separator();
            ImGui::BeginChild("##errs", ImVec2(0, -40), true);
            for (const auto& e : m_pendingImport.errors) {
                ImGui::TextWrapped("- %s", e.c_str());
            }
            ImGui::EndChild();
            if (ImGui::Button("OK")) {
                m_pendingImport = {};
                m_importConfirmOpen = false;
                ImGui::CloseCurrentPopup();
            }
        } else {
            int currentRows = static_cast<int>(m_rows.size());
            int newRows = static_cast<int>(m_pendingImport.rows.size());
            int delta = newRows - currentRows;

            ImGui::Text("Current: %d rows", currentRows);
            ImGui::Text("Importing: %d rows (%+d)", newRows, delta);
            ImGui::Text("Warnings: %zu", m_pendingImport.warnings.size());

            if (!m_modifiedCells.empty()) {
                ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1),
                                   "WARNING: %zu pending manual edits will be replaced!",
                                   m_modifiedCells.size());
            }

            ImGui::Separator();

            // Log toggle
            if (!m_pendingImport.warnings.empty()) {
                if (ImGui::Checkbox("Show warning log", &m_importShowLog)) {
                    // just toggle
                }
                if (m_importShowLog) {
                    ImGui::BeginChild("##warns", ImVec2(0, -40), true);
                    for (const auto& w : m_pendingImport.warnings) {
                        ImGui::TextWrapped("- %s", w.c_str());
                    }
                    ImGui::EndChild();
                }
            } else {
                // Fill space with nothing so Apply/Cancel stay at bottom.
                ImGui::Dummy(ImVec2(0, 0));
                // Reserve minimal height so buttons align consistently:
                ImGui::BeginChild("##spacer", ImVec2(0, -40), false);
                ImGui::TextDisabled("(no warnings)");
                ImGui::EndChild();
            }

            const std::string lblImpApply = IconLabel(m_fa6Available, kIconOk, "Apply");
            if (ImGui::Button(lblImpApply.c_str())) {
                ApplyPendingImport();
                m_importConfirmOpen = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                m_pendingImport = {};
                m_importConfirmOpen = false;
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::EndPopup();
    }

    if (!stayOpen) {
        m_pendingImport = {};
        m_importConfirmOpen = false;
    }
}

void DatViewer::ApplyPendingImport() {
    if (!m_pendingImport.valid || !m_table) return;

    // Replace rows + pull interval-seconds side-map out of the pending result
    // BEFORE we move rows (rows and intervalSeconds are independent fields but
    // we want everything off `m_pendingImport` in one pass).
    IntervalSecondMap intervalSeconds = std::move(m_pendingImport.intervalSeconds);
    m_rows = std::move(m_pendingImport.rows);

    // Rebuild m_dat from scratch (row count may have changed)
    auto newBytes = Serialize(m_rows, m_table->columns);
    // Patch interval secondaries back in: Serialize zero-fills them.
    if (!newBytes.empty() && !intervalSeconds.empty()) {
        PatchIntervalSecondHalves(newBytes,
                                  static_cast<uint32_t>(m_rows.size()),
                                  m_table->columns,
                                  intervalSeconds);
    }
    if (newBytes.empty()) {
        // Serialize failed — guard (shouldn't happen if rows are well-formed)
        // Roll back: reparse original bytes
        m_importLog.clear();
        m_importLog.push_back("ERROR: Serialize returned empty; import aborted");
        m_importMode = false;
        // Best-effort rollback: reparse old DAT if we have raw bytes; otherwise leave viewer inconsistent
        if (!m_dat.rawData.empty()) {
            m_dat = Parse(m_dat.rawData);
            // Re-decode rows (caller should probably call DecodeAllRows — here we just mark state)
        }
        m_pendingImport = {};
        return;
    }
    m_dat = Parse(newBytes);
    if (!m_dat.valid) {
        m_importLog.clear();
        m_importLog.push_back("ERROR: Parse(Serialize()) failed - import produced invalid DAT");
        m_importMode = false;
        m_pendingImport = {};
        return;
    }

    // Success — set import mode, clear edits/state
    m_importMode = true;
    m_importLog = std::move(m_pendingImport.warnings);
    m_modifiedCells.clear();
    m_sortedIndices.clear();
    m_expandedArrays.clear();
    m_selectedCell = {};
    m_editor.Close();
    m_highlightRow = -1;
    m_highlightTimer = 0.0f;

    m_pendingImport = {};

    // DatViewer uses m_sortColumn for sorting; RebuildSortOrder will regenerate on next render.
    RebuildSortOrder();
}

} // namespace GGPKExp
