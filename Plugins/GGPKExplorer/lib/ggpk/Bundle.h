#pragma once
#include "Oodle.h"
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace GGPK {

class BundleRecord;

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

/// Handles reading and writing of *.bundle.bin files.
/// Each bundle contains Oodle-compressed data split into chunks.
class Bundle {
public:
    /// Open an existing bundle file from disk.
    explicit Bundle(const std::string& filePath, BundleRecord* record = nullptr);

    /// Open an existing bundle from an in-memory buffer.
    /// If ownsData is true, the bundle takes ownership of the stream data.
    Bundle(std::vector<uint8_t> data, BundleRecord* record = nullptr);

    /// Create an empty bundle for writing (used by Index::CreateBundle).
    /// filePath is the on-disk location, used for error messages and truncation;
    /// pass empty when the bundle is not backed by a known path.
    Bundle(std::fstream stream, BundleRecord* record, std::string filePath = {});

    /// Create a bundle that reads from a sub-region of a parent stream.
    /// The parent stream and mutex are NOT owned by this Bundle.
    /// Used by GGPKBundleFactory to read bundles directly from Content.ggpk.
    Bundle(std::fstream& parentStream, std::mutex& parentMutex,
           int64_t baseOffset, int32_t totalSize, BundleRecord* record = nullptr);

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
    /// running game). Reading works; Save()/WriteRaw() will throw.
    bool IsReadOnly() const { return m_readOnly; }

    /// True only when backed by a writable destination: an open on-disk file
    /// stream, or an in-memory buffer. A GGPK sub-stream (custom bundle whose
    /// on-disk file was missing, resolved read-only from inside Content.ggpk) and
    /// a read-only file open are NOT writable — Save()/WriteRaw() would fail with a
    /// cryptic badbit. GetBundleToWrite() skips non-writable bundles and creates a
    /// fresh on-disk custom bundle instead.
    bool IsWritable() const {
        return m_isMemoryBased || (!m_isSubStream && !m_readOnly && m_fileStream.is_open());
    }

    int32_t GetUncompressedSize() const { return m_header.uncompressed_size; }
    void SetUncompressedSize(int32_t size) { m_header.uncompressed_size = size; }

    int32_t GetCompressedSize() const { return m_header.compressed_size; }

    /// Read the entire decompressed content of the bundle.
    std::vector<uint8_t> ReadAll();

    /// Read a portion of the decompressed content.
    /// Returns a buffer containing the requested range.
    std::vector<uint8_t> Read(int32_t offset, int32_t length);

    /// Save the bundle with new content, compressing it into chunks.
    void Save(const uint8_t* newContent, int32_t contentSize,
              Oodle::CompressionLevel level = Oodle::CompressionLevel::Normal);

    void Save(const std::vector<uint8_t>& newContent,
              Oodle::CompressionLevel level = Oodle::CompressionLevel::Normal) {
        Save(newContent.data(), static_cast<int32_t>(newContent.size()), level);
    }

private:
    /// Decompress chunks [start, end) into the output buffer.
    void ReadChunks(uint8_t* output, int32_t start, int32_t end);

    /// Seek to the start of compressed data for chunk `start`.
    int64_t GetChunkDataOffset(int32_t start) const;

    BundleRecord* m_record = nullptr;
    BundleHeader m_header{};
    std::vector<int32_t> m_compressedChunkSizes;

    // Storage - either file-based or memory-based
    std::fstream m_fileStream;
    std::vector<uint8_t> m_memoryData;
    bool m_isMemoryBased = false;
    bool m_readOnly = false;
    std::string m_filePath;

    // Sub-stream mode: reads from parent stream at baseOffset
    std::fstream* m_parentStream = nullptr;
    std::mutex* m_parentMutex = nullptr;
    int64_t m_parentBaseOffset = 0;
    bool m_isSubStream = false;

    // Helper to read raw bytes from the underlying storage
    void ReadRaw(int64_t offset, void* buffer, size_t size);

    /// Read raw bytes assuming the caller already holds the parent mutex.
    /// Used by ReadChunks() to avoid per-chunk lock/unlock overhead.
    void ReadRawLocked(int64_t offset, void* buffer, size_t size);
    void WriteRaw(int64_t offset, const void* buffer, size_t size);
    void TruncateStorage(int64_t newSize);
    void FlushStorage();
};

} // namespace GGPK
