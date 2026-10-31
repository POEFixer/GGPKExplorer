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
#include <unordered_set>

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

// The selection's files of the format's source type, each once. The paths are the tree
// nodes' own strings (a selected file walks to itself): they stay put while the job runs.
void SaveAsExporter::CollectTargets(SaveFormat fmt, std::vector<ExtractJob::Item>& out, bool& sawNonMatching) const {
    sawNonMatching = false;
    if (!m_tree || !m_treePanel) return;
    const char* needExt = RequiredExt(fmt);
    const auto& flat = m_tree->GetFlatList();
    std::unordered_set<void*> seen;
    auto add = [&](const std::string& fullPath, const std::string& name, int64_t /*sz*/, void* nodePtr) {
        if (nodePtr && EndsWithCI(name, needExt) && seen.insert(nodePtr).second)
            out.push_back({ static_cast<GGPK::FileRecord*>(nodePtr), &fullPath, 0 });
    };

    for (int idx : m_treePanel->GetSelectedIndices()) {
        if (idx < 0 || idx >= static_cast<int>(flat.size())) continue;
        const auto& e = flat[static_cast<uint32_t>(idx)];
        if (e.isDirectory || EndsWithCI(e.name, needExt)) {
            m_tree->ForEachFileUnder(e.fullPath, add);
        } else {
            sawNonMatching = true;
        }
    }
}

void SaveAsExporter::SaveSelectionAs(SaveFormat fmt) {
    if (IsRunning()) return;
    if (!m_manager || !m_tree || !m_treePanel) return;

    std::vector<ExtractJob::Item> targets;
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
        std::filesystem::path srcRel = PathUtils::FromUtf8(*targets.front().path);
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

    RunExport(fmt, std::move(targets), std::move(destRoot), std::move(singleOut));
}

void SaveAsExporter::UpdateProgress() {
    if (m_statusBar && m_job && m_job->IsRunning()) {
        const size_t total = m_job->Total();
        m_statusBar->progress.store(total ? static_cast<float>(m_job->Done()) / static_cast<float>(total) : 0.0f);
    }
}

void SaveAsExporter::RunExport(SaveFormat fmt, std::vector<ExtractJob::Item> targets,
                               std::filesystem::path destRoot, std::filesystem::path singleOut) {
    GGPK::Index* index = m_manager ? m_manager->GetIndex() : nullptr;
    if (!index) return;
    if (!m_job) m_job = std::make_unique<ExtractJob>();
    if (m_job->IsRunning()) return;

    // The schema of this run: the game data may replace m_schema meanwhile (the old one stays
    // alive, GGPKExplorer keeps every schema it loaded).
    DatSchema* schema = m_schema;

    ExtractJob::Options options;
    if (!singleOut.empty()) {
        options.singleOutput = singleOut.native();
    } else {
        options.outputDir = destRoot.native();
        options.replaceExtension = OutExtW(fmt);
    }
    options.readWorkers = ExtractJob::DefaultReadWorkers();
    options.convertWorkers = ExtractJob::DefaultConvertWorkers();
    // Two queues here (read -> convert -> write) and a converted table can be several times
    // its DAT: half the extraction's bound each keeps the whole job's memory alike.
    options.queueBytes = 128u * 1024 * 1024;

    using Outcome = ExtractJob::Outcome;
    switch (fmt) {
    case SaveFormat::Json:
    case SaveFormat::Csv: {
        const bool json = fmt == SaveFormat::Json;
        options.convert = [schema, json](const ExtractJob::Item& item, std::vector<uint8_t>& bytes,
                                         std::string& why) -> Outcome {
            if (bytes.empty()) { why = "empty file"; return Outcome::Fail; }
            if (!schema || !schema->IsLoaded()) { why = "no schema"; return Outcome::Skip; }
            std::vector<uint8_t> out;
            std::string err;
            const std::string table = ExtractTableName(*item.path);
            const bool ok = json ? DatExport::ConvertBytesToJsonBytes(bytes, table, *schema, out, &err)
                                 : DatExport::ConvertBytesToCsvBytes(bytes, table, *schema, out, &err);
            if (ok) {
                bytes = std::move(out);
                return Outcome::Write;
            }
            why = err;
            // A table the schema does not know is skipped, never a failure.
            return err.rfind("no schema", 0) == 0 ? Outcome::Skip : Outcome::Fail;
        };
        break;
    }
    case SaveFormat::Png:
        options.convert = [](const ExtractJob::Item& /*item*/, std::vector<uint8_t>& bytes,
                             std::string& why) -> Outcome {
            if (bytes.empty()) { why = "empty file"; return Outcome::Fail; }
            std::vector<uint8_t> png;
            if (!ImageExport::DdsBytesToPngBytes(bytes, png, &why)) return Outcome::Fail;
            bytes = std::move(png);
            return Outcome::Write;
        };
        break;
    case SaveFormat::Dds:
        break;   // the file as it is (an empty one is written empty, as an extraction)
    }

    if (m_statusBar) {
        m_statusBar->progress.store(0.0f);
        m_statusBar->progressLabel = LocText{ I18n::Key("progress_save_as"), { { "format", FormatName(fmt) } } };
        m_statusBar->progressActive.store(true);
    }

    // Logged from the job's thread: it uses only what outlives the job.
    LogCallback log = m_log;
    StatusBarState* statusBar = m_statusBar;
    const char* formatName = FormatName(fmt);
    auto onFinished = [log, statusBar, formatName](const ExtractJob::Summary& s) {
        if (statusBar) statusBar->progressActive.store(false);
        if (!log) return;
        char buf[512];
        if (!s.fatalError.empty()) {
            snprintf(buf, sizeof(buf), "GGPKExplorer: Save As %s stopped: %s", formatName, s.fatalError.c_str());
            log("Error", buf);
        }
        snprintf(buf, sizeof(buf),
                 "GGPKExplorer: Save As %s - %zu written, %zu skipped (no schema), %zu failed%s",
                 formatName, s.written, s.skipped, s.failed, s.cancelled ? ", cancelled" : "");
        log("Info", buf);
        for (const auto& error : s.sampleErrors) {
            snprintf(buf, sizeof(buf), "GGPKExplorer: Save As %s failed: %s", formatName, error.c_str());
            log("Error", buf);
        }
    };

    if (!m_job->Start(*index, std::move(targets), std::move(options), std::move(onFinished))) {
        if (m_statusBar) m_statusBar->progressActive.store(false);
        if (m_log) m_log("Error", "GGPKExplorer: could not start Save As");
    }
}

} // namespace GGPKExp
