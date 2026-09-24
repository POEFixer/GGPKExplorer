#include "Bundle.h"
#include "BundleRecord.h"
#include "FileRecord.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <stdexcept>

namespace GGPK {

std::string DescribeLastOpenError() {
    // Must run right after the failed open, before errno is overwritten.
    const int err = errno;
    const unsigned long oserr = _doserrno; // Win32 error captured by the CRT
    char buf[128]{};
    if (strerror_s(buf, sizeof(buf), err) != 0 || buf[0] == '\0')
        return "errno " + std::to_string(err);
    std::string msg = buf;
    if (oserr == 32ul) // ERROR_SHARING_VIOLATION
        msg += "; file is locked by another process - close the game and try again";
    else if (err == ENOENT)
        msg += "; file is missing - verify/repair the game files and try again";
    return msg;
}

std::string DescribeWriteError(const std::string& path) {
    // A buffered fstream surfaces the OS error only when its internal buffer is
    // flushed to disk, so a failed write()/flush() leaves the real reason in
    // errno/_doserrno (set by the CRT's last _write). Read them first, then
    // report free space on the target volume — together these distinguish the
    // actual cause (disk full vs. a lock held by antivirus/the game vs. a device
    // error) instead of the old blind "disk full or file locked?" guess.
    // Must run immediately after the failed write, before errno is overwritten.
    const int err = errno;
    const unsigned long oserr = _doserrno; // Win32 error captured by the CRT
    std::string msg;
    if (oserr != 0ul) {
        msg = "OS error " + std::to_string(oserr);
        switch (oserr) {
            case 112ul:   // ERROR_DISK_FULL
            case 39ul:    // ERROR_HANDLE_DISK_FULL
                msg += " - the drive is full"; break;
            case 33ul:    // ERROR_LOCK_VIOLATION
                msg += " - the file is locked (antivirus may be scanning it; add the game folder to AV exclusions)"; break;
            case 32ul:    // ERROR_SHARING_VIOLATION
                msg += " - the file is in use (close the game and any antivirus scanning it)"; break;
            case 5ul:     // ERROR_ACCESS_DENIED
                msg += " - access denied (run as administrator or check game-folder permissions)"; break;
            default: break;
        }
    } else if (err != 0) {
        char buf[128]{};
        if (strerror_s(buf, sizeof(buf), err) == 0 && buf[0] != '\0')
            msg = buf;
        else
            msg = "errno " + std::to_string(err);
    }
    if (!path.empty()) {
        std::error_code ec;
        const auto info = std::filesystem::space(
            std::filesystem::path(path).parent_path(), ec);
        if (!ec) {
            const unsigned long long freeMiB =
                static_cast<unsigned long long>(info.available) / (1024ull * 1024ull);
            if (!msg.empty()) msg += "; ";
            msg += std::to_string(freeMiB) + " MiB free on target drive";
        }
    }
    if (msg.empty())
        msg = "disk full or file locked?";
    return msg;
}

namespace {
// Bundle headers and chunk tables come straight from disk and are fully
// untrusted: a truncated/corrupt file (or a stale custom bundle left behind by
// an interrupted save) must fail with a clean error here — if these values
// leak into size arithmetic they turn into wild memmoves (crash_506).
void ValidateBundleHeader(const BundleHeader& h, const std::string& source) {
    const int64_t cs = h.chunk_size;
    const int64_t cc = h.chunk_count;
    const int64_t u  = h.uncompressed_size;
    const bool ok = cs > 0 && cc >= 0 && u >= 0 &&
                    cc * cs >= u &&
                    (cc == 0 ? u == 0 : (cc - 1) * cs < u);
    if (!ok)
        throw std::runtime_error("Corrupt bundle header in " + source
            + " (uncompressed_size=" + std::to_string(h.uncompressed_size)
            + ", chunk_count=" + std::to_string(h.chunk_count)
            + ", chunk_size=" + std::to_string(h.chunk_size) + ")");
}
} // namespace

Bundle::Bundle(const std::string& filePath, BundleRecord* record)
    : m_record(record)
    , m_filePath(filePath) {
    // Read-write first: custom bundles (LibGGPK3/*) and _.index.bin need write
    // access for Save(). Regular game bundles are only ever read during patching.
    m_fileStream.open(std::filesystem::path(filePath), std::ios::in | std::ios::out | std::ios::binary);
    if (!m_fileStream.is_open()) {
        // A running game (or antivirus) holds bundle files with deny-write
        // sharing, and read-only attributes also reject RW opens. Reading does
        // not need write access, so fall back to read-only before failing.
        m_fileStream.clear();
        m_fileStream.open(std::filesystem::path(filePath), std::ios::in | std::ios::binary);
        if (!m_fileStream.is_open())
            throw std::runtime_error("Failed to open bundle file: " + filePath
                                     + " (" + DescribeLastOpenError() + ")");
        m_readOnly = true;
    }

    // Read header
    m_fileStream.read(reinterpret_cast<char*>(&m_header), sizeof(BundleHeader));
    if (m_fileStream.gcount() != sizeof(BundleHeader))
        throw std::runtime_error("Failed to read bundle header from: " + filePath);

    ValidateBundleHeader(m_header, filePath);

    if (record)
        record->SetUncompressedSize(m_header.uncompressed_size);

    // Read compressed chunk sizes
    m_compressedChunkSizes.resize(static_cast<size_t>(m_header.chunk_count));
    if (m_header.chunk_count > 0) {
        const std::streamsize tableBytes =
            static_cast<std::streamsize>(m_header.chunk_count) * static_cast<std::streamsize>(sizeof(int32_t));
        m_fileStream.read(reinterpret_cast<char*>(m_compressedChunkSizes.data()), tableBytes);
        if (m_fileStream.gcount() != tableBytes)
            throw std::runtime_error("Truncated bundle (chunk size table): " + filePath);
    }
}

Bundle::Bundle(std::vector<uint8_t> data, BundleRecord* record)
    : m_record(record)
    , m_memoryData(std::move(data))
    , m_isMemoryBased(true) {
    if (m_memoryData.size() < sizeof(BundleHeader))
        throw std::runtime_error("Bundle data too small for header");

    std::memcpy(&m_header, m_memoryData.data(), sizeof(BundleHeader));

    ValidateBundleHeader(m_header, "memory bundle");

    if (record)
        record->SetUncompressedSize(m_header.uncompressed_size);

    m_compressedChunkSizes.resize(static_cast<size_t>(m_header.chunk_count));
    if (m_header.chunk_count > 0) {
        const size_t tableBytes = static_cast<size_t>(m_header.chunk_count) * sizeof(int32_t);
        if (m_memoryData.size() < sizeof(BundleHeader) + tableBytes)
            throw std::runtime_error("Bundle data too small for chunk size table");
        std::memcpy(m_compressedChunkSizes.data(),
                     m_memoryData.data() + sizeof(BundleHeader),
                     tableBytes);
    }
}

Bundle::Bundle(std::fstream stream, BundleRecord* record, std::string filePath)
    : m_record(record)
    , m_fileStream(std::move(stream))
    , m_filePath(std::move(filePath)) {
    // Empty bundle for writing
    m_compressedChunkSizes.clear();
    m_fileStream.seekp(0);
    m_fileStream.write(reinterpret_cast<const char*>(&m_header), sizeof(BundleHeader));
    if (!m_fileStream)
        throw std::runtime_error("Failed to initialize new bundle file");
    auto pos = m_fileStream.tellp();
    // Truncate by closing and reopening is not ideal, but we'll just track the logical end
}

Bundle::Bundle(std::fstream& parentStream, std::mutex& parentMutex,
               int64_t baseOffset, int32_t /*totalSize*/, BundleRecord* record)
    : m_record(record)
    , m_parentStream(&parentStream)
    , m_parentMutex(&parentMutex)
    , m_parentBaseOffset(baseOffset)
    , m_isSubStream(true) {

    // Read header and chunk sizes in a single lock scope
    std::lock_guard<std::mutex> lock(*m_parentMutex);

    m_parentStream->seekg(static_cast<std::streamoff>(m_parentBaseOffset));
    m_parentStream->read(reinterpret_cast<char*>(&m_header), sizeof(BundleHeader));
    if (m_parentStream->gcount() != sizeof(BundleHeader))
        throw std::runtime_error("Truncated bundle header in GGPK sub-stream");

    ValidateBundleHeader(m_header, "GGPK sub-stream");

    if (record)
        record->SetUncompressedSize(m_header.uncompressed_size);

    m_compressedChunkSizes.resize(static_cast<size_t>(m_header.chunk_count));
    if (m_header.chunk_count > 0) {
        // Stream is already positioned right after header -- no seek needed
        const std::streamsize tableBytes =
            static_cast<std::streamsize>(m_header.chunk_count) * static_cast<std::streamsize>(sizeof(int32_t));
        m_parentStream->read(reinterpret_cast<char*>(m_compressedChunkSizes.data()), tableBytes);
        if (m_parentStream->gcount() != tableBytes)
            throw std::runtime_error("Truncated bundle (chunk size table) in GGPK sub-stream");
    }
}

void Bundle::ReadRaw(int64_t offset, void* buffer, size_t size) {
    if (m_isSubStream) {
        std::lock_guard<std::mutex> lock(*m_parentMutex);
        ReadRawLocked(offset, buffer, size);
    } else {
        ReadRawLocked(offset, buffer, size);
    }
}

void Bundle::ReadRawLocked(int64_t offset, void* buffer, size_t size) {
    if (m_isMemoryBased) {
        if (static_cast<size_t>(offset) + size > m_memoryData.size())
            throw std::runtime_error("Read past end of memory bundle");
        std::memcpy(buffer, m_memoryData.data() + offset, size);
    } else if (m_isSubStream) {
        // Caller must hold *m_parentMutex
        m_parentStream->seekg(static_cast<std::streamoff>(m_parentBaseOffset + offset));
        m_parentStream->read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
        if (m_parentStream->gcount() != static_cast<std::streamsize>(size))
            throw std::runtime_error("Failed to read from GGPK sub-stream");
    } else {
        m_fileStream.seekg(static_cast<std::streamoff>(offset));
        m_fileStream.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
        if (m_fileStream.gcount() != static_cast<std::streamsize>(size))
            throw std::runtime_error("Failed to read from bundle file");
    }
}

void Bundle::WriteRaw(int64_t offset, const void* buffer, size_t size) {
    if (m_readOnly)
        throw std::runtime_error("Bundle is read-only (file was locked when opened): " + m_filePath);
    if (m_isSubStream)
        throw std::runtime_error("Bundle is a read-only GGPK sub-stream and cannot be written "
            "(its on-disk custom bundle file is missing, so it resolved from inside Content.ggpk)");
    if (m_isMemoryBased) {
        if (static_cast<size_t>(offset) + size > m_memoryData.size())
            m_memoryData.resize(static_cast<size_t>(offset) + size);
        std::memcpy(m_memoryData.data() + offset, buffer, size);
    } else {
        m_fileStream.seekp(static_cast<std::streamoff>(offset));
        m_fileStream.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(size));
        // A silently failed write (disk full, antivirus, device error) leaves a
        // partial bundle on disk while the index still gets saved afterwards —
        // exactly the index/bundle mismatch that crashes later sessions.
        if (!m_fileStream)
            throw std::runtime_error("Failed to write to bundle file: "
                + (m_filePath.empty() ? std::string("<custom bundle>") : m_filePath)
                + " at offset " + std::to_string(offset) + " (" + std::to_string(size)
                + " bytes): " + DescribeWriteError(m_filePath));
    }
}

