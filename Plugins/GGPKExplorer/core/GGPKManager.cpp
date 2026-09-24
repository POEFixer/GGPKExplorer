#include "GGPKManager.h"
#include "AsyncTaskRunner.h"
#include "GGPK.h"
#include "BundledGGPK.h"
#include "Index.h"
#include "TreeNode.h"
#include "DriveBundleFactory.h"
#include "Oodle.h"
#include <Windows.h>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

namespace GGPKExp {

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
        // Normalise the path
        fs::path fsPath(path);
        std::string normalised = fsPath.lexically_normal().string();

        // Determine source type
        std::string lower = normalised;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (lower.ends_with(".ggpk")) {
            // ----- Standalone GGPK file (Content.ggpk) -----
            if (!fs::exists(fsPath)) {
                m_error = "File not found: " + normalised;
                return Result<void>::Fail(m_error);
            }

            m_bundledGgpk = std::make_unique<GGPK::BundledGGPK>(normalised, true);
            m_mode = SourceMode::StandaloneGGPK;
            m_openedPath = normalised;
            BuildTreeFromBundledGGPK();
            return Result<void>::Ok();
        }

        // ----- Direct path to _.index.bin -----
        if (lower.ends_with("_.index.bin") && fs::exists(fsPath)) {
            fs::path bundles2Path = fsPath.parent_path();         // .../Bundles2/
            fs::path indexPath = fsPath;
            std::string bundles2Dir = bundles2Path.string();
            auto factory = std::make_unique<GGPK::DriveBundleFactory>(bundles2Dir);
            m_index = std::make_unique<GGPK::Index>(indexPath.string(), true, std::move(factory));
            m_mode = SourceMode::SteamBundles;
            m_openedPath = normalised;
            BuildTreeFromBundles();
            return Result<void>::Ok();
        }

        // ----- Directory (Steam / Epic installation) -----
        if (!fs::is_directory(fsPath)) {
            m_error = "Path is not a directory or .ggpk file: " + normalised;
            return Result<void>::Fail(m_error);
        }

        // Look for Bundles2/_.index.bin
        fs::path indexPath = fsPath / "Bundles2" / "_.index.bin";
        if (!fs::exists(indexPath)) {
            m_error = "Cannot find Bundles2/_.index.bin in: " + normalised;
            return Result<void>::Fail(m_error);
        }

        std::string bundles2Dir = (fsPath / "Bundles2").string();
        auto factory = std::make_unique<GGPK::DriveBundleFactory>(bundles2Dir);
        m_index = std::make_unique<GGPK::Index>(indexPath.string(), true, std::move(factory));
        m_mode = SourceMode::SteamBundles;
        m_openedPath = normalised;
        BuildTreeFromBundles();
        return Result<void>::Ok();

    } catch (const std::exception& ex) {
        m_error = ex.what();
        Close();
        return Result<void>::Fail(m_error);
    }
}

// ---------------------------------------------------------------------------
// OpenAsync
// ---------------------------------------------------------------------------

std::future<Result<void>> GGPKManager::OpenAsync(const std::string& path, AsyncTaskRunner& runner) {
    m_loading.store(true);
    return runner.Submit<void>(
        [this, path](std::atomic<bool>& /*cancelFlag*/, AsyncTaskRunner::ProgressCallback /*progress*/) -> Result<void> {
            auto result = Open(path);
            m_loading.store(false);
            return result;
        }
    );
}

// ---------------------------------------------------------------------------
// InvalidatePathCache
// ---------------------------------------------------------------------------

void GGPKManager::InvalidatePathCache() {
    m_pathCache.clear();
    m_pathCacheBuilt = false;
}

// ---------------------------------------------------------------------------
// Close
// ---------------------------------------------------------------------------

void GGPKManager::Close() {
    m_bundleTreeRoot.reset();
    m_bundledGgpk.reset();
    m_ggpk.reset();
    m_index.reset();
    m_root = FileEntry{};
    m_mode = SourceMode::None;
    m_openedPath.clear();
    m_error.clear();
    m_pathCache.clear();
    m_pathCacheBuilt = false;
    m_hasPendingWrites = false;
}

