#pragma once
#include "FileTypeDetector.h"
#include "Result.h"
#include <atomic>
#include <functional>
#include <future>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>

// Forward declarations -- GGPK library types
namespace GGPK {
    class GGPKFile;
    class Index;
    class BundledGGPK;
    class FileRecord;
    class DirectoryNode;
    class FileNode;
    class Bundle;
    struct GGPKTreeNode;
}

namespace GGPKExp {

class AsyncTaskRunner;

enum class SourceMode { None, StandaloneGGPK, SteamBundles };

/// Unified file entry for VirtualTree consumption.
/// nodePtr is either GGPK::FileRecord* (Bundle mode) or GGPK::GGPKTreeNode* (GGPK mode).
struct FileEntry {
    std::string name;
    std::string fullPath;
    int64_t fileSize = -1;
    bool isDirectory = false;
    void* nodePtr = nullptr; // opaque -- FileRecord* or GGPKTreeNode*
    std::vector<FileEntry> children;
};

class GGPKManager {
public:
    GGPKManager();
    ~GGPKManager();

    Result<void> Open(const std::string& path);
    std::future<Result<void>> OpenAsync(const std::string& path, AsyncTaskRunner& runner);
    bool IsLoading() const { return m_loading.load(); }
    void Close();
    bool IsOpen() const { return m_mode != SourceMode::None; }
    SourceMode GetMode() const { return m_mode; }
    const std::string& GetError() const { return m_error; }
    const std::string& GetOpenedPath() const { return m_openedPath; }

    /// Get the root file entry (tree of all files).
    const FileEntry& GetRoot() const { return m_root; }

    /// Find a file node by path (case-insensitive). O(1) after first call.
    /// Returns nodePtr or nullptr if not found.
    void* FindNodeByPath(const std::string& path) const;

    /// Enumerate every non-directory file known to this manager (loose GGPK
    /// files AND bundle2 contents). Visitor receives the forward-slash full
    /// path, file size (-1 if unknown) and the opaque nodePtr. Walks the
    /// internal FileEntry tree — covers the same set as FindNodeByPath.
    void ForEachFile(
        const std::function<void(const std::string& fullPath, int64_t fileSize, void* nodePtr)>& visitor) const;

    /// Read file content by FileRecord* or GGPKTreeNode*.
    Result<std::vector<uint8_t>> ReadFile(void* nodePtr) const;

    /// Replace file content in memory (Bundle mode only).
    /// Does NOT persist to disk -- call FlushWrites() afterwards.
    bool WriteFile(void* nodePtr, const std::vector<uint8_t>& data);

    /// Persist all pending writes to disk.
    /// For StandaloneGGPK: calls BundledGGPK::SaveIndex() (writes index + renews hashes).
    /// For SteamBundles: calls Index::Save().
    /// Returns true on success (or if nothing to flush).
    bool FlushWrites();

    /// Check whether there are unflushed writes.
    bool HasPendingWrites() const { return m_hasPendingWrites; }

    /// Invalidate the path lookup cache (e.g. after reload).
    void InvalidatePathCache();

    /// Try to auto-detect game installation directory.
    static std::string AutoDetectGameDir();

    /// Initialize Oodle DLL from the given directory.
    static bool InitOodle(const std::string& dir);

private:
    SourceMode m_mode = SourceMode::None;
    std::atomic<bool> m_loading{false};
    std::string m_openedPath;
    std::string m_error;
    FileEntry m_root;

    // Standalone GGPK
    std::unique_ptr<GGPK::GGPKFile> m_ggpk;

    // Steam Bundles
    std::unique_ptr<GGPK::Index> m_index;

    // BundledGGPK (wraps both)
    std::unique_ptr<GGPK::BundledGGPK> m_bundledGgpk;

    // Stored directory tree root (kept alive for nodePtr validity)
    std::unique_ptr<GGPK::DirectoryNode> m_bundleTreeRoot;

    // Dirty flag -- set by WriteFile(), cleared by FlushWrites() / Close()
    bool m_hasPendingWrites = false;

    // Path lookup cache: lowercase path -> nodePtr (built lazily on first FindNodeByPath call)
    mutable std::unordered_map<std::string, void*> m_pathCache;
    mutable bool m_pathCacheBuilt = false;
    void BuildPathCache() const;

    void BuildTreeFromGGPK();
    void BuildTreeFromBundles();
    void BuildTreeFromBundledGGPK();
    void PopulateFromGGPKNode(FileEntry& entry, GGPK::GGPKTreeNode* node);
    void PopulateFromBundleDir(FileEntry& entry, GGPK::DirectoryNode* dir);
};

} // namespace GGPKExp