void Bundle::TruncateStorage(int64_t newSize) {
    if (m_readOnly)
        throw std::runtime_error("Bundle is read-only (file was locked when opened): " + m_filePath);
    if (m_isMemoryBased) {
        m_memoryData.resize(static_cast<size_t>(newSize));
    } else {
        m_fileStream.flush();
        if (!m_filePath.empty()) {
            m_fileStream.close();
            std::filesystem::resize_file(std::filesystem::path(m_filePath),
                                         static_cast<std::uintmax_t>(newSize));
            m_fileStream.open(std::filesystem::path(m_filePath), std::ios::in | std::ios::out | std::ios::binary);
            if (!m_fileStream.is_open())
                throw std::runtime_error("Failed to reopen bundle file after truncation: "
                                         + m_filePath + " (" + DescribeLastOpenError() + ")");
        }
    }
}

void Bundle::FlushStorage() {
    if (!m_isMemoryBased) {
        m_fileStream.flush();
        if (!m_fileStream)
            throw std::runtime_error("Failed to flush bundle file: "
                + (m_filePath.empty() ? std::string("<custom bundle>") : m_filePath)
                + " (" + DescribeWriteError(m_filePath) + ")");
    }
}

int64_t Bundle::GetChunkDataOffset(int32_t start) const {
    // Header (12 bytes for first 3 ints) + head_size + sum of compressed sizes before start
    int64_t offset = sizeof(int32_t) * 3 + static_cast<int64_t>(m_header.head_size);
    for (int32_t i = 0; i < start; ++i)
        offset += m_compressedChunkSizes[static_cast<size_t>(i)];
    return offset;
}