// ---------------------------------------------------------------------------
// Path lookup cache
// ---------------------------------------------------------------------------

void GGPKManager::BuildPathCache() const {
    m_pathCache.clear();
    std::function<void(const FileEntry&)> walk = [&](const FileEntry& entry) {
        if (!entry.isDirectory && entry.nodePtr) {
            std::string lower = entry.fullPath;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            m_pathCache[lower] = entry.nodePtr;
        }
        for (auto& child : entry.children) {
            walk(child);
        }
    };
    walk(m_root);
    m_pathCacheBuilt = true;
}

void* GGPKManager::FindNodeByPath(const std::string& path) const {
    if (!m_pathCacheBuilt) {
        BuildPathCache();
    }
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    auto it = m_pathCache.find(lower);
    return (it != m_pathCache.end()) ? it->second : nullptr;
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

    try {
        switch (m_mode) {
        case SourceMode::StandaloneGGPK: {
            // In BundledGGPK mode, all files go through the Index (FileRecord*)
            auto* record = static_cast<GGPK::FileRecord*>(nodePtr);
            auto data = record->Read();
            return Result<std::vector<uint8_t>>::Ok(std::move(data));
        }
        case SourceMode::SteamBundles: {
            auto* record = static_cast<GGPK::FileRecord*>(nodePtr);
            auto data = record->Read();
            return Result<std::vector<uint8_t>>::Ok(std::move(data));
        }
        default:
            return Result<std::vector<uint8_t>>::Fail("No source mode active");
        }
    } catch (const std::exception& ex) {
        return Result<std::vector<uint8_t>>::Fail(std::string("Read error: ") + ex.what());
    } catch (...) {
        return Result<std::vector<uint8_t>>::Fail("Unknown read error");
    }
}

// ---------------------------------------------------------------------------
// WriteFile
// ---------------------------------------------------------------------------

