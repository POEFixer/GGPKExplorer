#pragma once
#include "IViewer.h"
#include "../core/TextEncoding.h"
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <functional>
#include <utility>
#include <optional>

// Forward declarations
class TextEditor;
struct ImFont;

namespace GGPKExp {

struct PathLink {
    std::string path;   // extracted path without quotes
    int startCol;       // column of opening quote (inclusive)
    int endCol;         // column after closing quote (exclusive)
};

class TextViewer : public IViewer {
public:
    TextViewer();
    ~TextViewer();

    void LoadData(std::vector<uint8_t> data, const std::string& path) override;
    void Clear() override;
    void Render() override;

    // Navigation support
    bool HasPendingNavigation() const override { return !pendingNavPath.empty(); }
    std::string TakePendingNavigation() override {
        std::string path = std::move(pendingNavPath);
        pendingNavPath.clear();
        return path;
    }

    bool HasUnsavedChanges() const;
    std::string GetText() const;
    TextEncoding GetOriginalEncoding() const { return m_originalEncoding; }
    bool OriginalHadBom() const { return m_originalHadBom; }
    void SetOnSave(std::function<void(const std::string&)> cb) { m_onSave = std::move(cb); }
    void ScrollToLine(int line);

    // Hyperlink navigation
    std::string pendingNavPath;     // set on Ctrl+Click, consumed by ContentPanel
    void ShowNotFound(const std::string& path);

private:
    std::unique_ptr<TextEditor> m_editor;
    std::string m_filename;
    int m_undoIndexAtLoad = 0;
    TextEncoding m_originalEncoding = TextEncoding::UTF8;
    bool m_originalHadBom = false;

    // Save callback + progress
    std::function<void(const std::string&)> m_onSave;
    bool m_isSaving = false;

    // Monospace font cache (non-owning, lifetime managed by ImGui font atlas): valid for the
    // atlas it was found in, identified by the atlas and its first font's FontId (an atlas
    // rebuild frees every ImFont and numbers the new ones afresh).
    ::ImFont* m_monoFont = nullptr;
    bool m_monoFontSearched = false;
    const void* m_monoFontAtlas = nullptr;
    unsigned int m_monoFontAtlasFontId = 0;

    // Hyperlink overlay state
    std::string m_notFoundPath;
    float m_notFoundTimer = 0.0f;

    static std::optional<PathLink> FindPathAtColumn(const std::string& lineText, int column);

    void EnsureEditor();
    static ::ImFont* FindMonoFont();
    void SelectGrammar(const std::string& filename);
    std::string DetectAndConvertEncoding(const std::vector<uint8_t>& data);
};

} // namespace GGPKExp
