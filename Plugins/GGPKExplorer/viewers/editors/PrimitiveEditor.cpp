#include "../CellEditorPanel.h"
#include "../DatViewer.h"
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <charconv>
#include <string>
#include <cfloat>

namespace GGPKExp {

// -----------------------------------------------------------------------------
// Phase 4 / T13 — Primitive cell editor.
//
// Handles inline editing of scalar column types: Bool, U8, I16, U16, I32, U32,
// F32, EnumRow and String. Integer inputs share a unified decimal-text parse
// path using std::from_chars. Strings use a fixed-size thread_local buffer to
// avoid per-frame reinit churn while the same panel is focused.
// -----------------------------------------------------------------------------

void CellEditorPanel::RenderPrimitive()
{
    if (!m_colDef) return;

    switch (m_colDef->type) {
    case ColumnType::Bool: {
        bool b = std::holds_alternative<bool>(m_buffer) ? std::get<bool>(m_buffer) : false;
        if (ImGui::Checkbox("Value", &b)) {
            m_buffer = b;
            m_dirty = true;
        }
        break;
    }
    case ColumnType::U8:
    case ColumnType::I16:
    case ColumnType::U16:
    case ColumnType::I32:
    case ColumnType::U32:
    case ColumnType::EnumRow: {
        char buf[32];
        int64_t cur = 0;
        if (std::holds_alternative<int16_t>(m_buffer))       cur = std::get<int16_t>(m_buffer);
        else if (std::holds_alternative<uint16_t>(m_buffer)) cur = std::get<uint16_t>(m_buffer);
        else if (std::holds_alternative<int32_t>(m_buffer))  cur = std::get<int32_t>(m_buffer);
        else if (std::holds_alternative<uint32_t>(m_buffer)) cur = std::get<uint32_t>(m_buffer);
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(cur));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##val", buf, sizeof(buf),
                             ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_CharsNoBlank)) {
            int64_t parsed = 0;
            size_t len = std::strlen(buf);
            auto res = std::from_chars(buf, buf + len, parsed);
            if (res.ec == std::errc() &&
                (m_colDef->type != ColumnType::U8 ||
                 (res.ptr == buf + len && parsed >= 0 && parsed <= 255))) {
                switch (m_colDef->type) {
                case ColumnType::U8:
                    m_buffer = static_cast<uint32_t>(parsed);
                    break;
                case ColumnType::I16:
                    m_buffer = static_cast<int16_t>(parsed);
                    break;
                case ColumnType::U16:
                    m_buffer = static_cast<uint16_t>(parsed);
                    break;
                case ColumnType::I32:
                case ColumnType::EnumRow:
                    m_buffer = static_cast<int32_t>(parsed);
                    break;
                case ColumnType::U32:
                    m_buffer = static_cast<uint32_t>(parsed);
                    break;
                default:
                    break;
                }
                m_dirty = true;
            }
        }
        const char* typeLabel =
            m_colDef->type == ColumnType::U8 ? "u8" :
            m_colDef->type == ColumnType::I16 ? "i16" :
            m_colDef->type == ColumnType::U16 ? "u16" :
            m_colDef->type == ColumnType::I32 ? "i32" :
            m_colDef->type == ColumnType::U32 ? "u32" : "enum(i32)";
        ImGui::TextDisabled("Type: %s", typeLabel);
        break;
    }
    case ColumnType::F32: {
        float f = std::holds_alternative<float>(m_buffer) ? std::get<float>(m_buffer) : 0.0f;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputFloat("##val", &f, 0.0f, 0.0f, "%.6g")) {
            m_buffer = f;
            m_dirty = true;
        }
        ImGui::TextDisabled("Type: f32");
        break;
    }
    case ColumnType::String: {
        std::string s = std::holds_alternative<std::string>(m_buffer)
                          ? std::get<std::string>(m_buffer)
                          : std::string{};
        // Fixed-size scratch buffer — only one primitive editor is active in this
        // plugin at a time, so a thread_local arena is safe and avoids per-frame
        // reinit cost when the same cell stays focused.
        static thread_local char buf[4096];
        strncpy_s(buf, sizeof(buf), s.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextMultiline("##val", buf, sizeof(buf),
                                      ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4))) {
            m_buffer = std::string(buf);
            m_dirty = true;
        }
        ImGui::TextDisabled("Type: string");
        break;
    }
    default:
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "Unsupported primitive type");
        break;
    }
}

} // namespace GGPKExp
