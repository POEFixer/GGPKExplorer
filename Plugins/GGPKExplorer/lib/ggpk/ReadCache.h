#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace GGPK {

class Bundle;
class BundleRecord;
class IBundleFactory;

/// Cheap diagnostics of an Index's reads (Index::GetReadCacheStats). Counters only grow;
/// the last three are the cache's current contents.
struct ReadCacheStats {
    uint64_t bundleOpens = 0;      // read-only bundle opens (the bundle was not held)
    uint64_t bundleHits = 0;       // reads that found their bundle already open
    uint64_t chunksDecoded = 0;    // chunks decompressed by reads (FileRecord::Read and ReadFiles)
    uint64_t chunkHits = 0;        // chunks served from the decoded-chunk cache
    uint64_t bytesDecoded = 0;     // decompressed bytes
    uint64_t staleBundles = 0;     // held custom bundles dropped: their file changed on disk
    uint64_t batchRunFailures = 0; // ReadFiles runs that failed to decode (their files read one by one)
    size_t openBundles = 0;        // read-only bundles held now
    size_t cachedChunks = 0;       // decoded chunks held now
    size_t cachedChunkBytes = 0;   // their bytes
};

/// One decoded chunk shared by the cache and the readers using it.
struct DecodedChunk {
    std::unique_ptr<uint8_t[]> data;
    int32_t size = 0;
};

/// Per-Index read cache (never static; freed with the Index): a small LRU of open
/// read-only bundles and a byte-budgeted LRU of decoded chunks, keyed by BundleRecord*.
/// Thread-safe. Entries are handed out as shared_ptr, so eviction or Clear never frees a
/// bundle or chunk a reader still uses. Bundles that cannot be read concurrently (a custom
/// factory's read-write open) are handed out but never kept. The Index invalidates a
/// bundle when it saves it and clears everything on Index::Save; a cached bundle is never
/// handed to the write path (which opens its own, read-write first). A bundle another Index
/// or program may rewrite (a custom LibGGPK3/ bundle: `validate`) is re-checked against its
/// file (size, last-write time, still named) when it is acquired and its last check is
/// kRecheckInterval old, and reopened, its chunks dropped, when the file changed: every
/// acquire that starts kRecheckInterval or more after a rewrite completed sees it, and a
/// burst of reads pays one check (about 2 us) per interval. Its chunks are dropped when it is
/// opened again after an eviction, and a cached chunk of another decoded size than the
/// bundle now gives is never served.
class ReadCache {
public:
    static constexpr size_t kDefaultMaxBundles = 32;
    static constexpr size_t kDefaultChunkBudget = 64u * 1024 * 1024;
    static constexpr std::chrono::milliseconds kRecheckInterval{1};

    ReadCache() = default;
    ReadCache(const ReadCache&) = delete;
    ReadCache& operator=(const ReadCache&) = delete;

    /// The record's read-only bundle: held, or opened through factory.OpenForRead (outside
    /// the lock). Every open updates the record's uncompressed size from the fresh header
    /// (as every bundle open always did). `validate`: the bundle's file may change under us
    /// (see the class comment; Bundle::IsUnchangedOnDisk). Throws what the factory throws.
    std::shared_ptr<Bundle> AcquireBundle(BundleRecord* record, IBundleFactory& factory, bool validate = false);

    /// A cached chunk of `decodedSize` bytes (a cached one of another size is stale: dropped).
    std::shared_ptr<const DecodedChunk> FindChunk(const BundleRecord* record, int32_t chunk, int32_t decodedSize);
    void PutChunk(const BundleRecord* record, int32_t chunk, std::shared_ptr<const DecodedChunk> data);

    /// Drop the bundle and its chunks (the bundle's file changed or will change).
    void InvalidateBundle(const BundleRecord* record);

    /// Drop everything (handles closed once no reader holds them).
    void Clear();

    /// maxBundles 0 keeps no bundle open between reads; chunkBudget 0 caches no chunk.
    void SetLimits(size_t maxBundles, size_t chunkBudgetBytes);

    /// How old a held custom bundle's last check may be before an acquire checks it again
    /// (kRecheckInterval by default; zero checks on every acquire).
    void SetRecheckInterval(std::chrono::nanoseconds interval);

    ReadCacheStats Stats() const;

    void CountDecoded(uint64_t chunks, uint64_t bytes) {
        m_chunksDecoded.fetch_add(chunks, std::memory_order_relaxed);
        m_bytesDecoded.fetch_add(bytes, std::memory_order_relaxed);
    }
    void CountChunkHit() { m_chunkHits.fetch_add(1, std::memory_order_relaxed); }
    void CountBatchRunFailure() { m_batchRunFailures.fetch_add(1, std::memory_order_relaxed); }

private:
    struct ChunkKey {
        const BundleRecord* record;
        int32_t chunk;
        bool operator==(const ChunkKey& o) const { return record == o.record && chunk == o.chunk; }
    };
    struct ChunkKeyHash {
        size_t operator()(const ChunkKey& k) const {
            return std::hash<const void*>()(k.record) ^ (static_cast<size_t>(k.chunk) * 0x9E3779B97F4A7C15ull);
        }
    };
    struct BundleEntry {
        const BundleRecord* record;
        std::shared_ptr<Bundle> bundle;
        bool validate;              // re-checked against its file
        int64_t checkedAt = 0;      // steady_clock ticks of its last check (or its open)
    };
    using ChunkEntry = std::pair<ChunkKey, std::shared_ptr<const DecodedChunk>>;

    void TrimLocked();
    /// Drops `record`'s bundle entry and its chunks.
    void DropLocked(const BundleRecord* record);

    mutable std::mutex m_mutex;
    int64_t m_recheckTicks = std::chrono::duration_cast<std::chrono::steady_clock::duration>(kRecheckInterval).count();
    size_t m_maxBundles = kDefaultMaxBundles;
    size_t m_chunkBudget = kDefaultChunkBudget;
    std::list<BundleEntry> m_bundleOrder;
    std::unordered_map<const BundleRecord*, std::list<BundleEntry>::iterator> m_bundles;
    std::list<ChunkEntry> m_chunkOrder;
    std::unordered_map<ChunkKey, std::list<ChunkEntry>::iterator, ChunkKeyHash> m_chunks;
    size_t m_chunkBytes = 0;

    std::atomic<uint64_t> m_bundleOpens{0};
    std::atomic<uint64_t> m_bundleHits{0};
    std::atomic<uint64_t> m_chunksDecoded{0};
    std::atomic<uint64_t> m_chunkHits{0};
    std::atomic<uint64_t> m_bytesDecoded{0};
    std::atomic<uint64_t> m_staleBundles{0};
    std::atomic<uint64_t> m_batchRunFailures{0};
};

} // namespace GGPK