void Bundle::ReadChunks(uint8_t* output, int32_t start, int32_t end) {
    if (start >= end)
        return;

    Oodle::Settings settings;
    settings.chunkSize = m_header.chunk_size;
    settings.compressor = static_cast<Oodle::Compressor>(m_header.compressor);
    settings.enableCompressing = false;
    Oodle::Initialize(settings);

    // For sub-stream mode, lock the parent mutex for the entire decompression pass.
    // This prevents interleaved seeks from corrupting chunk reads.
    std::unique_lock<std::mutex> streamLock;
    if (m_isSubStream && m_parentMutex)
        streamLock = std::unique_lock<std::mutex>(*m_parentMutex);

    int64_t dataOffset = GetChunkDataOffset(start);
    int32_t compressedBufferSize = Oodle::GetCompressedBufferSize();
    std::vector<uint8_t> compressed(static_cast<size_t>(compressedBufferSize));

    int32_t lastChunk = m_header.chunk_count - 1;
    uint8_t* p = output;

    for (int32_t i = start; i < end; ++i) {
        int32_t compSize = m_compressedChunkSizes[static_cast<size_t>(i)];
        // The chunk size table is untrusted file data — a corrupt entry must
        // not overflow the compressed buffer (heap overwrite) below.
        if (compSize < 0 || compSize > compressedBufferSize)
            throw std::runtime_error("Corrupt bundle: chunk " + std::to_string(i)
                + " compressed size " + std::to_string(compSize)
                + " exceeds buffer size " + std::to_string(compressedBufferSize));
        ReadRawLocked(dataOffset, compressed.data(), static_cast<size_t>(compSize));
        dataOffset += compSize;

        int32_t expectedSize = (i == lastChunk) ? m_header.GetLastChunkSize() : m_header.chunk_size;
        auto result = Oodle::Decompress(compressed.data(), compSize, p, expectedSize);
        if (result != expectedSize)
            throw std::runtime_error("Failed to decompress chunk " + std::to_string(i));

        p += m_header.chunk_size; // Always advance by chunk_size for buffer layout
    }
}

