#include "SaveAsExporter.h"

#include "../core/GGPKManager.h"
#include "../core/VirtualTree.h"
#include "../core/DatSchema.h"     // ExtractTableName, SchemaTable
#include "../core/DatExport.h"
#include "../core/ImageExport.h"
#include "../core/PathUtils.h"
#include "TreePanel.h"
#include "StatusBar.h"
#include "UiText.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <commdlg.h>
#include <shlobj.h>

namespace GGPKExp {

const char* SaveAsExporter::RequiredExt(SaveFormat fmt) {
    switch (fmt) {
    case SaveFormat::Json: case SaveFormat::Csv: return ".datc64";
    case SaveFormat::Dds:  case SaveFormat::Png: return ".dds";
    }
    return "";
}
const wchar_t* SaveAsExporter::OutExtW(SaveFormat fmt) {
    switch (fmt) {
    case SaveFormat::Json: return L".json";
    case SaveFormat::Csv:  return L".csv";
    case SaveFormat::Dds:  return L".dds";
    case SaveFormat::Png:  return L".png";
    }
    return L"";
}
const char* SaveAsExporter::FormatName(SaveFormat fmt) {
    switch (fmt) {
    case SaveFormat::Json: return "JSON";
    case SaveFormat::Csv:  return "CSV";
    case SaveFormat::Dds:  return "DDS";
    case SaveFormat::Png:  return "PNG";
    }
    return "";
}
const wchar_t* SaveAsExporter::FormatNameW(SaveFormat fmt) {
    switch (fmt) {
    case SaveFormat::Json: return L"JSON";
    case SaveFormat::Csv:  return L"CSV";
    case SaveFormat::Dds:  return L"DDS";
    case SaveFormat::Png:  return L"PNG";
    }
    return L"";
}

static bool EndsWithCI(const std::string& s, const char* suffix) {
    size_t n = 0; while (suffix[n]) ++n;
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[s.size() - n + i])));
        char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b) return false;
    }
    return true;
}

void SaveAsExporter::CollectTargets(SaveFormat fmt, std::vector<Target>& out, bool& sawNonMatching) const {
    sawNonMatching = false;
    if (!m_tree || !m_treePanel) return;
    const char* needExt = RequiredExt(fmt);
    const auto& flat = m_tree->GetFlatList();
    std::set<std::string> seen;

    for (int idx : m_treePanel->GetSelectedIndices()) {
        if (idx < 0 || idx >= static_cast<int>(flat.size())) continue;
        const auto& e = flat[static_cast<uint32_t>(idx)];
        if (e.isDirectory) {
            m_tree->ForEachFileUnder(e.fullPath,
                [&](const std::string& fullPath, const std::string& name, int64_t /*sz*/, void* nodePtr) {
                    if (EndsWithCI(name, needExt) && seen.insert(fullPath).second)
                        out.push_back({ nodePtr, fullPath });
                });
        } else if (EndsWithCI(e.name, needExt)) {
            if (seen.insert(e.fullPath).second)
                out.push_back({ e.nodePtr, e.fullPath });
        } else {
            sawNonMatching = true;
        }
    }
}

