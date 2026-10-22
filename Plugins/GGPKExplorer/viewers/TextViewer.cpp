#include "TextViewer.h"
#pragma warning(push, 0)
#include <TextEditor.h>
#pragma warning(pop)
#include <imgui.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace GGPKExp {

TextViewer::TextViewer() = default;

void TextViewer::EnsureEditor() {
    if (!m_editor) {
        m_editor = std::make_unique<TextEditor>();
        m_editor->SetReadOnlyEnabled(false);
        m_editor->SetShowWhitespacesEnabled(false);
    }
}

TextViewer::~TextViewer() = default;

void TextViewer::LoadData(std::vector<uint8_t> data, const std::string& path) {
    m_filename = path;
    m_error.clear();
    try {
        std::string text = DetectAndConvertEncoding(data);

        // Strip embedded null bytes that can crash TextEditor
        text.erase(std::remove(text.begin(), text.end(), '\0'), text.end());

        EnsureEditor();
        m_editor->SetText(text);
        m_editor->SetReadOnlyEnabled(false);
        m_undoIndexAtLoad = m_editor->GetUndoIndex();
        m_loaded = true;
        SelectGrammar(path);
    } catch (const std::exception& e) {
        m_error = std::string("Failed to load: ") + e.what();
        m_loaded = false;
    } catch (...) {
        m_error = "Failed to load: unknown error";
        m_loaded = false;
    }
}

void TextViewer::Clear() {
    if (m_editor) m_editor->SetText("");
    m_loaded = false;
    m_filename.clear();
    m_error.clear();
    m_undoIndexAtLoad = 0;
    pendingNavPath.clear();
    m_notFoundPath.clear();
    m_notFoundTimer = 0.0f;
}

