#pragma once
#include <algorithm>
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

    /// The bundle's files in the order they were added (removed ones dropped). Not
    /// thread-safe against AddFile/RemoveFile (the Index calls both under its write lock).
    const std::vector<FileRecord*>& GetFiles() const {
        Compact();
        return m_files;
    }
    void AddFile(FileRecord* f) { m_files.push_back(f); }

    /// Every FileRecord::Write that redirects a game file calls it (~455k times in a
    /// PotatoMode Apply). O(log n) amortized instead of a linear find + erase: the slot is
    /// cleared (a hole) and found through a sorted (pointer, slot) lookup built the first
    /// time a bundle loses a file; the list is compacted, order kept, once half of it is
    /// holes. The lookup costs memory only for bundles that lose files, until they compact;
    /// a slot per FileRecord would grow every record's heap block (+16 bytes, about 66 MB on
    /// a 4.27M-file index).
    void RemoveFile(FileRecord* f) {
        if (!RemoveFromLookup(f)) {
            // Small bundle, or a file added after the lookup was built.
            auto it = std::find(m_files.begin() + static_cast<std::ptrdiff_t>(m_lookupCovers), m_files.end(), f);
            if (it == m_files.end()) {
                it = std::find(m_files.begin(), m_files.end(), f);
                if (it == m_files.end()) return;
            }
            *it = nullptr;
        }
        if (++m_holes > 64 && m_holes * 2 > m_files.size())
            Compact();
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
    static constexpr size_t kLookupMinFiles = 64;

    /// Clears f's slot through the lookup; false when the lookup does not hold f.
    bool RemoveFromLookup(FileRecord* f) {
        if (m_lookup.empty()) {
            if (m_files.size() < kLookupMinFiles)
                return false;
            m_lookup.reserve(m_files.size());
            for (size_t i = 0; i < m_files.size(); ++i)
                if (m_files[i]) m_lookup.emplace_back(m_files[i], i);
            std::sort(m_lookup.begin(), m_lookup.end());
            m_lookupCovers = m_files.size();
        }
        auto it = std::lower_bound(m_lookup.begin(), m_lookup.end(), std::make_pair(f, size_t{0}));
        if (it == m_lookup.end() || it->first != f || m_files[it->second] != f)
            return false;
        m_files[it->second] = nullptr;
        return true;
    }

    // Holes (nullptr) left by RemoveFile until the next Compact; never visible outside.
    void Compact() const {
        if (m_holes == 0) return;
        m_files.erase(std::remove(m_files.begin(), m_files.end(), nullptr), m_files.end());
        m_holes = 0;
        m_lookup.clear();
        m_lookup.shrink_to_fit();
        m_lookupCovers = 0;
    }

    mutable std::vector<FileRecord*> m_files;
    mutable size_t m_holes = 0;
    mutable std::vector<std::pair<FileRecord*, size_t>> m_lookup;   // sorted by pointer
    mutable size_t m_lookupCovers = 0;                              // m_files slots it covers
};

} // namespace GGPK