void SaveAsExporter::SaveSelectionAs(SaveFormat fmt) {
    if (m_running.load()) return;
    if (!m_manager || !m_tree || !m_treePanel) return;

    std::vector<Target> targets;
    bool sawNonMatching = false;
    CollectTargets(fmt, targets, sawNonMatching);

    if (targets.empty()) {
        if (m_log) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "GGPKExplorer: nothing to export as %s (no %s files in selection)",
                     FormatName(fmt), RequiredExt(fmt));
            m_log("Info", buf);
        }
        return;
    }

    // Single-file selection (exactly one selected entry, and it is a file) -> Save dialog.
    bool singleFileSel = false;
    {
        const auto& sel = m_treePanel->GetSelectedIndices();
        if (sel.size() == 1) {
            const auto& flat = m_tree->GetFlatList();
            int idx = *sel.begin();
            if (idx >= 0 && idx < static_cast<int>(flat.size()) &&
                !flat[static_cast<uint32_t>(idx)].isDirectory)
                singleFileSel = true;
        }
    }

    std::filesystem::path destRoot, singleOut;

    if (singleFileSel) {
        std::filesystem::path srcRel = PathUtils::FromUtf8(targets.front().ggpkPath);
        std::wstring defName = srcRel.stem().wstring() + OutExtW(fmt);
        wchar_t savePath[MAX_PATH] = {};
        if (defName.size() < MAX_PATH) wcsncpy_s(savePath, defName.c_str(), defName.size());

        // "<FMT> files (*.ext)", "All files (*.*)": labels in the program language.
        std::wstring ext = OutExtW(fmt);
        DialogFilter filter;
        filter.Add(I18n::TrF("dialog_filter_ext_files", { { "ext", FormatName(fmt) } }), L"*" + ext);
        filter.Add(I18n::Tr("dialog_filter_all_files"), L"*.*");
        const std::wstring title = Wide(I18n::Tr("dialog_save_as"));

        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFilter = filter.Get();
        ofn.lpstrFile = savePath;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
        ofn.lpstrDefExt = OutExtW(fmt) + 1; // skip the leading dot
        ofn.lpstrTitle = title.c_str();
        if (!GetSaveFileNameW(&ofn)) return;
        singleOut = std::filesystem::path(savePath);
    } else {
        HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        bool comInit = SUCCEEDED(comHr);
        const std::wstring browseTitle = Wide(I18n::Tr("dialog_output_folder"));
        BROWSEINFOW bi = {};
        bi.lpszTitle = browseTitle.c_str();
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        std::wstring folder;
        if (pidl) {
            wchar_t path[MAX_PATH] = {};
            if (SHGetPathFromIDListW(pidl, path)) folder = path;
            CoTaskMemFree(pidl);
        }
        if (comInit) CoUninitialize();
        if (folder.empty()) return;
        destRoot = std::filesystem::path(folder);
    }

    if (m_thread.joinable()) m_thread.join();   // reap a previous finished run
    m_running.store(true);
    if (m_statusBar) {
        m_statusBar->progress.store(0.0f);
        m_statusBar->progressLabel = LocText{ I18n::Key("progress_save_as"), { { "format", FormatName(fmt) } } };
        m_statusBar->progressActive.store(true);
    }
    RunExport(fmt, std::move(targets), std::move(destRoot), std::move(singleOut));
}

void SaveAsExporter::RunExport(SaveFormat fmt, std::vector<Target> targets,
                               std::filesystem::path destRoot, std::filesystem::path singleOut) {
    // The schema of this run: the game data may replace m_schema meanwhile (the old one stays
    // alive, GGPKExplorer keeps every schema it loaded).
    DatSchema* schema = m_schema;
    m_thread = std::thread([this, fmt, schema, targets = std::move(targets),
                            destRoot = std::move(destRoot), singleOut = std::move(singleOut)]() mutable {
        size_t ok = 0, skippedNoSchema = 0, failed = 0;
        const size_t total = targets.size();

        for (size_t i = 0; i < total; ++i) {
            if (!m_running.load()) break;
            const Target& t = targets[i];

            std::filesystem::path outPath;
            if (!singleOut.empty()) {
                outPath = singleOut;
            } else {
                std::filesystem::path rel = PathUtils::FromUtf8(t.ggpkPath);
                rel.replace_extension(OutExtW(fmt));
                outPath = destRoot / rel;
                std::error_code ec;
                std::filesystem::create_directories(outPath.parent_path(), ec);
            }

            auto r = m_manager->ReadFile(t.nodePtr);
            if (!r || r.value.empty()) {
                ++failed;
            } else {
                bool wrote = false;
                std::string err;
                switch (fmt) {
                case SaveFormat::Json:
                    if (schema && schema->IsLoaded()) wrote = DatExport::ConvertBytesToJson(r.value, ExtractTableName(t.ggpkPath), *schema, outPath, &err);
                    else err = "no schema";
                    break;
                case SaveFormat::Csv:
                    if (schema && schema->IsLoaded()) wrote = DatExport::ConvertBytesToCsv(r.value, ExtractTableName(t.ggpkPath), *schema, outPath, &err);
                    else err = "no schema";
                    break;
                case SaveFormat::Dds: {
                    std::ofstream os(outPath, std::ios::binary);
                    if (os.is_open()) {
                        os.write(reinterpret_cast<const char*>(r.value.data()),
                                 static_cast<std::streamsize>(r.value.size()));
                        wrote = os.good();
                    }
                    break;
                }
                case SaveFormat::Png:
                    wrote = ImageExport::DdsBytesToPng(r.value, outPath, &err);
                    break;
                }
                if (wrote) ++ok;
                else if (err.rfind("no schema", 0) == 0) ++skippedNoSchema;
                else ++failed;
            }

            if (m_statusBar)
                m_statusBar->progress.store(static_cast<float>(i + 1) / static_cast<float>(total));
        }

        if (m_statusBar) m_statusBar->progressActive.store(false);
        m_running.store(false);

        if (m_log) {
            char buf[384];
            snprintf(buf, sizeof(buf),
                     "GGPKExplorer: Save As %s - %zu written, %zu skipped (no schema), %zu failed",
                     FormatName(fmt), ok, skippedNoSchema, failed);
            m_log("Info", buf);
        }
    });
}

void SaveAsExporter::JoinThread() {
    if (m_thread.joinable()) m_thread.join();
}

} // namespace GGPKExp