void TextViewer::Render() {
    if (!m_loaded) {
        if (!m_error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error: %s", m_error.c_str());
        } else {
            ImGui::TextDisabled("No text loaded");
        }
        return;
    }

    // ---- Save button (only when changes exist) ----
    {
        if (HasUnsavedChanges() && m_onSave) {
            if (ImGui::Button("Save")) {
                m_isSaving = true;
                m_onSave(m_editor->GetText());
                m_undoIndexAtLoad = m_editor->GetUndoIndex();
                m_isSaving = false;
            }
            ImGui::SameLine();
        }
        ImGui::TextDisabled("%s", m_filename.c_str());
    }

    // ---- Text editor (monospace font required for correct positioning) ----
    if (!m_monoFontSearched) {
        m_monoFont = FindMonoFont();
        m_monoFontSearched = true;
    }
    if (m_monoFont) ImGui::PushFont(m_monoFont, m_monoFont->LegacySize);
    m_editor->Render("##TextViewer");
    if (m_monoFont) ImGui::PopFont();

    // ---- Hyperlink overlay (Ctrl+Hover = underline, Ctrl+Click = navigate) ----
    {
        ImVec2 editorMin = ImGui::GetItemRectMin();
        ImVec2 editorMax = ImGui::GetItemRectMax();
        ImVec2 mousePos = ImGui::GetMousePos();

        // Only process when Ctrl is held and mouse is over the editor area
        bool ctrlHeld = ImGui::GetIO().KeyCtrl;
        bool mouseInEditor = mousePos.x >= editorMin.x && mousePos.x <= editorMax.x &&
                             mousePos.y >= editorMin.y && mousePos.y <= editorMax.y;

        if (ctrlHeld && mouseInEditor && m_editor->GetLineCount() > 0) {
            ImVec2 contentOrigin = m_editor->GetContentScreenPos();
            float charW = m_editor->GetCharAdvanceX();
            float lineH = m_editor->GetLineHeight();
            float textStartX = m_editor->GetTextStartX();
            int tabSize = m_editor->GetTabSize();

            // Convert mouse position to text coordinates
            int hoveredLine = static_cast<int>(floorf((mousePos.y - contentOrigin.y) / lineH));
            int hoveredVisualCol = static_cast<int>(floorf((mousePos.x - contentOrigin.x - textStartX) / charW));

            // Clamp to valid range
            int firstVisible = m_editor->GetFirstVisibleLine();
            int lastVisible = m_editor->GetLastVisibleLine();
            if (hoveredLine >= firstVisible && hoveredLine <= lastVisible &&
                hoveredLine >= 0 && hoveredLine < m_editor->GetLineCount() && hoveredVisualCol >= 0) {

                std::string lineText = m_editor->GetLineText(hoveredLine);

                // Convert visual column to raw character index (tabs expand to multiple visual columns)
                int hoveredRawCol = 0;
                {
                    int vc = 0;
                    for (int i = 0; i < static_cast<int>(lineText.size()); ++i) {
                        if (vc >= hoveredVisualCol) break;
                        if (lineText[i] == '\t')
                            vc += tabSize - (vc % tabSize);
                        else
                            vc++;
                        hoveredRawCol = i + 1;
                    }
                }

                auto link = FindPathAtColumn(lineText, hoveredRawCol);

                if (link) {
                    // Convert raw startCol/endCol to visual columns for screen position
                    auto rawToVisual = [&](int rawCol) -> int {
                        int vc = 0;
                        for (int i = 0; i < rawCol && i < static_cast<int>(lineText.size()); ++i) {
                            if (lineText[i] == '\t')
                                vc += tabSize - (vc % tabSize);
                            else
                                vc++;
                        }
                        return vc;
                    };
                    int visualStart = rawToVisual(link->startCol);
                    int visualEnd = rawToVisual(link->endCol);

                    // Compute underline screen coordinates using visual columns
                    float startX = contentOrigin.x + textStartX + visualStart * charW;
                    float endX = contentOrigin.x + textStartX + visualEnd * charW;
                    float lineY = contentOrigin.y + hoveredLine * lineH + lineH - 1.0f;

                    // Clip to editor bounds
                    startX = (startX < editorMin.x) ? editorMin.x : startX;
                    endX = (endX > editorMax.x) ? editorMax.x : endX;

                    if (startX < endX && lineY >= editorMin.y && lineY <= editorMax.y) {
                        auto* drawList = ImGui::GetForegroundDrawList();
                        drawList->AddLineH(startX, endX, lineY, IM_COL32(102, 204, 255, 230), 1.0f);
                    }

                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("Navigate to %s", link->path.c_str());

                    // Ctrl+Click -> set pending navigation
                    if (ImGui::IsMouseClicked(0)) {
                        pendingNavPath = link->path;
                    }
                }
            }
        }
    }

    // ---- "File not found" toast (fades over 2 seconds) ----
    if (m_notFoundTimer > 0.0f) {
        float dt = ImGui::GetIO().DeltaTime;
        m_notFoundTimer -= dt;

        float alpha = (m_notFoundTimer > 1.0f) ? 1.0f : m_notFoundTimer;
        if (alpha > 0.0f) {
            char toastBuf[256];
            snprintf(toastBuf, sizeof(toastBuf), "File not found: %s", m_notFoundPath.c_str());
            ImVec2 textSize = ImGui::CalcTextSize(toastBuf);
            ImVec2 winPos = ImGui::GetWindowPos();
            ImVec2 winSize = ImGui::GetWindowSize();
            float px = winPos.x + winSize.x - textSize.x - 16.0f;
            float py = winPos.y + 8.0f;

            auto* drawList = ImGui::GetForegroundDrawList();
            ImU32 bgCol = IM_COL32(40, 0, 0, static_cast<int>(180 * alpha));
            ImU32 textCol = IM_COL32(255, 100, 100, static_cast<int>(255 * alpha));
            drawList->AddRectFilled(
                ImVec2(px - 6.0f, py - 2.0f),
                ImVec2(px + textSize.x + 6.0f, py + textSize.y + 2.0f),
                bgCol, 4.0f);
            drawList->AddText(ImVec2(px, py), textCol, toastBuf);
        }

        if (m_notFoundTimer <= 0.0f) {
            m_notFoundTimer = 0.0f;
            m_notFoundPath.clear();
        }
    }

    // ---- Saving spinner overlay ----
    if (m_isSaving) {
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
        ImVec2 textSize = ImGui::CalcTextSize("Saving...");
        drawList->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y + radius + 10.0f),
            IM_COL32(255, 255, 255, 255), "Saving...");
    }
}

bool TextViewer::HasUnsavedChanges() const {
    return m_editor && m_editor->GetUndoIndex() != m_undoIndexAtLoad;
}

std::string TextViewer::GetText() const {
    return m_editor ? m_editor->GetText() : std::string{};
}

void TextViewer::ScrollToLine(int line) {
    if (!m_loaded || line < 1) return;
    m_editor->SetViewAtLine(line - 1, TextEditor::SetViewAtLineMode::Centered);
    m_editor->SetCursorPosition(line - 1, 0);
}

void TextViewer::ShowNotFound(const std::string& path) {
    m_notFoundPath = path;
    m_notFoundTimer = 2.0f;
}

