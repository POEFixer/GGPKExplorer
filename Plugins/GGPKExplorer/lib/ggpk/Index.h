#pragma once
#include "Bundle.h"
#include "BundleRecord.h"
#include "FileRecord.h"
#include "NameHash.h"
#include "ReadCache.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
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

/// Options of Index::ReadFiles.
struct ReadFilesOptions {
    /// Worker threads; 0 = min(hardware threads, 4). With 1 everything (sink calls included)
    /// runs on the calling thread.
    unsigned workers = 0;
    /// Decoded bytes one worker holds per run of neighbouring files (a single larger file
    /// is read on its own into its own buffer).
    size_t maxRunBytes = 16u * 1024 * 1024;
    /// true: sink calls never overlap (the library serializes them); false: up to
    /// `workers` calls run at once on different threads.
    bool serializeSink = false;
};

/// Outcome of Index::ReadFiles.
struct ReadFilesResult {
    size_t delivered = 0;     // sink calls with data
    size_t failed = 0;        // sink calls with an error
    bool cancelled = false;   // stopped by the cancel flag; the rest was not delivered
};

/// sink(position in `records`, record, bytes, error). On success error == nullptr and
/// `bytes` holds the file (the sink may move it away); on failure `bytes` is empty and
/// *error says why. Called once per record unless cancelled.
using ReadFilesSink = std::function<void(size_t position, FileRecord* record,
                                         std::vector<uint8_t>&& bytes, const std::string* error)>;

/// Manages the _.index.bin file - the master index of all bundled files.
/// Supports both Steam (disk-based) and Standalone (GGPK-based) installations.
class Index {
public:
    /// Open an index from a file path (Steam/Epic: path to _.index.bin, UTF-8).
    /// The base directory (parent of _.index.bin) is used for bundle resolution.
    /// ReadWrite: _.index.bin read-write first; when only a read-only open succeeds (the
    /// running game) the index is read-only (IsReadOnly). ReadOnly: never asks for write
    /// access and releases _.index.bin once it is read. A read-only index refuses
    /// FileRecord::Write and Save with a clear error; reads always open bundles read-only.
    explicit Index(const std::string& filePath, bool parsePaths = true,
                   std::unique_ptr<IBundleFactory> factory = nullptr,
                   OpenMode mode = OpenMode::ReadWrite);

    /// Open an index from raw bundle data (for BundledGGPK).
    Index(std::vector<uint8_t> bundleData, bool parsePaths = true,
          std::unique_ptr<IBundleFactory> factory = nullptr,
          OpenMode mode = OpenMode::ReadWrite);

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
    /// Clears the read cache first (records may change bundles; bundles are rewritten).
    void Save();

    // --- Read cache -------------------------------------------------------------------
    // FileRecord::Read() (and ReadFiles) keep up to 32 read-only bundles open and up to
    // 64 MiB of decoded chunks (ReadCache). Release them after a job:
    void TrimReadCache() { m_readCache.Clear(); }
    /// maxBundles 0 keeps no bundle open between reads; chunkBudget 0 caches no chunk.
    void SetReadCacheLimits(size_t maxBundles, size_t chunkBudgetBytes) {
        m_readCache.SetLimits(maxBundles, chunkBudgetBytes);
    }
    ReadCacheStats GetReadCacheStats() const { return m_readCache.Stats(); }
    /// A held custom (LibGGPK3/) bundle is checked against its file when it is acquired and
    /// its last check is older than this (default ReadCache::kRecheckInterval, 1 ms; zero:
    /// on every acquire). This Index's own Write and Save never depend on it.
    void SetReadCacheRecheckInterval(std::chrono::nanoseconds interval) {
        m_readCache.SetRecheckInterval(interval);
    }

