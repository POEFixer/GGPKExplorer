#include "ExtractDialog.h"
#include "UiText.h"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <unordered_map>
#include <set>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <commdlg.h>
#include <shlobj.h>

namespace fs = std::filesystem;

// Helper: Convert wide string to UTF-8 (duplicated from GGPKExplorer.cpp)
static std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr || !wstr[0]) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string result(needed - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), needed, nullptr, nullptr);
    return result;
}

// Helper: Convert UTF-8 to wide string (duplicated from GGPKExplorer.cpp)
static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(needed - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, result.data(), needed);
    return result;
}

namespace GGPKExp {

// Returns length of parent path of `path` (everything up to and including
// the last '/'). Returns 0 if `path` has no '/' — used by the extract
// functions so the selected folder's own name is preserved in the output.
static size_t ParentPrefixLen(const std::string& path) {
    auto slash = path.rfind('/');
    return (slash != std::string::npos) ? slash + 1 : 0;
}

// Helper: format and log via callback
static void LogFmt(const ExtractDialog::LogCallback& log, const char* category, const char* fmt, ...) {
    if (!log) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log(category, buf);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ExtractDialog::JoinExtractThread() {
    m_extractRunning.store(false);
    if (m_extractThread.joinable())
        m_extractThread.join();
}

// ---------------------------------------------------------------------------
// Open Extract Options modal
// ---------------------------------------------------------------------------

void ExtractDialog::OpenExtractOptions(const std::string& dirPath) {
    m_extractOptions.targetDirPath = dirPath;
    CollectExtensionStats(dirPath);
    m_showExtractOptions = true;
}

// ---------------------------------------------------------------------------
// CollectExtensionStats
// ---------------------------------------------------------------------------

void ExtractDialog::CollectExtensionStats(const std::string& dirPath) {
    m_extractOptions.filters.clear();

    if (!m_tree) return;

    std::unordered_map<std::string, int> extCounts;

    m_tree->ForEachFileUnder(dirPath, [&](const std::string& /*fullPath*/, const std::string& name, int64_t /*fileSize*/, void* /*nodePtr*/) {
        std::string ext;
        auto dot = name.rfind('.');
        if (dot != std::string::npos) {
            ext = name.substr(dot);
            for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            ext = "no_extension";
        }
        extCounts[ext]++;
    });

    m_extractOptions.filters.reserve(extCounts.size());
    for (auto& [ext, count] : extCounts) {
        m_extractOptions.filters.push_back({ext, count, true});
    }
    std::sort(m_extractOptions.filters.begin(), m_extractOptions.filters.end(),
        [](const ExtractFilter& a, const ExtractFilter& b) { return a.count > b.count; });
}

// ---------------------------------------------------------------------------
// RenderExtractOptions (modal UI)
// ---------------------------------------------------------------------------

void ExtractDialog::RenderExtractOptions() {
    if (!m_showExtractOptions) return;

    const char* title = I18n::TrId("extract_title");
    ImGui::OpenPopup(title);
    ImGui::SetNextWindowSize(ImVec2(450, 600), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(350, 400), ImVec2(FLT_MAX, FLT_MAX));

    if (ImGui::BeginPopupModal(title, &m_showExtractOptions,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {

        // Header with total count
        int totalIncluded = 0;
        for (const auto& f : m_extractOptions.filters)
            if (f.included) totalIncluded += f.count;
        ImGui::TextUnformatted(I18n::Tr("extract_step_types"));
        ImGui::SameLine(0.0f, ImGui::GetFontSize());
        ImGui::TextDisabled("%s", I18n::TrF("extract_selected_count", { { "count", Num("%d", totalIncluded) } }).c_str());

        ImGui::Spacing();

        // Calculate fixed bottom height: size restrictions + buttons
        float lineH = ImGui::GetFrameHeightWithSpacing();
        float bottomH = lineH * 4 + ImGui::GetStyle().ItemSpacing.y * 4 + 8.0f; // restrictions + buttons + separators
        if (m_extractOptions.skipLargeFiles) bottomH += lineH; // extra line for size input

        // Table fills remaining space
        float tableH = ImGui::GetContentRegionAvail().y - bottomH;
        if (tableH < 80.0f) tableH = 80.0f;

        ImGuiTableFlags tableFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg
            | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit;

        if (ImGui::BeginTable("##ExtFilters", 3, tableFlags, ImVec2(-1, tableH))) {
            // Fixed columns fit their translated header and cells (width 0: not resizable).
            ImGui::TableSetupColumn(I18n::TrId("extract_col_include"),   ImGuiTableColumnFlags_WidthFixed, 0.0f);
            ImGui::TableSetupColumn(I18n::TrId("extract_col_extension"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(I18n::TrId("extract_col_count"),     ImGuiTableColumnFlags_WidthFixed, 0.0f);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_extractOptions.filters.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    auto& f = m_extractOptions.filters[static_cast<size_t>(i)];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushID(i);
                    ImGui::Checkbox("##inc", &f.included);
                    ImGui::PopID();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(f.extension == "no_extension" ? I18n::Tr("extract_no_extension")
                                                                         : f.extension.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", f.count);
                }
            }
            clipper.End();
            ImGui::EndTable();
        }

        // Size restrictions
        ImGui::Separator();
        ImGui::TextUnformatted(I18n::Tr("extract_step_size"));
        ImGui::Checkbox(I18n::TrId("extract_skip_larger"), &m_extractOptions.skipLargeFiles);
        if (m_extractOptions.skipLargeFiles) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
            int maxSize = static_cast<int>(m_extractOptions.maxFileSize);
            if (ImGui::InputInt("##maxSize", &maxSize, 0, 0)) {
                if (maxSize < 1) maxSize = 1;
                m_extractOptions.maxFileSize = maxSize;
            }
            ImGui::SameLine();
            const char* units[] = { I18n::Tr("unit_kb"), I18n::Tr("unit_mb"), I18n::Tr("unit_gb") };
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.5f);
            ImGui::Combo("##sizeUnit", &m_extractOptions.sizeUnit, units, 3);
        }

        // Buttons row
        ImGui::Separator();

        const char* selectAllLabel = I18n::TrId("extract_select_all");
        const char* selectNoneLabel = I18n::TrId("extract_select_none");
        const char* extractLabel = I18n::TrId("extract_extract");
        const char* cancelLabel = I18n::TrId("common_cancel");
        if (ImGui::Button(selectAllLabel, ImVec2(ButtonWidth(selectAllLabel, 7.5f), 0))) {
            for (auto& f : m_extractOptions.filters) f.included = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(selectNoneLabel, ImVec2(ButtonWidth(selectNoneLabel, 7.5f), 0))) {
            for (auto& f : m_extractOptions.filters) f.included = false;
        }
        ImGui::SameLine(0, 20);
        if (ImGui::Button(extractLabel, ImVec2(ButtonWidth(extractLabel, 7.0f), 0))) {
            m_showExtractOptions = false;
            ImGui::CloseCurrentPopup();
            ExtractDirectoryFiltered();
        }
        ImGui::SameLine();
        if (ImGui::Button(cancelLabel, ImVec2(ButtonWidth(cancelLabel, 7.0f), 0))) {
            m_showExtractOptions = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// ExtractFile
// ---------------------------------------------------------------------------

void ExtractDialog::DoExtractFile() {
    if (!m_treePanel || !m_tree || !m_manager) return;

    const auto& selPath = m_treePanel->GetSelectedPath();
    if (selPath.empty()) return;

    uint32_t idx = m_tree->FindByPath(selPath);
    if (idx == UINT32_MAX) return;

    const auto& flatList = m_tree->GetFlatList();
    const auto& entry = flatList[idx];
    if (entry.isDirectory || !entry.nodePtr) return;

    // Determine default filename and filter from extension
    std::wstring wideName = Utf8ToWide(entry.name);

    // Double-null-terminated filter string for OPENFILENAMEW, labels in the program language.
    DialogFilter filter;
    auto dotPos = entry.name.rfind('.');
    if (dotPos != std::string::npos) {
        std::string ext = entry.name.substr(dotPos);
        std::string extUpper = ext.substr(1);
        for (auto& c : extUpper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        filter.Add(I18n::TrF("dialog_filter_ext_files", { { "ext", extUpper } }), L"*" + Utf8ToWide(ext));
    }
    filter.Add(I18n::Tr("dialog_filter_all_files"), L"*.*");
    const std::wstring title = Wide(I18n::Tr("dialog_extract_file_as"));

    wchar_t savePath[MAX_PATH] = {};
    if (wideName.size() < MAX_PATH)
        wcsncpy_s(savePath, wideName.c_str(), wideName.size());

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = filter.Get();
    ofn.lpstrFile = savePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = title.c_str();

    if (!GetSaveFileNameW(&ofn)) return;

    // Read from GGPK
    auto readResult = m_manager->ReadFile(entry.nodePtr);
    if (!readResult || readResult.value.empty()) {
        LogFmt(m_log, "Error", "GGPKExplorer: failed to read file: %s", selPath.c_str());
        return;
    }
    auto& data = readResult.value;

    // Write to disk
    std::ofstream outFile{ fs::path(savePath), std::ios::binary };
    if (!outFile.is_open()) {
        LogFmt(m_log, "Error", "GGPKExplorer: failed to write to disk");
        return;
    }
    outFile.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    outFile.close();

    LogFmt(m_log, "Info", "GGPKExplorer: extracted %s (%lld bytes)", entry.name.c_str(),
           static_cast<long long>(data.size()));
}

// ---------------------------------------------------------------------------
// ReplaceFile
// ---------------------------------------------------------------------------

void ExtractDialog::DoReplaceFile() {
    if (!m_treePanel || !m_tree || !m_manager) return;

    const auto& selPath = m_treePanel->GetSelectedPath();
    if (selPath.empty()) return;

    uint32_t idx = m_tree->FindByPath(selPath);
    if (idx == UINT32_MAX) return;

    const auto& flatList = m_tree->GetFlatList();
    const auto& entry = flatList[idx];
    if (entry.isDirectory || !entry.nodePtr) return;

    wchar_t filePath[MAX_PATH] = {};
    DialogFilter filter;
    filter.Add(I18n::Tr("dialog_filter_all_files"), L"*.*");
    const std::wstring title = Wide(I18n::Tr("dialog_select_replacement"));
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = filter.Get();
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = title.c_str();

    if (!GetOpenFileNameW(&ofn)) return;

    // Read replacement from disk
    std::ifstream inFile{ fs::path(filePath), std::ios::binary | std::ios::ate };
    if (!inFile.is_open()) {
        LogFmt(m_log, "Error", "GGPKExplorer: failed to read replacement file");
        return;
    }

    auto fileSize = inFile.tellg();
    inFile.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(fileSize));
    inFile.read(reinterpret_cast<char*>(data.data()), fileSize);
    inFile.close();

    // Write into GGPK
    if (!m_manager->WriteFile(entry.nodePtr, data)) {
        LogFmt(m_log, "Error", "GGPKExplorer: WriteFile failed for %s", selPath.c_str());
        return;
    }

    LogFmt(m_log, "Info", "GGPKExplorer: replaced %s (%lld bytes)", entry.name.c_str(),
           static_cast<long long>(data.size()));

    // Reload in content panel
    if (m_contentPanel) {
        m_contentPanel->LoadFile(entry.fullPath, entry.name, static_cast<int64_t>(data.size()),
                                  entry.type, entry.nodePtr);
    }
}

// ---------------------------------------------------------------------------
// ExtractDirectory
// ---------------------------------------------------------------------------

void ExtractDialog::DoExtractDirectory() {
    if (!m_treePanel || !m_tree || !m_manager || !m_statusBar) return;

    const auto& selPath = m_treePanel->GetSelectedPath();
    if (selPath.empty()) return;

    // Already extracting?
    if (m_extractRunning.load()) {
        LogFmt(m_log, "Info", "GGPKExplorer: extraction already in progress");
        return;
    }

    // Browse for output directory
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool comInit = SUCCEEDED(comHr);

    const std::wstring browseTitle = Wide(I18n::Tr("dialog_output_dir_extract"));
    BROWSEINFOW bi = {};
    bi.lpszTitle = browseTitle.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    std::wstring outputDirW;
    if (pidl) {
        wchar_t folderPath[MAX_PATH] = {};
        if (SHGetPathFromIDListW(pidl, folderPath)) {
            outputDirW = folderPath;
        }
        CoTaskMemFree(pidl);
    }
    if (comInit) CoUninitialize();

    if (outputDirW.empty()) return;

    std::string outputDir = WideToUtf8(outputDirW.c_str());

    // Wait for any previous thread
    if (m_extractThread.joinable())
        m_extractThread.join();

    // Gather all files under the selected directory
    struct ExtractEntry {
        std::string relativePath;
        void* nodePtr;
    };
    std::vector<ExtractEntry> filesToExtract;

    size_t prefixLen = ParentPrefixLen(selPath);

    m_tree->ForEachFileUnder(selPath, [&](const std::string& fullPath, const std::string& name, int64_t /*fileSize*/, void* nodePtr) {
        if (!nodePtr) return;
        std::string rel = (fullPath.size() > prefixLen) ? fullPath.substr(prefixLen) : name;
        filesToExtract.push_back({ rel, nodePtr });
    });

    if (filesToExtract.empty()) {
        LogFmt(m_log, "Info", "GGPKExplorer: no files to extract in %s", selPath.c_str());
        return;
    }

    int totalFiles = static_cast<int>(filesToExtract.size());
    m_statusBar->progressActive.store(true);
    m_statusBar->progress.store(0.0f);
    m_statusBar->progressLabel = LocText{ I18n::Key("progress_extracting"), {} };
    m_extractRunning.store(true);

    m_extractThread = std::thread([this, filesToExtract = std::move(filesToExtract), outputDir, totalFiles]() {
        int extracted = 0;
        int failed = 0;

        for (const auto& fe : filesToExtract) {
            if (!m_extractRunning.load()) break;

            fs::path outPath = fs::path(outputDir) / fs::path(fe.relativePath);

            fs::path parentDir = outPath.parent_path();
            if (!fs::exists(parentDir)) {
                std::error_code ec;
                fs::create_directories(parentDir, ec);
                if (ec) {
                    ++failed;
                    continue;
                }
            }

            auto readRes = m_manager->ReadFile(fe.nodePtr);
            if (!readRes || readRes.value.empty()) {
                ++failed;
            } else {
                std::ofstream outFile{ outPath, std::ios::binary };
                if (outFile.is_open()) {
                    outFile.write(reinterpret_cast<const char*>(readRes.value.data()),
                                  static_cast<std::streamsize>(readRes.value.size()));
                    ++extracted;
                } else {
                    ++failed;
                }
            }

            float pct = static_cast<float>(extracted + failed) / static_cast<float>(totalFiles);
            m_statusBar->progress.store(pct);
        }

        m_statusBar->progressActive.store(false);
        m_extractRunning.store(false);

        // The result goes to the (English) log: the status bar hides its label with the bar.
        LogFmt(m_log, "Info", "GGPKExplorer: extracted %d/%d files (%d failed)", extracted, totalFiles, failed);
    });
}

// ---------------------------------------------------------------------------
// ExtractDirectoryFiltered
// ---------------------------------------------------------------------------

void ExtractDialog::ExtractDirectoryFiltered() {
    if (!m_tree || !m_manager || !m_statusBar) return;

    const auto& selPath = m_extractOptions.targetDirPath;
    if (selPath.empty()) return;

    if (m_extractRunning.load()) {
        LogFmt(m_log, "Info", "GGPKExplorer: extraction already in progress");
        return;
    }

    // Build set of included extensions
    std::set<std::string> includedExts;
    for (const auto& f : m_extractOptions.filters) {
        if (f.included)
            includedExts.insert(f.extension);
    }
    if (includedExts.empty()) {
        LogFmt(m_log, "Info", "GGPKExplorer: no file types selected for extraction");
        return;
    }

    // Compute max file size in bytes
    int64_t maxBytes = INT64_MAX;
    if (m_extractOptions.skipLargeFiles) {
        maxBytes = m_extractOptions.maxFileSize;
        switch (m_extractOptions.sizeUnit) {
            case 0: maxBytes *= 1024LL; break;
            case 1: maxBytes *= 1024LL * 1024LL; break;
            case 2: maxBytes *= 1024LL * 1024LL * 1024LL; break;
            default: break;
        }
    }

    // Browse for output directory
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool comInit = SUCCEEDED(comHr);

    const std::wstring browseTitle = Wide(I18n::Tr("dialog_output_dir_filtered"));
    BROWSEINFOW bi = {};
    bi.lpszTitle = browseTitle.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    std::wstring outputDirW;
    if (pidl) {
        wchar_t folderPath[MAX_PATH] = {};
        if (SHGetPathFromIDListW(pidl, folderPath)) {
            outputDirW = folderPath;
        }
        CoTaskMemFree(pidl);
    }
    if (comInit) CoUninitialize();

    if (outputDirW.empty()) return;

    std::string outputDir = WideToUtf8(outputDirW.c_str());

    if (m_extractThread.joinable())
        m_extractThread.join();

    // Gather files, applying extension and size filters
    struct ExtractEntry {
        std::string relativePath;
        void* nodePtr;
    };
    std::vector<ExtractEntry> filesToExtract;

    size_t prefixLen = ParentPrefixLen(selPath);

    m_tree->ForEachFileUnder(selPath, [&](const std::string& fullPath, const std::string& name, int64_t fileSize, void* nodePtr) {
        if (!nodePtr) return;

        std::string ext;
        auto dot = name.rfind('.');
        if (dot != std::string::npos) {
            ext = name.substr(dot);
            for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            ext = "no_extension";
        }
        if (includedExts.find(ext) == includedExts.end()) return;

        if (m_extractOptions.skipLargeFiles && fileSize > maxBytes) return;

        std::string rel = (fullPath.size() > prefixLen) ? fullPath.substr(prefixLen) : name;
        filesToExtract.push_back({ rel, nodePtr });
    });

    if (filesToExtract.empty()) {
        LogFmt(m_log, "Info", "GGPKExplorer: no files match filters in %s", selPath.c_str());
        return;
    }

    int totalFiles = static_cast<int>(filesToExtract.size());
    m_statusBar->progressActive.store(true);
    m_statusBar->progress.store(0.0f);
    m_statusBar->progressLabel = LocText{ I18n::Key("progress_extracting_filtered"), {} };
    m_extractRunning.store(true);

    bool skipLarge = m_extractOptions.skipLargeFiles;
    m_extractThread = std::thread([this, filesToExtract = std::move(filesToExtract), outputDir, totalFiles, maxBytes, skipLarge]() {
        int extracted = 0;
        int failed = 0;
        int skipped = 0;

        for (const auto& fe : filesToExtract) {
            if (!m_extractRunning.load()) break;

            fs::path outPath = fs::path(outputDir) / fs::path(fe.relativePath);

            fs::path parentDir = outPath.parent_path();
            if (!fs::exists(parentDir)) {
                std::error_code ec;
                fs::create_directories(parentDir, ec);
                if (ec) {
                    ++failed;
                    continue;
                }
            }

            auto readRes = m_manager->ReadFile(fe.nodePtr);
            if (!readRes || readRes.value.empty()) {
                ++failed;
            } else {
                if (skipLarge && static_cast<int64_t>(readRes.value.size()) > maxBytes) {
                    ++skipped;
                } else {
                    std::ofstream outFile{ outPath, std::ios::binary };
                    if (outFile.is_open()) {
                        outFile.write(reinterpret_cast<const char*>(readRes.value.data()),
                                      static_cast<std::streamsize>(readRes.value.size()));
                        ++extracted;
                    } else {
                        ++failed;
                    }
                }
            }

            float pct = static_cast<float>(extracted + failed + skipped) / static_cast<float>(totalFiles);
            m_statusBar->progress.store(pct);
        }

        m_statusBar->progressActive.store(false);
        m_extractRunning.store(false);

        // The result goes to the (English) log: the status bar hides its label with the bar.
        LogFmt(m_log, "Info", "GGPKExplorer: extracted %d/%d files (%d failed, %d skipped)",
               extracted, totalFiles, failed, skipped);
    });
}

// ---------------------------------------------------------------------------
// ExtractSelectedFiles
// ---------------------------------------------------------------------------

void ExtractDialog::DoExtractSelectedFiles() {
    if (!m_treePanel || !m_tree || !m_manager || !m_statusBar) return;

    if (m_extractRunning.load()) {
        LogFmt(m_log, "Info", "GGPKExplorer: extraction already in progress");
        return;
    }

    // Collect all files: directly selected files + files under selected directories
    struct FileInfo {
        std::string relativePath;
        void* nodePtr;
    };
    std::vector<FileInfo> files;
    std::set<std::string> addedPaths;

    const auto& flatList = m_tree->GetFlatList();
    for (int idx : m_treePanel->GetSelectedIndices()) {
        if (idx < 0 || idx >= static_cast<int>(flatList.size())) continue;
        const auto& entry = flatList[static_cast<uint32_t>(idx)];

        // Per-entry strip prefix: the parent path of the selected entry.
        // This preserves the selected folder/file name at the top of the output.
        size_t stripLen = ParentPrefixLen(entry.fullPath);

        if (entry.isDirectory) {
            m_tree->ForEachFileUnder(entry.fullPath,
                [&](const std::string& fp, const std::string& nm, int64_t, void* np) {
                    if (!np || !addedPaths.insert(fp).second) return;
                    std::string rel = (fp.size() > stripLen) ? fp.substr(stripLen) : nm;
                    files.push_back({ rel, np });
                });
        } else if (entry.nodePtr) {
            if (!addedPaths.insert(entry.fullPath).second) continue;
            std::string rel = (entry.fullPath.size() > stripLen)
                ? entry.fullPath.substr(stripLen)
                : entry.name;
            files.push_back({ rel, entry.nodePtr });
        }
    }

    if (files.empty()) return;

    // Browse for output directory
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool comInit = SUCCEEDED(comHr);

    const std::wstring browseTitle = Wide(I18n::Tr("dialog_output_dir"));
    BROWSEINFOW bi = {};
    bi.lpszTitle = browseTitle.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    std::wstring outputDirW;
    if (pidl) {
        wchar_t folderPath[MAX_PATH] = {};
        if (SHGetPathFromIDListW(pidl, folderPath)) {
            outputDirW = folderPath;
        }
        CoTaskMemFree(pidl);
    }
    if (comInit) CoUninitialize();

    if (outputDirW.empty()) return;

    std::string outputDir = WideToUtf8(outputDirW.c_str());

    if (m_extractThread.joinable())
        m_extractThread.join();

    int totalFiles = static_cast<int>(files.size());
    m_statusBar->progressActive.store(true);
    m_statusBar->progress.store(0.0f);
    m_statusBar->progressLabel = LocText{ I18n::Key("progress_extracting_selected"), {} };
    m_extractRunning.store(true);

    m_extractThread = std::thread([this, files = std::move(files), outputDir, totalFiles]() {
        int extracted = 0;
        int failed = 0;

        for (const auto& fe : files) {
            if (!m_extractRunning.load()) break;

            fs::path outPath = fs::path(outputDir) / fs::path(fe.relativePath);

            fs::path parentDir = outPath.parent_path();
            if (!fs::exists(parentDir)) {
                std::error_code ec;
                fs::create_directories(parentDir, ec);
            }

            auto readRes = m_manager->ReadFile(fe.nodePtr);
            if (!readRes || readRes.value.empty()) {
                ++failed;
            } else {
                std::ofstream outFile{ outPath, std::ios::binary };
                if (outFile.is_open()) {
                    outFile.write(reinterpret_cast<const char*>(readRes.value.data()),
                                  static_cast<std::streamsize>(readRes.value.size()));
                    ++extracted;
                } else {
                    ++failed;
                }
            }

            float pct = static_cast<float>(extracted + failed) / static_cast<float>(totalFiles);
            m_statusBar->progress.store(pct);
        }

        m_statusBar->progressActive.store(false);
        m_extractRunning.store(false);

        // The result goes to the (English) log: the status bar hides its label with the bar.
        LogFmt(m_log, "Info", "GGPKExplorer: extracted %d/%d files (%d failed)", extracted, totalFiles, failed);
    });
}

} // namespace GGPKExp
