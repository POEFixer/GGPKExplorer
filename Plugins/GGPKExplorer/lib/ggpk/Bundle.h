#pragma once
#include "FileAccess.h"
#include "Oodle.h"
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace GGPK {

class BundleRecord;
class GGPKFile;

/// Build a human-readable reason for the most recent failed CRT/fstream open.
/// Must be called immediately after the failed open (reads errno/_doserrno).
std::string DescribeLastOpenError();

/// Build a human-readable reason for the most recent failed write/flush: the OS
/// error from errno/_doserrno plus free space on the target volume (path may be
/// empty, in which case free space is omitted). Call immediately after the failure.
std::string DescribeWriteError(const std::string& path);

/// Bundle file header - 60 bytes, matches the binary layout exactly.
#pragma pack(push, 4)
struct BundleHeader {
    int32_t uncompressed_size = 0;
    int32_t compressed_size = 0;
    int32_t head_size = 48;               // chunk_count * 4 + 48
    int32_t compressor = 13;              // Leviathan
    int32_t unknown = 1;
    int64_t uncompressed_size_long = 0;
    int64_t compressed_size_long = 0;
    int32_t chunk_count = 0;
    int32_t chunk_size = 256 * 1024;      // 262144
    int32_t unknown3 = 0;
    int32_t unknown4 = 0;
    int32_t unknown5 = 0;
    int32_t unknown6 = 0;

    int32_t GetLastChunkSize() const {
        return uncompressed_size - (chunk_size * (chunk_count - 1));
    }
};
#pragma pack(pop)
static_assert(sizeof(BundleHeader) == 60, "BundleHeader must be exactly 60 bytes");

/// Largest chunk_size a header may declare. Game bundles and LibGGPK3 use 256 KiB; the
/// bound only refuses absurd values (a corrupt header) before they size any buffer.
constexpr int32_t kMaxBundleChunkSize = 16 * 1024 * 1024;

/// Handles reading and writing of *.bundle.bin files.
/// Each bundle contains Oodle-compressed data split into chunks.
///
/// Thread safety: a bundle backed by a read-only file, a GGPK sub-stream or memory reads
/// positionally and may be read (Read / ReadAll / ReadInto / DecodeChunk) from any number of
/// threads at once. A bundle backed by an fstream (read-write opens, write targets) is
/// single-threaded, as before.
class Bundle {
public:
    /// Open an existing bundle file from disk (path is UTF-8).
    /// ReadWrite: read-write first (custom bundles and _.index.bin need it for Save), falling
    /// back to read-only when that is refused. ReadOnly: a read-only handle shared for
    /// read/write/delete; Save() throws.
    explicit Bundle(const std::string& filePath, BundleRecord* record = nullptr,
                    OpenMode mode = OpenMode::ReadWrite);

    /// Open an existing bundle from an in-memory buffer (the bundle owns it).
    Bundle(std::vector<uint8_t> data, BundleRecord* record = nullptr);

    /// Create an empty bundle for writing (used by Index::CreateBundle).
    /// filePath is the on-disk location, used for error messages and truncation;
    /// pass empty when the bundle is not backed by a known path.
    Bundle(std::fstream stream, BundleRecord* record, std::string filePath = {});

    /// A bundle stored inside Content.ggpk at [baseOffset, baseOffset + totalSize). Reads are
    /// positional on the GGPK's read-only handle and never leave that region. The GGPKFile is
    /// not owned and must outlive the bundle. Never writable.
    Bundle(const GGPKFile& parent, int64_t baseOffset, int64_t totalSize, BundleRecord* record = nullptr);

    ~Bundle() = default;

    Bundle(const Bundle&) = delete;
    Bundle& operator=(const Bundle&) = delete;
    Bundle(Bundle&&) = default;
    Bundle& operator=(Bundle&&) = default;

    BundleRecord* GetRecord() const { return m_record; }

    /// Get raw storage data (memory-based bundles only). Returns empty vector for file-based.
    const std::vector<uint8_t>& GetRawMemoryData() const { return m_memoryData; }

    /// Check if this is a memory-based bundle
    bool IsMemoryBased() const { return m_isMemoryBased; }

    /// True when the file could only be opened read-only (e.g. locked by the
    /// running game) or was opened OpenMode::ReadOnly. Reading works; Save()/WriteRaw() throw.
    bool IsReadOnly() const { return m_readOnly; }

    /// Why a ReadWrite open fell back to read-only (empty otherwise).
    const std::string& GetReadOnlyReason() const { return m_readOnlyReason; }

    /// True only when backed by a writable destination: an open on-disk file
    /// stream, or an in-memory buffer. A GGPK sub-stream (custom bundle whose
    /// on-disk file was missing, resolved read-only from inside Content.ggpk) and
    /// a read-only file open are NOT writable — Save()/WriteRaw() would fail with a
    /// cryptic badbit. GetBundleToWrite() skips non-writable bundles and creates a
    /// fresh on-disk custom bundle instead.
    bool IsWritable() const {
        return m_isMemoryBased || (!m_isSubStream && !m_readOnly && m_fileStream.is_open());
    }

    /// True for storage that is read positionally (read-only file, GGPK sub-stream, memory):
    /// such a bundle may be shared by concurrent readers.
    bool IsConcurrentReadable() const { return m_isMemoryBased || m_isSubStream || m_readFile.IsOpen(); }

    /// False when the file of a read-only file bundle changed since it was opened (size,
    /// last-write time, or no longer at its name): its header and chunk table may be stale.
    /// Other storages (GGPK sub-stream, memory, read-write stream) report true.
    bool IsUnchangedOnDisk() const { return !m_readFile.IsOpen() || m_readFile.Unchanged(); }

    int32_t GetUncompressedSize() const { return m_header.uncompressed_size; }
    void SetUncompressedSize(int32_t size) { m_header.uncompressed_size = size; }

    int32_t GetCompressedSize() const { return m_header.compressed_size; }
    int32_t GetChunkSize() const { return m_header.chunk_size; }
    int32_t GetChunkCount() const { return m_header.chunk_count; }

    /// Decoded size of chunk `index` (chunk_size, or the short last chunk).
    int32_t GetChunkDecodedSize(int32_t index) const {
        return index == m_header.chunk_count - 1 ? m_header.GetLastChunkSize() : m_header.chunk_size;
    }

    /// Read the entire decompressed content of the bundle.
    std::vector<uint8_t> ReadAll();

    /// Read a portion of the decompressed content.
    /// Returns a buffer containing the requested range.
    std::vector<uint8_t> Read(int32_t offset, int32_t length);

    /// Decode [offset, offset + length) into `out` (at least `length` bytes). Chunks fully
    /// inside the range decode straight into `out`, in parallel when there are many and
    /// `allowParallel`; the partial first/last chunk goes through a per-thread buffer.
    /// Throws std::out_of_range on a bad range, std::runtime_error on corrupt data.
    void ReadInto(int32_t offset, int32_t length, uint8_t* out, bool allowParallel = true);

    /// Decode chunks [first, end) into `out` laid out at chunk_size stride (the last chunk of
    /// the bundle may be short). One contiguous compressed read per window of chunks.
    void DecodeChunks(int32_t first, int32_t end, uint8_t* out, bool allowParallel = true);

    /// Release the file handle (read-only file bundles). Later reads throw.
    void CloseStorage();

    /// Save the bundle with new content, compressing it into chunks.
    void Save(const uint8_t* newContent, int32_t contentSize,
              Oodle::CompressionLevel level = Oodle::CompressionLevel::Normal);

    void Save(const std::vector<uint8_t>& newContent,
              Oodle::CompressionLevel level = Oodle::CompressionLevel::Normal) {
        Save(newContent.data(), static_cast<int32_t>(newContent.size()), level);
    }

private:
    /// Offset of chunk `start`'s compressed data (prefix sums of the size table).
    int64_t GetChunkDataOffset(int32_t start) const {
        return m_chunkOffsets[static_cast<size_t>(start)];
    }

    /// Validate the size table and build m_chunkOffsets (after the table is read or saved).
    void BuildChunkOffsets();

    /// Read the header + size table from positional storage and validate them.
    void LoadHeaderAndTable(int64_t storageSize, const std::string& source);

    BundleRecord* m_record = nullptr;
    BundleHeader m_header{};
    std::vector<int32_t> m_compressedChunkSizes;
    std::vector<int64_t> m_chunkOffsets;      // chunk_count + 1 entries

    // Storage - either file-based or memory-based
    std::fstream m_fileStream;
    ReadOnlyFile m_readFile;                   // OpenMode::ReadOnly file bundles
    std::vector<uint8_t> m_memoryData;
    bool m_isMemoryBased = false;
    bool m_readOnly = false;
    std::string m_readOnlyReason;
    std::string m_filePath;

    // Sub-stream mode: reads from the parent GGPK at baseOffset
    const GGPKFile* m_parent = nullptr;
    int64_t m_parentBaseOffset = 0;
    int64_t m_subSize = 0;
    bool m_isSubStream = false;

    // Helper to read raw bytes from the underlying storage
    void ReadRaw(int64_t offset, void* buffer, size_t size);
    void WriteRaw(int64_t offset, const void* buffer, size_t size);
    void TruncateStorage(int64_t newSize);
    void FlushStorage();
};

} // namespace GGPK