    // --- Batch read -------------------------------------------------------------------
    /// Read many files at once: sorted by (bundle, offset), whole bundles per worker, each
    /// run of neighbouring files decoded once (one contiguous compressed read per run)
    /// within options.maxRunBytes per worker; never a blind ReadAll of a bundle. Files whose
    /// data sits in the pending write buffer (or whose location changes during the batch)
    /// go through the normal FileRecord::Read path, as do the files of a run that failed to
    /// decode (so each gets exactly FileRecord::Read's outcome). A file's error (missing
    /// bundle, corrupt chunk, bad range) is reported to the sink and the batch goes on. `cancel` is checked
    /// per run and per file. The sink is called from the worker threads (see
    /// ReadFilesOptions) in bundle order, not in `records` order; write-buffer files are
    /// delivered last, on the calling thread. No library lock is held while the sink runs:
    /// it may Read, Write and Save (a Save may even remove a custom bundle the batch has
    /// still to read; its files are then read the normal way). An exception thrown by the
    /// sink stops the batch and is rethrown here after every worker has stopped; no
    /// exception ever escapes a worker thread. Null records are reported as errors.
    ///
    /// Which bytes a sink call gets when sink calls rewrite files: each file is checked
    /// against its location under the library's lock when its run is decoded (and its
    /// bundle reopened if a save rewrote it); the run's bytes are copies, handed out after
    /// that without the lock. So a file written (FileRecord::Write) before its run is
    /// decoded is delivered with its new bytes, through the normal read path after the
    /// batch's own runs. A file written after its run was decoded and before its own sink
    /// call is delivered with the bytes it had when its run was decoded, its content before
    /// that Write: a later file of the same run (a run spans at most maxRunBytes of decoded
    /// chunks), or with several workers a file of a run another worker decoded and has not
    /// handed out yet (serializeSink does not prevent this; workers 1 does). The record
    /// passed is the live one: its next FileRecord::Read gives the new bytes. A Save
    /// changes no bytes already decoded.
    ReadFilesResult ReadFiles(const std::vector<FileRecord*>& records, const ReadFilesSink& sink,
                              const std::atomic<bool>* cancel = nullptr, unsigned workers = 0);
    ReadFilesResult ReadFiles(const std::vector<FileRecord*>& records, const ReadFilesSink& sink,
                              const std::atomic<bool>* cancel, const ReadFilesOptions& options);

    /// Get the base bundle (for BundledGGPK write-back)
    Bundle* GetBaseBundle() { return m_baseBundle.get(); }

    /// Get the bundle factory
    IBundleFactory* GetFactory() const { return m_factory.get(); }

    /// True when writes are refused: opened ReadOnly, _.index.bin / Content.ggpk could
    /// only be opened read-only, or MarkReadOnly was called.
    bool IsReadOnly() const { return m_readOnly; }
    const std::string& GetReadOnlyReason() const { return m_readOnlyReason; }

    /// Refuse every later Write/Save with this reason (BundledGGPK: Content.ggpk read-only).
    void MarkReadOnly(std::string reason) {
        m_readOnly = true;
        m_readOnlyReason = std::move(reason);
    }

    /// Guards the pending write buffer and the write target (taken by FileRecord::Read,
    /// Write and Save; never hold it while calling them).
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

    /// Save under the caller's locks (m_storageMutex exclusive + ioMutex).
    void SaveLocked();

    /// Read [offset, offset + size) of `record`'s bundle on disk through the read cache.
    /// The caller holds m_storageMutex shared.
    std::vector<uint8_t> ReadFromBundle(BundleRecord* record, int32_t offset, int32_t size);

    /// The record's read-only bundle from the cache ("Failed to get bundle: ..." on error).
    std::shared_ptr<Bundle> AcquireReadBundle(BundleRecord* record);

    /// One decoded chunk of `bundle` (cache, else decoded and cached).
    std::shared_ptr<const DecodedChunk> GetChunk(Bundle& bundle, const BundleRecord* record, int32_t chunk);

    std::unique_ptr<IBundleFactory> m_factory;
    std::unique_ptr<Bundle> m_baseBundle;

    /// Readers hold it shared while they read bundle files; Write and Save hold it
    /// exclusive (they rewrite bundle files, move records, erase BundleRecords). Taken
    /// before ioMutex.
    mutable std::shared_mutex m_storageMutex;
    ReadCache m_readCache;
    /// Moves (under m_storageMutex exclusive) whenever bundle files may be rewritten, so a
    /// batch holding a bundle across lock scopes knows to reopen it.
    uint64_t m_storageGeneration = 0;

    std::vector<std::unique_ptr<BundleRecord>> m_bundles;
    /// BundleRecords Save removed (emptied custom bundles): kept until the Index is
    /// destroyed, never freed, so a pointer taken before a Save (a FileRecord::Read on
    /// another thread, a ReadFiles snapshot) never dangles. A handful per session.
    std::vector<std::unique_ptr<BundleRecord>> m_retiredBundles;
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
    bool m_readOnly = false;
    std::string m_readOnlyReason;

    /// Throws a clear error when the index is read-only.
    void ThrowIfReadOnly() const;

    static constexpr const char* CUSTOM_BUNDLE_BASE_PATH = "LibGGPK3/";
};

} // namespace GGPK