std::vector<uint8_t> Bundle::ReadAll() {
    if (m_header.uncompressed_size == 0)
        return {};

    std::vector<uint8_t> result(static_cast<size_t>(m_header.uncompressed_size));
    ReadChunks(result.data(), 0, m_header.chunk_count);
    return result;
}

std::vector<uint8_t> Bundle::Read(int32_t offset, int32_t length) {
    if (length == 0)
        return {};
    if (offset < 0 || length < 0 || offset + length > m_header.uncompressed_size)
        throw std::out_of_range("Read range out of bounds");

    int32_t startChunk = offset / m_header.chunk_size;
    int32_t endChunk = (offset + length - 1) / m_header.chunk_size + 1;

    // Allocate buffer for all needed chunks
    size_t totalDecompressed;
    if (endChunk == m_header.chunk_count) {
        // Contains the last chunk
        totalDecompressed = static_cast<size_t>(m_header.uncompressed_size - m_header.chunk_size * startChunk);
    } else {
        totalDecompressed = static_cast<size_t>(m_header.chunk_size) * (endChunk - startChunk);
    }
    std::vector<uint8_t> chunkData(totalDecompressed);
    ReadChunks(chunkData.data(), startChunk, endChunk);

    // Extract the requested range
    int32_t offsetInChunk = offset % m_header.chunk_size;
    std::vector<uint8_t> result(static_cast<size_t>(length));
    std::memcpy(result.data(), chunkData.data() + offsetInChunk, static_cast<size_t>(length));
    return result;
}