bool GGPKManager::WriteFile(void* nodePtr, const std::vector<uint8_t>& data) {
    if (!nodePtr)
        return false;

    try {
        // Write is supported for both modes -- nodePtr is always FileRecord*
        // because both BundledGGPK and SteamBundles use the Index for file access.
        // Pass save=false so the index is NOT flushed yet -- caller must call
        // FlushWrites() to persist changes to disk.
        auto* record = static_cast<GGPK::FileRecord*>(nodePtr);
        record->Write(data, false);
        m_hasPendingWrites = true;
        return true;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// FlushWrites
// ---------------------------------------------------------------------------

bool GGPKManager::FlushWrites() {
    if (!m_hasPendingWrites)
        return true;

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
            return false;
        }

        m_hasPendingWrites = false;
        return true;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Tree building: BundledGGPK (standalone .ggpk file with internal bundles)
// ---------------------------------------------------------------------------

void GGPKManager::BuildTreeFromBundledGGPK() {
    auto* index = m_bundledGgpk->GetIndex();
    m_bundleTreeRoot = index->BuildTree(true);

    m_root.name = "Root";
    m_root.fullPath.clear();
    m_root.isDirectory = true;
    m_root.nodePtr = nullptr;

    if (m_bundleTreeRoot) {
        PopulateFromBundleDir(m_root, m_bundleTreeRoot.get());
    }
}

// ---------------------------------------------------------------------------
// Tree building: Steam / Epic (disk-based bundles)
// ---------------------------------------------------------------------------

void GGPKManager::BuildTreeFromBundles() {
    m_bundleTreeRoot = m_index->BuildTree(true);

    m_root.name = "Root";
    m_root.fullPath.clear();
    m_root.isDirectory = true;
    m_root.nodePtr = nullptr;

    if (m_bundleTreeRoot) {
        PopulateFromBundleDir(m_root, m_bundleTreeRoot.get());
    }
}

// ---------------------------------------------------------------------------
// Tree building: raw GGPK without bundles (legacy, pre-3.21)
// ---------------------------------------------------------------------------

void GGPKManager::BuildTreeFromGGPK() {
    auto* root = m_ggpk->GetRoot();

    m_root.name = "Root";
    m_root.fullPath.clear();
    m_root.isDirectory = true;
    m_root.nodePtr = nullptr;

    if (root) {
        PopulateFromGGPKNode(m_root, root);
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
// Helpers: populate FileEntry tree from raw GGPK GGPKTreeNode
// ---------------------------------------------------------------------------

void GGPKManager::PopulateFromGGPKNode(FileEntry& entry, GGPK::GGPKTreeNode* node) {
    entry.children.reserve(node->children.size());

    for (auto& [name, childPtr] : node->children) {
        FileEntry fe;
        fe.name = name;

        // Build full path
        if (entry.fullPath.empty()) {
            fe.fullPath = fe.name;
        } else {
            fe.fullPath = entry.fullPath + "/" + fe.name;
        }

        if (childPtr->isDirectory) {
            fe.isDirectory = true;
            fe.fileSize = -1;
            fe.nodePtr = static_cast<void*>(childPtr.get());
            PopulateFromGGPKNode(fe, childPtr.get());
        } else {
            fe.isDirectory = false;
            fe.fileSize = childPtr->dataLength;
            fe.nodePtr = static_cast<void*>(childPtr.get());
        }

        entry.children.push_back(std::move(fe));
    }

    // Sort children: directories first, then alphabetical
    std::sort(entry.children.begin(), entry.children.end(),
              [](const FileEntry& a, const FileEntry& b) {
                  if (a.isDirectory != b.isDirectory)
                      return a.isDirectory > b.isDirectory;
                  return a.name < b.name;
              });
}

// ---------------------------------------------------------------------------
// AutoDetectGameDir
// ---------------------------------------------------------------------------

std::string GGPKManager::AutoDetectGameDir() {
    // 1. Windows Registry
    {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\GrindingGearGames\\Path of Exile 2",
            0, KEY_READ | KEY_WOW64_32KEY, &hKey) == ERROR_SUCCESS)
        {
            wchar_t buf[MAX_PATH] = {};
            DWORD bufSize = sizeof(buf);
            DWORD type = 0;
            if (RegQueryValueExW(hKey, L"InstallLocation", nullptr, &type,
                reinterpret_cast<LPBYTE>(buf), &bufSize) == ERROR_SUCCESS
                && type == REG_SZ && buf[0] != L'\0')
            {
                RegCloseKey(hKey);
                int needed = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
                if (needed > 0) {
                    std::string path(needed - 1, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, buf, -1, path.data(), needed, nullptr, nullptr);
                    if (fs::exists(path)) return path;
                }
            } else {
                RegCloseKey(hKey);
            }
        }
    }

    // 2. Common install paths
    const char* commonPaths[] = {
        "C:\\Program Files (x86)\\Grinding Gear Games\\Path of Exile 2",
        "C:\\Program Files\\Grinding Gear Games\\Path of Exile 2",
        "C:\\Games\\Path of Exile 2",
    };
    for (const char* p : commonPaths) {
        if (fs::exists(p) && fs::exists(fs::path(p) / "Bundles2" / "_.index.bin"))
            return p;
    }

    return {};
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
            auto oodlePath = fs::path(dir) / name;
            if (fs::exists(oodlePath)) {
                try { GGPK::Oodle::LoadDll(oodlePath.wstring()); } catch (...) {}
                if (GGPK::Oodle::IsLoaded()) return true;
            }
        }
    }

    // 2. Fall back to exe-relative Resources/smoother/
    wchar_t exePathBuf[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(NULL, exePathBuf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return false;

    auto oodlePath = fs::path(exePathBuf).parent_path() / "Resources" / "smoother" / "oo2core.dll";
    if (!fs::exists(oodlePath))
        return false;

    try {
        GGPK::Oodle::LoadDll(oodlePath.wstring());
    } catch (...) {
        return false;
    }

    return GGPK::Oodle::IsLoaded();
}

} // namespace GGPKExp
