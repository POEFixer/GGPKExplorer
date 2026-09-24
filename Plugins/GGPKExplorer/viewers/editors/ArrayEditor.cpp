#include "../CellEditorPanel.h"
#include "../DatViewer.h"
#include "IconHelpers.h"
#include <imgui.h>
#include <cstring>
#include <string>

namespace GGPKExp {

// -----------------------------------------------------------------------------
// Phase 4 / T14 — Array cell editor.
//
// Renders a scrollable list of per-item primitive editors plus an "Add item"
// / "Clear all" footer. Per-item delete buttons are reserved on the right of
// each row. ForeignRow/Row items use a bare InputInt for v1; the full row
// picker is deferred to T15 (ForeignRowEditor).
// -----------------------------------------------------------------------------

static CellValue DefaultElement(ColumnType t)
{
    switch (t) {
    case ColumnType::Bool:       return false;
    case ColumnType::I16:        return int16_t{0};
    case ColumnType::U16:        return uint16_t{0};
    case ColumnType::I32:
    case ColumnType::EnumRow:    return int32_t{0};
    case ColumnType::U8:
    case ColumnType::U32:        return uint32_t{0};
    case ColumnType::F32:        return 0.0f;
    case ColumnType::String:     return std::string{};
    case ColumnType::Row:
    case ColumnType::ForeignRow: return int64_t{-1};
    default:                     return std::monostate{};
    }
}

static const char* TypeName(ColumnType t)
{
    switch (t) {
    case ColumnType::Bool:       return "bool";
    case ColumnType::I16:        return "i16";
    case ColumnType::U16:        return "u16";
    case ColumnType::I32:
    case ColumnType::EnumRow:    return "i32";
    case ColumnType::U8:         return "u8";
    case ColumnType::U32:        return "u32";
    case ColumnType::F32:        return "f32";
    case ColumnType::String:     return "string";
    case ColumnType::Row:        return "row";
    case ColumnType::ForeignRow: return "foreignrow";
    default:                     return "?";
    }
}

void CellEditorPanel::RenderArray()
{
    if (!m_colDef) return;
    if (!std::holds_alternative<CellArray>(m_buffer)) {
        m_buffer = CellArray{};
    }
    auto& arr = std::get<CellArray>(m_buffer);

    // FA6 gate — pulled from the owning DatViewer so delete/add/clear buttons
    // pick glyphs when the host atlas has FA6 merged.
    const bool fa6 = m_owner && m_owner->IsFA6Available();

    // ---- Header: element type + optional refTable / refFile / refFiles ----
    if (!m_colDef->refTable.empty()) {
        ImGui::Text("Element type: %s -> %s", TypeName(m_colDef->type), m_colDef->refTable.c_str());
    } else {
        ImGui::Text("Element type: %s", TypeName(m_colDef->type));
    }
    if (!m_colDef->refFile.empty()) {
        ImGui::TextDisabled("refFile: %s", m_colDef->refFile.c_str());
    }
    if (!m_colDef->refFiles.empty()) {
        std::string joined;
        for (const auto& f : m_colDef->refFiles) {
            if (!joined.empty()) joined += ", ";
            joined += f;
        }
        ImGui::TextDisabled("refFiles: [%s]", joined.c_str());
    }

    ImGui::Separator();

    // ---- Items list (scrollable child, footer reserved at bottom) ----
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() * 2.0f + 8.0f;
    ImGui::BeginChild("##arr_items", ImVec2(0.0f, -footerHeight), true);

    int removeIdx = -1;
    for (size_t i = 0; i < arr.items.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::Text("[%zu]", i);
        ImGui::SameLine();

        auto& item = arr.items[i];

        // Reserve room on the right for the per-item "X" delete button so
        // the input field fills the rest of the row.
        const float deleteBtnW = 30.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - deleteBtnW - 8.0f);

        switch (m_colDef->type) {
        case ColumnType::String: {
            std::string s = std::holds_alternative<std::string>(item)
                              ? std::get<std::string>(item)
                              : std::string{};
            char buf[1024];
            strncpy_s(buf, sizeof(buf), s.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText("##v", buf, sizeof(buf))) {
                item = std::string(buf);
                m_dirty = true;
            }
            break;
        }
        case ColumnType::I32:
        case ColumnType::EnumRow: {
            int32_t v = std::holds_alternative<int32_t>(item) ? std::get<int32_t>(item) : 0;
            if (ImGui::InputInt("##v", &v, 0, 0)) {
                item = v;
                m_dirty = true;
            }
            break;
        }
        case ColumnType::U8:
        case ColumnType::U32: {
            uint32_t uv = std::holds_alternative<uint32_t>(item) ? std::get<uint32_t>(item) : 0u;
            int32_t v = static_cast<int32_t>(uv);
            if (ImGui::InputInt("##v", &v, 0, 0) &&
                (m_colDef->type != ColumnType::U8 || (v >= 0 && v <= 255))) {
                item = static_cast<uint32_t>(v);
                m_dirty = true;
            }
            break;
        }
        case ColumnType::I16:
        case ColumnType::U16: {
            int32_t v = 0;
            if (std::holds_alternative<int16_t>(item))       v = std::get<int16_t>(item);
            else if (std::holds_alternative<uint16_t>(item)) v = static_cast<int32_t>(std::get<uint16_t>(item));
            if (ImGui::InputInt("##v", &v, 0, 0)) {
                if (m_colDef->type == ColumnType::I16) item = static_cast<int16_t>(v);
                else                                   item = static_cast<uint16_t>(v);
                m_dirty = true;
            }
            break;
        }
        case ColumnType::Row:
        case ColumnType::ForeignRow: {
            int64_t v64 = std::holds_alternative<int64_t>(item) ? std::get<int64_t>(item) : int64_t{-1};
            int32_t v = static_cast<int32_t>(v64);
            if (ImGui::InputInt("##v", &v, 0, 0)) {
                item = static_cast<int64_t>(v);
                m_dirty = true;
            }
            break;
        }
        case ColumnType::F32: {
            float f = std::holds_alternative<float>(item) ? std::get<float>(item) : 0.0f;
            if (ImGui::InputFloat("##v", &f, 0.0f, 0.0f, "%.6g")) {
                item = f;
                m_dirty = true;
            }
            break;
        }
        case ColumnType::Bool: {
            bool b = std::holds_alternative<bool>(item) ? std::get<bool>(item) : false;
            if (ImGui::Checkbox("##v", &b)) {
                item = b;
                m_dirty = true;
            }
            break;
        }
        default:
            ImGui::TextDisabled("(unsupported element type)");
            break;
        }

        ImGui::SameLine();
        // Per-item delete — icon-only when FA6 present, "X" text fallback.
        const std::string delLbl = fa6 ? std::string(kIconDelete) : std::string("X");
        if (ImGui::SmallButton(delLbl.c_str())) {
            removeIdx = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Deferred removal — safe after the loop/child has closed.
    if (removeIdx >= 0) {
        arr.items.erase(arr.items.begin() + removeIdx);
        m_dirty = true;
    }

    // ---- Footer: Add / Clear / count ----
    const std::string lblAdd = IconLabel(fa6, kIconAdd, "Add item");
    if (ImGui::Button(lblAdd.c_str())) {
        arr.items.push_back(DefaultElement(m_colDef->type));
        m_dirty = true;
    }
    ImGui::SameLine();
    const std::string lblClearAll = IconLabel(fa6, kIconClear, "Clear all");
    if (ImGui::Button(lblClearAll.c_str()) && !arr.items.empty()) {
        ImGui::OpenPopup("Confirm clear array");
    }
    ImGui::SameLine();
    ImGui::Text("(%zu items)", arr.items.size());

    if (ImGui::BeginPopupModal("Confirm clear array", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Clear all %zu items? This resets the array to [].", arr.items.size());
        if (ImGui::Button("Yes, clear")) {
            arr.items.clear();
            m_dirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace GGPKExp