void Bundle::Save(const uint8_t* newContent, int32_t contentSize, Oodle::CompressionLevel level) {
    Oodle::Settings settings;
    settings.chunkSize = m_header.chunk_size;
    settings.compressor = static_cast<Oodle::Compressor>(m_header.compressor);
    settings.compressionLevel = level;
    settings.enableCompressing = true;
    Oodle::Initialize(settings);

    m_header.uncompressed_size = contentSize;
    m_header.uncompressed_size_long = contentSize;
    m_header.chunk_count = contentSize / m_header.chunk_size;
    if (contentSize > m_header.chunk_count * m_header.chunk_size)
        ++m_header.chunk_count;
    m_header.head_size = m_header.chunk_count * sizeof(int32_t) + (sizeof(BundleHeader) - sizeof(int32_t) * 3);

    // Position after header + chunk sizes
    int64_t dataStart = sizeof(int32_t) * 3 + static_cast<int64_t>(m_header.head_size);
    int64_t writeOffset = dataStart;

    m_compressedChunkSizes.resize(static_cast<size_t>(m_header.chunk_count));
    m_header.compressed_size = 0;

    int32_t compBufSize = Oodle::GetCompressedBufferSize();
    std::vector<uint8_t> compBuf(static_cast<size_t>(compBufSize));

    const uint8_t* p = newContent;
    int32_t lastChunk = m_header.chunk_count - 1;

    for (int32_t i = 0; i < m_header.chunk_count; ++i) {
        int32_t chunkInputSize = (i == lastChunk) ? m_header.GetLastChunkSize() : m_header.chunk_size;
        int32_t compSize = Oodle::Compress(p, chunkInputSize, compBuf.data());
        // Oodle returns the compressed size, or <= 0 on failure. An unchecked
        // negative would become a huge size_t in WriteRaw below and surface as a
        // bogus "disk full" write error (or a heap over-read), so validate here.
        if (compSize <= 0 || compSize > compBufSize)
            throw std::runtime_error("Oodle compression failed for chunk "
                + std::to_string(i) + " (returned " + std::to_string(compSize)
                + ", buffer size " + std::to_string(compBufSize) + ")");

        m_compressedChunkSizes[static_cast<size_t>(i)] = compSize;
        m_header.compressed_size += compSize;

        WriteRaw(writeOffset, compBuf.data(), static_cast<size_t>(compSize));
        writeOffset += compSize;

        p += chunkInputSize;
    }

    m_header.compressed_size_long = m_header.compressed_size;

    // Write header at position 0
    WriteRaw(0, &m_header, sizeof(BundleHeader));
    // Write chunk sizes
    WriteRaw(sizeof(BundleHeader), m_compressedChunkSizes.data(),
             static_cast<size_t>(m_header.chunk_count) * sizeof(int32_t));

    // Truncate to actual size
    TruncateStorage(dataStart + m_header.compressed_size);
    FlushStorage();

    if (m_record)
        m_record->SetUncompressedSize(m_header.uncompressed_size);
}

// BundleRecord::Serialize implementation (here because it needs BundleRecord definition)
void BundleRecord::Serialize(std::vector<uint8_t>& out) const {
    int32_t pathLen = static_cast<int32_t>(m_path.size());
    size_t pos = out.size();
    out.resize(pos + sizeof(int32_t) + static_cast<size_t>(pathLen) + sizeof(int32_t));

    std::memcpy(out.data() + pos, &pathLen, sizeof(int32_t));
    pos += sizeof(int32_t);

    std::memcpy(out.data() + pos, m_path.data(), static_cast<size_t>(pathLen));
    pos += static_cast<size_t>(pathLen);

    std::memcpy(out.data() + pos, &m_uncompressedSize, sizeof(int32_t));
}

// FileRecord::Serialize implementation
void FileRecord::Serialize(std::vector<uint8_t>& out) const {
    size_t pos = out.size();
    out.resize(pos + RECORD_LENGTH);

    std::memcpy(out.data() + pos, &m_pathHash, sizeof(uint64_t));
    pos += sizeof(uint64_t);

    int32_t bundleIdx = m_bundleRecord->GetBundleIndex();
    std::memcpy(out.data() + pos, &bundleIdx, sizeof(int32_t));
    pos += sizeof(int32_t);

    std::memcpy(out.data() + pos, &m_offset, sizeof(int32_t));
    pos += sizeof(int32_t);

    std::memcpy(out.data() + pos, &m_size, sizeof(int32_t));
}

} // namespace GGPK
