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
    class Index;
    class BundledGGPK;
    class FileRecord;
    class DirectoryNode;
}

namespace GGPKExp {

class AsyncTaskRunner;

enum class SourceMode { None, StandaloneGGPK, SteamBundles };

/// Unified file entry for VirtualTree consumption.
/// nodePtr is the file's GGPK::FileRecord* (both modes read through the bundle index);
/// null for directories.
struct FileEntry {
    std::string name;
    std::string fullPath;
    int64_t fileSize = -1;
    bool isDirectory = false;
    void* nodePtr = nullptr; // opaque -- GGPK::FileRecord*
    std::vector<FileEntry> children;
};

/// Every path that crosses this class is UTF-8 (the library's convention: converted with
/// CP_UTF8, never the ANSI code page).
class GGPKManager {
public:
    GGPKManager();
    ~GGPKManager();

    Result<void> Open(const std::string& path);
    std::future<Result<void>> OpenAsync(const std::string& path, AsyncTaskRunner& runner);
    void Close();
    bool IsOpen() const { return m_mode != SourceMode::None; }
    SourceMode GetMode() const { return m_mode; }
    const std::string& GetError() const { return m_error; }
    const std::string& GetOpenedPath() const { return m_openedPath; }

    /// Get the root file entry (tree of all files).
    const FileEntry& GetRoot() const { return m_root; }

    /// Find a file by its tree path ('/'-separated, case-insensitive for A-Z). Returns the
    /// nodePtr or nullptr. A hash lookup in the index (no cache, safe from any thread).
    void* FindNodeByPath(const std::string& path) const;

    /// Enumerate every non-directory file known to this manager (loose GGPK
    /// files AND bundle2 contents). Visitor receives the forward-slash full
    /// path, file size (-1 if unknown) and the opaque nodePtr. Walks the
    /// internal FileEntry tree — covers the same set as FindNodeByPath.
    void ForEachFile(
        const std::function<void(const std::string& fullPath, int64_t fileSize, void* nodePtr)>& visitor) const;

    /// Read file content by FileRecord*.
    Result<std::vector<uint8_t>> ReadFile(void* nodePtr) const;

    /// The open bundle index (nullptr when nothing is open): Index::ReadFiles batch reads and
    /// the read cache. Valid until Close().
    GGPK::Index* GetIndex() const;

    /// Releases the index's read cache (open bundle handles, decoded chunks): call it when a
    /// job that read many files ends.
    void TrimReadCache() const;

    /// Replace file content in memory (Bundle mode only).
    /// Does NOT persist to disk -- call FlushWrites() afterwards. On failure *error (when
    /// given) says why (e.g. the running game holds the files: the index is read-only).
    bool WriteFile(void* nodePtr, const std::vector<uint8_t>& data, std::string* error = nullptr);

    /// Persist all pending writes to disk.
    /// For StandaloneGGPK: calls BundledGGPK::SaveIndex() (writes index + renews hashes).
    /// For SteamBundles: calls Index::Save().
    /// Returns true on success (or if nothing to flush); on failure *error says why.
    bool FlushWrites(std::string* error = nullptr);

    /// Check whether there are unflushed writes.
    bool HasPendingWrites() const { return m_hasPendingWrites.load(); }

    /// Initialize Oodle DLL from the given directory (UTF-8; empty: the program's copy).
    static bool InitOodle(const std::string& dir);

private:
    SourceMode m_mode = SourceMode::None;
    std::string m_openedPath;
    std::string m_error;
    FileEntry m_root;

    // Steam Bundles
    std::unique_ptr<GGPK::Index> m_index;

    // BundledGGPK (wraps both)
    std::unique_ptr<GGPK::BundledGGPK> m_bundledGgpk;

    // Stored directory tree root (kept alive for nodePtr validity)
    std::unique_ptr<GGPK::DirectoryNode> m_bundleTreeRoot;

    // Dirty flag -- set by WriteFile() (any thread), cleared by FlushWrites() / Close()
    std::atomic<bool> m_hasPendingWrites{false};

    void BuildTreeFromIndex(GGPK::Index& index);
    void PopulateFromBundleDir(FileEntry& entry, GGPK::DirectoryNode* dir);
};

} // namespace GGPKExp
