#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

namespace GGPK {

class BundleRecord;
class Bundle;
class Index;

/// A file entry in the Index, pointing to data inside a bundle.
class FileRecord {
public:
    /// Size of one record when serialized: uint64 pathHash + 3x int32 (bundleIndex, offset, size) = 20 bytes
    static constexpr int32_t RECORD_LENGTH = sizeof(uint64_t) + sizeof(int32_t) * 3;

    FileRecord(uint64_t pathHash, BundleRecord* bundleRecord, int32_t offset, int32_t size)
        : m_pathHash(pathHash)
        , m_bundleRecord(bundleRecord)
        , m_offset(offset)
        , m_size(size) {}

    uint64_t GetPathHash() const { return m_pathHash; }

    BundleRecord* GetBundleRecord() const { return m_bundleRecord.load(std::memory_order_relaxed); }

    int32_t GetOffset() const { return m_offset; }
    int32_t GetSize() const { return m_size; }

    /// Full path of the file (set by ParsePaths, may be empty if not parsed)
    const std::string& GetPath() const { return m_path; }
    void SetPath(std::string path) { m_path = std::move(path); }

    /// Read the content of this file from its bundle.
    /// If a bundle is provided, reads from it. Otherwise: the Index's pending write buffer
    /// first (a file written since the last save), else the bundle on disk through the
    /// Index's read cache (read-only handles, decoded chunks). Thread-safe against other
    /// readers and against Write/Save on other threads (they wait for each other).
    /// Never call it while holding the Index's ioMutex.
    std::vector<uint8_t> Read(Bundle* bundle = nullptr) const;

    /// Replace the content of this file.
    /// Redirects to a custom bundle in the Index.
    /// Call Index::Save() after all writes to persist changes.
    void Write(const std::vector<uint8_t>& newContent, bool saveIndex = false);
    void Write(const uint8_t* data, int32_t size, bool saveIndex = false);

    /// Redirect this file record to a different bundle/offset/size.
    void Redirect(BundleRecord* bundle, int32_t offset, int32_t size);

    /// Serialize to binary (for Index::Save)
    void Serialize(std::vector<uint8_t>& out) const;

private:
    uint64_t m_pathHash;
    // Atomic only so the hop to the Index (whose lock guards every change) is no data
    // race; every read and change of the file's location happens under that lock.
    std::atomic<BundleRecord*> m_bundleRecord;
    int32_t m_offset;
    int32_t m_size;
    std::string m_path;
};

} // namespace GGPK
