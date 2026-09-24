#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace GGPK {

class Index;
class Bundle;
class FileRecord;

/// Metadata for one bundle in the Index.
class BundleRecord {
public:
    BundleRecord(std::string path, int32_t uncompressedSize, Index* index, int32_t bundleIndex)
        : m_path(std::move(path))
        , m_uncompressedSize(uncompressedSize)
        , m_index(index)
        , m_bundleIndex(bundleIndex) {}

    /// Path without ".bundle.bin" extension (as stored in Index)
    const std::string& GetPathBase() const { return m_path; }

    /// Full path with ".bundle.bin" extension
    std::string GetPath() const { return m_path + ".bundle.bin"; }

    int32_t GetUncompressedSize() const { return m_uncompressedSize; }
    void SetUncompressedSize(int32_t size) { m_uncompressedSize = size; }

    int32_t GetBundleIndex() const { return m_bundleIndex; }

    Index* GetIndex() const { return m_index; }

    const std::vector<FileRecord*>& GetFiles() const { return m_files; }
    void AddFile(FileRecord* f) { m_files.push_back(f); }
    void RemoveFile(FileRecord* f) {
        auto it = std::find(m_files.begin(), m_files.end(), f);
        if (it != m_files.end()) m_files.erase(it);
    }

    /// Try to get a Bundle instance via the factory
    std::unique_ptr<Bundle> TryGetBundle() const;

    /// Size when serialized to Index binary
    int32_t GetRecordLength() const {
        return static_cast<int32_t>(m_path.size()) + sizeof(int32_t) + sizeof(int32_t);
    }

    /// Serialize to binary (for Index::Save)
    void Serialize(std::vector<uint8_t>& out) const;

private:
    friend class Index;

    std::string m_path;
    int32_t m_uncompressedSize;
    Index* m_index;
    int32_t m_bundleIndex;
    std::vector<FileRecord*> m_files;
};

} // namespace GGPK
