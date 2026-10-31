#pragma once
#include "FileAccess.h"
#include <array>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GGPK {

/// Tags for GGPK record types
namespace RecordTag {
    constexpr int32_t GGPK = 0x4B504747; // "GGPK"
    constexpr int32_t PDIR = 0x52494450; // "PDIR"
    constexpr int32_t FILE = 0x454C4946; // "FILE"
    constexpr int32_t FREE = 0x45455246; // "FREE"
}

/// A directory entry in a GGPK DirectoryRecord. Sorted by nameHash.
struct GGPKEntry {
    uint32_t nameHash;
    int64_t offset;
};

/// GGPK record types
struct GGPKRecordInfo {
    int64_t offset;
    int32_t length;
    int32_t tag;
};

/// A node in the GGPK file tree
struct GGPKTreeNode {
    std::string name;
    int64_t offset = 0;     // Record offset in file
    int32_t length = 0;     // Record length
    bool isDirectory = false;

    // SHA-256 hash from GGPK record (32 bytes)
    std::array<uint8_t, 32> hash{};

    // Parent node (nullptr for root)
    GGPKTreeNode* parent = nullptr;

    // For files:
    int64_t dataOffset = 0;
    int32_t dataLength = 0;

    // For directories:
    std::vector<GGPKEntry> entries;
    std::unordered_map<std::string, std::unique_ptr<GGPKTreeNode>> children;

    GGPKTreeNode* FindChild(const std::string& childName) {
        auto it = children.find(childName);
        return (it != children.end()) ? it->second.get() : nullptr;
    }

    /// Navigate a path like "Bundles2/_.index.bin"
    GGPKTreeNode* FindByPath(const std::string& path) {
        size_t start = 0;
        GGPKTreeNode* current = this;
        while (start < path.size() && current) {
            size_t slash = path.find('/', start);
            if (slash == std::string::npos) {
                return current->FindChild(path.substr(start));
            }
            current = current->FindChild(path.substr(start, slash - start));
            start = slash + 1;
        }
        return current;
    }
};

/// Handles Content.ggpk file for standalone PoE installations.
/// Simplified reader that navigates to Bundles2/_.index.bin for bundle-based patching.
///
/// Paths are UTF-8. Reads (the record walk at open, ReadAt / ReadFileContent and the bundles
/// read from inside it) go through a read-only handle with positional reads: thread-safe and
/// never blocking the game. Writes keep using the read-write stream under m_streamMutex.
class GGPKFile {
public:
    /// ReadWrite (default): Content.ggpk opened read-write for in-place writes; when that is
    /// refused (the running game) it opens read-only and the write methods throw a clear
    /// error naming why. ReadOnly: never asks for write access.
    explicit GGPKFile(const std::string& filePath, OpenMode mode = OpenMode::ReadWrite);
    ~GGPKFile();

    GGPKFile(const GGPKFile&) = delete;
    GGPKFile& operator=(const GGPKFile&) = delete;

    /// Get the root directory node
    GGPKTreeNode* GetRoot() { return m_root.get(); }

    /// Read file content from a GGPK file node
    std::vector<uint8_t> ReadFileContent(const GGPKTreeNode* node);

    /// Thread-safe positional read; a short read returns the bytes that exist, an error {}.
    std::vector<uint8_t> ReadAt(int64_t offset, int32_t size);

    /// Thread-safe positional read of exactly `size` bytes; throws std::runtime_error(what).
    void ReadExact(int64_t offset, void* buffer, size_t size,
                   const std::string& what = "Failed to read from GGPK file") const;

    /// Size of Content.ggpk when it was opened.
    int64_t GetFileSize() const { return m_readFile.Size(); }

    /// True when opened ReadOnly or when the read-write open was refused.
    bool IsReadOnly() const { return m_readOnly; }

    /// Why the file is read-only (empty when writable).
    const std::string& GetReadOnlyReason() const { return m_readOnlyReason; }

    /// Write file content to a GGPK file node (must fit in existing space or use free space)
    void WriteFileContent(GGPKTreeNode* node, const uint8_t* data, int32_t size);

    /// Get the version of the GGPK file
    uint32_t GetVersion() const { return m_version; }

    /// Get the file path
    const std::string& GetFilePath() const { return m_filePath; }

    /// Replace file content, relocating the record if the new data doesn't fit.
    /// Updates the node's dataOffset/dataLength and parent directory entry.
    /// Note: only handles ASCII file names (safe for all Bundles2/ content).
    void ReplaceFileContent(GGPKTreeNode* node, const uint8_t* data, int32_t size,
                            GGPKTreeNode* parentDir = nullptr);

    /// Renew SHA-256 hashes for dirty directories.
    /// By default, Root and Root's direct children are NOT renewed,
    /// preserving the official hash chain so the game doesn't trigger re-patching.
    void RenewHashes(bool forceRenewRoot = false);

    /// Get the underlying stream (for sub-stream Bundle reads)
    std::fstream& GetStream() { return m_stream; }

    /// Mutex for thread-safe stream access
    std::mutex& GetStreamMutex() { return m_streamMutex; }

    /// Flush changes to disk
    void Flush() { if (!m_readOnly) m_stream.flush(); }

private:
    /// Read a record header at the given offset
    GGPKRecordInfo ReadRecordInfo(int64_t offset);

    /// Read one record (length, tag and the bytes its header needs) in one read where it
    /// fits m_scratch, a second read only for the rest of a long record.
    int32_t LoadRecord(int64_t offset, size_t needAtLeast);

    /// Make sure m_scratch holds `size` bytes of the record at `offset` (reads the rest).
    void EnsureRecordBytes(int64_t offset, size_t size);

    /// Read and populate a directory node
    void ReadDirectoryRecord(int64_t offset, GGPKTreeNode& node);

    /// Read and populate a file node
    void ReadFileRecord(int64_t offset, GGPKTreeNode& node);

    /// Populate from the record bytes already in m_scratch.
    void ParseDirectoryRecord(int64_t offset, GGPKTreeNode& node);
    void ParseFileRecord(int64_t offset, GGPKTreeNode& node);

    /// Throws a clear error when the file is read-only.
    void ThrowIfReadOnly(const char* operation) const;

    /// Recursively read the tree starting from a directory node
    void ReadTree(GGPKTreeNode& dirNode);

    /// Compute SHA-256 of a data buffer (uses Windows BCrypt API)
    static bool ComputeSHA256(const uint8_t* data, size_t size, uint8_t outHash[32]);

    /// Write a node's stored hash to its correct position in the GGPK file.
    /// FILE records: hash at offset+12. PDIR records: hash at offset+16.
    void WriteNodeHash(GGPKTreeNode* node);

    /// Recompute a directory node's hash from concatenated children hashes
    void RenewDirectoryHash(GGPKTreeNode* dirNode);

    std::fstream m_stream;
    ReadOnlyFile m_readFile;
    bool m_readOnly = false;
    std::string m_readOnlyReason;
    std::vector<uint8_t> m_scratch;     // record walk at open (single-threaded)
    size_t m_scratchValid = 0;
    std::string m_filePath;
    uint32_t m_version = 3;
    int64_t m_rootOffset = 0;
    int64_t m_firstFreeOffset = 0;
    std::unique_ptr<GGPKTreeNode> m_root;
    std::mutex m_streamMutex;

    /// Directories whose hashes need recalculation (parents of modified files)
    std::unordered_set<GGPKTreeNode*> m_dirtyHashes;
};

} // namespace GGPK
