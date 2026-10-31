#include "GGPKManager.h"
#include "AsyncTaskRunner.h"
#include "GGPK.h"
#include "BundledGGPK.h"
#include "Index.h"
#include "TreeNode.h"
#include "DriveBundleFactory.h"
#include "FileAccess.h"
#include "Oodle.h"
#include <Windows.h>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

namespace GGPKExp {

namespace {

// The index hashes paths lowered for A-Z only (NameHash::AsciiLower); the old lowercase path
// cache used std::tolower, which in the "C" locale the process runs in maps A-Z only too.
bool EqualsAsciiNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (GGPK::NameHash::AsciiLower(static_cast<uint8_t>(a[i])) !=
            GGPK::NameHash::AsciiLower(static_cast<uint8_t>(b[i])))
            return false;
    }
    return true;
}

std::string AsciiLowerCopy(std::string s) {
    for (auto& c : s) c = static_cast<char>(GGPK::NameHash::AsciiLower(static_cast<uint8_t>(c)));
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

GGPKManager::GGPKManager() = default;

GGPKManager::~GGPKManager() {
    Close();
}

// ---------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------

Result<void> GGPKManager::Open(const std::string& path) {
    // Close any previously opened source
    Close();

    try {
        // Normalise the path (UTF-8 in, UTF-8 to the library: never the ANSI code page)
        const fs::path fsPath = GGPK::Utf8Path(path).lexically_normal();
        const std::string normalised = GGPK::PathToUtf8(fsPath);

        // Determine source type
        const std::string lower = AsciiLowerCopy(normalised);

        if (lower.ends_with(".ggpk")) {
            // ----- Standalone GGPK file (Content.ggpk) -----
            if (!fs::exists(fsPath)) {
                m_error = "File not found: " + normalised;
                return Result<void>::Fail(m_error);
            }

            m_bundledGgpk = std::make_unique<GGPK::BundledGGPK>(normalised, true);
            m_mode = SourceMode::StandaloneGGPK;
            m_openedPath = normalised;
            BuildTreeFromIndex(*m_bundledGgpk->GetIndex());
            return Result<void>::Ok();
        }

        // ----- Direct path to _.index.bin -----
        if (lower.ends_with("_.index.bin") && fs::exists(fsPath)) {
            auto factory = std::make_unique<GGPK::DriveBundleFactory>(GGPK::PathToUtf8(fsPath.parent_path()));   // .../Bundles2/
            m_index = std::make_unique<GGPK::Index>(normalised, true, std::move(factory));
            m_mode = SourceMode::SteamBundles;
            m_openedPath = normalised;
            BuildTreeFromIndex(*m_index);
            return Result<void>::Ok();
        }

        // ----- Directory (Steam / Epic installation) -----
        if (!fs::is_directory(fsPath)) {
            m_error = "Path is not a directory or .ggpk file: " + normalised;
            return Result<void>::Fail(m_error);
        }

        // Look for Bundles2/_.index.bin
        const fs::path bundles2Path = fsPath / "Bundles2";
        const fs::path indexPath = bundles2Path / "_.index.bin";
        if (!fs::exists(indexPath)) {
            m_error = "Cannot find Bundles2/_.index.bin in: " + normalised;
            return Result<void>::Fail(m_error);
        }

        auto factory = std::make_unique<GGPK::DriveBundleFactory>(GGPK::PathToUtf8(bundles2Path));
        m_index = std::make_unique<GGPK::Index>(GGPK::PathToUtf8(indexPath), true, std::move(factory));
        m_mode = SourceMode::SteamBundles;
        m_openedPath = normalised;
        BuildTreeFromIndex(*m_index);
        return Result<void>::Ok();

    } catch (const std::exception& ex) {
        // Close() clears m_error: keep the reason for the UI ("Failed to open: <why>").
        std::string error = ex.what();
        Close();
        m_error = std::move(error);
        return Result<void>::Fail(m_error);
    } catch (...) {
        Close();
        m_error = "Unknown error while opening the game files";
        return Result<void>::Fail(m_error);
    }
}

// ---------------------------------------------------------------------------
// OpenAsync
// ---------------------------------------------------------------------------

std::future<Result<void>> GGPKManager::OpenAsync(const std::string& path, AsyncTaskRunner& runner) {
    return runner.Submit<void>(
        [this, path](std::atomic<bool>& /*cancelFlag*/, AsyncTaskRunner::ProgressCallback /*progress*/) -> Result<void> {
            return Open(path);
        }
    );
}

// ---------------------------------------------------------------------------
// Close
// ---------------------------------------------------------------------------

void GGPKManager::Close() {
    m_bundleTreeRoot.reset();
    m_bundledGgpk.reset();
    m_index.reset();
    m_root = FileEntry{};
    m_mode = SourceMode::None;
    m_openedPath.clear();
    m_error.clear();
    m_hasPendingWrites.store(false);
}

// ---------------------------------------------------------------------------
// Index access
// ---------------------------------------------------------------------------

GGPK::Index* GGPKManager::GetIndex() const {
    if (m_bundledGgpk) return m_bundledGgpk->GetIndex();
    return m_index.get();
}

void GGPKManager::TrimReadCache() const {
    if (GGPK::Index* index = GetIndex())
        index->TrimReadCache();
}

// ---------------------------------------------------------------------------
// FindNodeByPath — the index's own hash lookup. Same answers as the lowercase map of every
// tree path it replaces: the hash finds the record, and the record must be in the tree (a
// parsed path; BuildTree leaves path-less records out) under exactly this path ignoring A-Z
// case (which also rules out a hash collision and the hash's trailing-'/' trim).
// ---------------------------------------------------------------------------

void* GGPKManager::FindNodeByPath(const std::string& path) const {
    GGPK::Index* index = GetIndex();
    if (!index || path.empty()) return nullptr;
    GGPK::FileRecord* record = index->TryGetFile(path);
    if (!record) return nullptr;
    const std::string& recordPath = record->GetPath();
    if (recordPath.empty() || !EqualsAsciiNoCase(recordPath, path)) return nullptr;
    return record;
}

// ---------------------------------------------------------------------------
// ForEachFile — walk every known file path (loose GGPK + bundle contents).
// Walks the FileEntry tree built during Open(); visitor must not mutate the
// tree. Used by DatViewer / FileRefEditor / GgpkTreePicker for full-index
// enumeration (the VirtualTree-backed equivalents only see loose files).
// ---------------------------------------------------------------------------
void GGPKManager::ForEachFile(
    const std::function<void(const std::string& fullPath, int64_t fileSize, void* nodePtr)>& visitor) const
{
    if (!visitor) return;
    std::function<void(const FileEntry&)> walk = [&](const FileEntry& entry) {
        if (!entry.isDirectory && entry.nodePtr) {
            visitor(entry.fullPath, entry.fileSize, entry.nodePtr);
        }
        for (const auto& child : entry.children) {
            walk(child);
        }
    };
    walk(m_root);
}

// ---------------------------------------------------------------------------
// ReadFile
// ---------------------------------------------------------------------------

Result<std::vector<uint8_t>> GGPKManager::ReadFile(void* nodePtr) const {
    if (!nodePtr)
        return Result<std::vector<uint8_t>>::Fail("Null node pointer");
    if (!IsOpen())
        return Result<std::vector<uint8_t>>::Fail("No source mode active");

    try {
        // Both modes read through the bundle index: nodePtr is always a FileRecord*.
        auto* record = static_cast<GGPK::FileRecord*>(nodePtr);
        auto data = record->Read();
        return Result<std::vector<uint8_t>>::Ok(std::move(data));
    } catch (const std::exception& ex) {
        return Result<std::vector<uint8_t>>::Fail(std::string("Read error: ") + ex.what());
    } catch (...) {
        return Result<std::vector<uint8_t>>::Fail("Unknown read error");
    }
}

// ---------------------------------------------------------------------------
// WriteFile
// ---------------------------------------------------------------------------

bool GGPKManager::WriteFile(void* nodePtr, const std::vector<uint8_t>& data, std::string* error) {
    if (!nodePtr) {
        if (error) *error = "Null node pointer";
        return false;
    }

    try {
        // Write is supported for both modes -- nodePtr is always FileRecord*
        // because both BundledGGPK and SteamBundles use the Index for file access.
        // Pass save=false so the index is NOT flushed yet -- caller must call
        // FlushWrites() to persist changes to disk.
        auto* record = static_cast<GGPK::FileRecord*>(nodePtr);
        record->Write(data, false);
        m_hasPendingWrites.store(true);
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    } catch (...) {
        if (error) *error = "Unknown write error";
        return false;
    }
}

// ---------------------------------------------------------------------------
// FlushWrites
// ---------------------------------------------------------------------------

bool GGPKManager::FlushWrites(std::string* error) {
    if (!m_hasPendingWrites.load())
        return true;

    // Cleared before saving: a write that lands while the save runs sets it again (and is
    // saved next time) instead of being forgotten.
    m_hasPendingWrites.store(false);
    try {
        switch (m_mode) {
        case SourceMode::StandaloneGGPK:
            // SaveIndex() calls Index::Save() internally, then writes the
            // serialised index back into Content.ggpk and renews the hash chain.
            m_bundledGgpk->SaveIndex();
            break;

        case SourceMode::SteamBundles:
            m_index->Save();
            break;

        default:
            if (error) *error = "No source mode active";
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        m_hasPendingWrites.store(true);
        if (error) *error = ex.what();
        return false;
    } catch (...) {
        m_hasPendingWrites.store(true);
        if (error) *error = "Unknown error while saving";
        return false;
    }
}

// ---------------------------------------------------------------------------
// Tree building: both modes (Content.ggpk's internal bundles, or Bundles2 on disk)
// ---------------------------------------------------------------------------

void GGPKManager::BuildTreeFromIndex(GGPK::Index& index) {
    m_bundleTreeRoot = index.BuildTree(true);

    m_root.name = "Root";
    m_root.fullPath.clear();
    m_root.isDirectory = true;
    m_root.nodePtr = nullptr;

    if (m_bundleTreeRoot) {
        PopulateFromBundleDir(m_root, m_bundleTreeRoot.get());
    }
}

// ---------------------------------------------------------------------------
// Helpers: populate FileEntry tree from bundle DirectoryNode
// ---------------------------------------------------------------------------

void GGPKManager::PopulateFromBundleDir(FileEntry& entry, GGPK::DirectoryNode* dir) {
    auto& children = dir->GetChildren();
    entry.children.reserve(children.size());

    for (auto& child : children) {
        FileEntry fe;
        fe.name = child->GetName();

        // Build full path
        if (entry.fullPath.empty()) {
            fe.fullPath = fe.name;
        } else {
            fe.fullPath = entry.fullPath + "/" + fe.name;
        }

        if (child->IsDirectory()) {
            fe.isDirectory = true;
            fe.fileSize = -1;
            fe.nodePtr = nullptr;
            PopulateFromBundleDir(fe, static_cast<GGPK::DirectoryNode*>(child.get()));
        } else {
            auto* fileNode = static_cast<GGPK::FileNode*>(child.get());
            auto* record = fileNode->GetRecord();
            fe.isDirectory = false;
            fe.fileSize = record ? record->GetSize() : -1;
            fe.nodePtr = static_cast<void*>(record);
        }

        entry.children.push_back(std::move(fe));
    }

    // Sort children: directories first, then alphabetical
    std::sort(entry.children.begin(), entry.children.end(),
              [](const FileEntry& a, const FileEntry& b) {
                  if (a.isDirectory != b.isDirectory)
                      return a.isDirectory > b.isDirectory; // dirs first
                  return a.name < b.name;
              });
}

// ---------------------------------------------------------------------------
// InitOodle
// ---------------------------------------------------------------------------

bool GGPKManager::InitOodle(const std::string& dir) {
    if (GGPK::Oodle::IsLoaded())
        return true;

    // 1. Try user-provided directory first (game dir)
    if (!dir.empty()) {
        for (const char* name : { "oo2core_9_win64.dll", "oo2core.dll" }) {
            try {
                const fs::path oodlePath = GGPK::Utf8Path(dir) / name;
                std::error_code ec;
                if (fs::exists(oodlePath, ec)) {
                    GGPK::Oodle::LoadDll(oodlePath.wstring());
                    if (GGPK::Oodle::IsLoaded()) return true;
                }
            } catch (...) {}
        }
    }

    // 2. Fall back to exe-relative Resources/smoother/
    wchar_t exePathBuf[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(NULL, exePathBuf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return false;

    auto oodlePath = fs::path(exePathBuf).parent_path() / "Resources" / "smoother" / "oo2core.dll";
    std::error_code ec;
    if (!fs::exists(oodlePath, ec))
        return false;

    try {
        GGPK::Oodle::LoadDll(oodlePath.wstring());
    } catch (...) {
        return false;
    }

    return GGPK::Oodle::IsLoaded();
}

} // namespace GGPKExp
