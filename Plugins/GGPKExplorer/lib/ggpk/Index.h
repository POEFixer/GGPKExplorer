#pragma once
#include "Bundle.h"
#include "BundleRecord.h"
#include "FileRecord.h"
#include "NameHash.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace GGPK {

class IBundleFactory;
class DirectoryNode;
class FileNode;

/// Index DirectoryRecord as stored in the binary index data.
#pragma pack(push, 4)
struct IndexDirectoryRecord {
    uint64_t pathHash;
    int32_t offset;
    int32_t size;
    int32_t recursiveSize;
};
#pragma pack(pop)
static_assert(sizeof(IndexDirectoryRecord) == 20, "IndexDirectoryRecord must be 20 bytes");

/// Manages the _.index.bin file - the master index of all bundled files.
/// Supports both Steam (disk-based) and Standalone (GGPK-based) installations.
class Index {
public:
    /// Open an index from a file path (Steam/Epic: path to _.index.bin).
    /// The base directory (parent of _.index.bin) is used for bundle resolution.
    explicit Index(const std::string& filePath, bool parsePaths = true,
                   std::unique_ptr<IBundleFactory> factory = nullptr);

    /// Open an index from raw bundle data (for BundledGGPK).
    Index(std::vector<uint8_t> bundleData, bool parsePaths = true,
          std::unique_ptr<IBundleFactory> factory = nullptr);

    ~Index();

    Index(const Index&) = delete;
    Index& operator=(const Index&) = delete;

    /// Maximum bundle size for writing (default 200 MiB)
    int32_t maxBundleSize = 200 * 1024 * 1024;

    /// Get all bundle records
    const std::vector<std::unique_ptr<BundleRecord>>& GetBundles() const { return m_bundles; }

    /// Get all file records (hash -> FileRecord*)
    const std::unordered_map<uint64_t, std::unique_ptr<FileRecord>>& GetFiles() const { return m_files; }

    /// Parse all file paths from the directory bundle data.
    /// Returns number of paths that failed to parse.
    int ParsePaths();

    /// Build the directory tree. ParsePaths() must have been called first.
    std::unique_ptr<DirectoryNode> BuildTree(bool ignoreNullPath = false);

    /// Get a file by its full path (forward-slash separated).
    /// Does NOT require BuildTree().
    FileRecord* TryGetFile(const std::string& path);

    /// Compute the name hash for a path.
    uint64_t ComputeNameHash(const std::string& path) const;

    /// Save the index back to disk. Must be called after Write() operations.
    void Save();

    /// Get the base bundle (for BundledGGPK write-back)
    Bundle* GetBaseBundle() { return m_baseBundle.get(); }

    /// Get the bundle factory
    IBundleFactory* GetFactory() const { return m_factory.get(); }

    /// Global mutex for thread-safe file I/O
    std::mutex ioMutex;

private:
    friend class FileRecord;
    friend class BundleRecord;

    /// Parse the raw binary index data.
    void ParseIndexData(const std::vector<uint8_t>& data);

    /// Get an available custom bundle for writing, or create a new one.
    /// Returns the bundle and sets originalSize to the existing data size.
    Bundle* GetBundleToWrite(int32_t& originalSize);

    /// Create a new bundle and add it to the bundles list.
    Bundle* CreateBundle(const std::string& bundlePath);

    std::unique_ptr<IBundleFactory> m_factory;
    std::unique_ptr<Bundle> m_baseBundle;

    std::vector<std::unique_ptr<BundleRecord>> m_bundles;
    std::unordered_map<uint64_t, std::unique_ptr<FileRecord>> m_files;
    std::vector<IndexDirectoryRecord> m_directories;
    std::vector<uint8_t> m_directoryBundleData;

    // Custom bundles created by this library for writing
    std::vector<BundleRecord*> m_customBundles;

    // Currently open bundle for writing
    Bundle* m_bundleToWrite = nullptr;
    std::unique_ptr<Bundle> m_bundleToWriteOwned;
    std::vector<uint8_t> m_writeBuffer;

    bool m_pathsParsed = false;
    bool m_useMurmurHash = true;

    static constexpr const char* CUSTOM_BUNDLE_BASE_PATH = "LibGGPK3/";
};

} // namespace GGPK