std::optional<PathLink> TextViewer::FindPathAtColumn(const std::string& lineText, int column) {
    size_t pos = 0;
    while (pos < lineText.size()) {
        // Find next opening quote
        size_t openQuote = lineText.find('"', pos);
        if (openQuote == std::string::npos)
            break;

        // Find closing quote
        size_t closeQuote = lineText.find('"', openQuote + 1);
        if (closeQuote == std::string::npos)
            break;

        int startCol = static_cast<int>(openQuote);
        int endCol = static_cast<int>(closeQuote + 1); // past closing quote

        // Check if cursor is within this quoted range
        if (column >= startCol && column < endCol) {
            std::string content = lineText.substr(openQuote + 1, closeQuote - openQuote - 1);

            // Rule 1: must contain at least one '/'
            if (content.find('/') == std::string::npos) {
                pos = closeQuote + 1;
                continue;
            }

            // Rule 2: must end with .extension (1-10 latin chars)
            auto dotPos = content.rfind('.');
            if (dotPos == std::string::npos || dotPos == content.size() - 1) {
                pos = closeQuote + 1;
                continue;
            }
            std::string ext = content.substr(dotPos + 1);
            if (ext.empty() || ext.size() > 10) {
                pos = closeQuote + 1;
                continue;
            }
            bool validExt = true;
            for (char c : ext) {
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
                    validExt = false;
                    break;
                }
            }
            if (!validExt) {
                pos = closeQuote + 1;
                continue;
            }

            return PathLink{ content, startCol, endCol };
        }

        pos = closeQuote + 1;
    }
    return std::nullopt;
}

void TextViewer::SelectGrammar(const std::string& filename) {
    auto dot = filename.rfind('.');
    if (dot == std::string::npos) return;
    std::string ext = filename.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".lua") {
        m_editor->SetLanguageDefinition(TextEditor::LanguageDefinitionId::Lua);
    } else if (ext == ".hlsl" || ext == ".fx" || ext == ".ffx") {
        m_editor->SetLanguageDefinition(TextEditor::LanguageDefinitionId::Hlsl);
    } else if (ext == ".glsl") {
        m_editor->SetLanguageDefinition(TextEditor::LanguageDefinitionId::Glsl);
    } else if (ext == ".json" || ext == ".hideout" || ext == ".atl") {
        m_editor->SetLanguageDefinition(TextEditor::LanguageDefinitionId::Json);
    } else if (ext == ".py") {
        m_editor->SetLanguageDefinition(TextEditor::LanguageDefinitionId::Python);
    } else if (ext == ".sql") {
        m_editor->SetLanguageDefinition(TextEditor::LanguageDefinitionId::Sql);
    } else if (ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".c" || ext == ".cc") {
        m_editor->SetLanguageDefinition(TextEditor::LanguageDefinitionId::Cpp);
    } else if (ext == ".cs") {
        m_editor->SetLanguageDefinition(TextEditor::LanguageDefinitionId::Cs);
    } else if (ext == ".csd") {
        m_editor->SetLanguageDefinition(TextEditor::LanguageDefinitionId::Csd);
    } else if (ext == ".ot" || ext == ".otc" || ext == ".it" || ext == ".itc" ||
               ext == ".pet" || ext == ".sm" || ext == ".trl" ||
               ext == ".act" || ext == ".ao" || ext == ".amd" || ext == ".arm" ||
               ext == ".env" || ext == ".epk" || ext == ".mat" || ext == ".mtd" ||
               ext == ".tgr" || ext == ".tgt" || ext == ".tsi" || ext == ".rs" ||
               ext == ".dgr" || ext == ".tmd" || ext == ".cht" || ext == ".ecf" ||
               ext == ".dct" || ext == ".dlp" || ext == ".ddt" || ext == ".fgp" ||
               ext == ".gft" || ext == ".toy" || ext == ".tst" || ext == ".gt" ||
               ext == ".tmo" || ext == ".et" || ext == ".clt" || ext == ".fxgraph" ||
               ext == ".atlas" || ext == ".ai" || ext == ".cfg" || ext == ".red" ||
               ext == ".ui") {
        m_editor->SetLanguageDefinition(TextEditor::LanguageDefinitionId::Poe);
    } else {
        m_editor->SetLanguageDefinition(TextEditor::LanguageDefinitionId::None);
    }
}

::ImFont* TextViewer::FindMonoFont() {
    auto& io = ImGui::GetIO();
    float fontSize = ImGui::GetFontSize();
    for (int i = 0; i < io.Fonts->Fonts.Size; ++i) {
        ImFont* f = io.Fonts->Fonts[i];
        if (!f->IsLoaded()) continue;
        float wW = f->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, "W").x;
        float wi = f->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, "i").x;
        if (std::abs(wW - wi) < 0.5f) return f;
    }
    return nullptr;
}

std::string TextViewer::DetectAndConvertEncoding(const std::vector<uint8_t>& data) {
    if (data.empty()) return {};

    // Use the shared encoding detection utility
    m_originalEncoding = GGPKExp::DetectEncoding(data.data(), data.size());

    // Track whether original had a BOM
    m_originalHadBom = false;
    if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
        m_originalHadBom = true;
    else if (data.size() >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF)))
        m_originalHadBom = true;

    return GGPKExp::DecodeToUtf8(data.data(), data.size(), m_originalEncoding);
}

} // namespace GGPKExp
