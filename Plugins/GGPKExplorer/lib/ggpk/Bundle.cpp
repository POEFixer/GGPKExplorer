#include "Bundle.h"
#include "BundleRecord.h"
#include "FileRecord.h"
#include "GGPK.h"
#include "GGPKHost.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>

namespace GGPK {

// ============================================================================
// Paths and Win32 read-only files
// ============================================================================

std::filesystem::path Utf8Path(const std::string& utf8) {
    if (utf8.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                      static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0)
        throw std::runtime_error("Path is not valid UTF-8: " + utf8);
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), n);
    return std::filesystem::path(std::move(wide));
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::wstring& wide = path.native();
    if (wide.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n,
                            nullptr, nullptr);
    return out;
}

std::string DescribeWin32Error(unsigned long error) {
    std::string msg;
    wchar_t* text = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                                   reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    if (n > 0 && text) {
        msg = PathToUtf8(std::filesystem::path(std::wstring(text, n)));
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' ' || msg.back() == '.'))
            msg.pop_back();
    }
    if (text)
        LocalFree(text);
    msg = (msg.empty() ? std::string("Win32 error") : msg) + " (" + std::to_string(error) + ")";
    if (error == ERROR_SHARING_VIOLATION)
        msg += "; file is locked by another process - close the game and try again";
    else if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        msg += "; file is missing - verify/repair the game files and try again";
    return msg;
}

namespace {

// One manual-reset event per thread for overlapped reads (ReadFile resets it when the
// operation starts; a thread has at most one read in flight).
struct ThreadEvent {
    HANDLE handle = nullptr;
    ~ThreadEvent() {
        if (handle)
            CloseHandle(handle);
    }
};

HANDLE ReadEvent() {
    thread_local ThreadEvent event;
    if (!event.handle) {
        event.handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event.handle)
            throw std::runtime_error("CreateEvent failed (" + DescribeWin32Error(GetLastError()) + ")");
    }
    return event.handle;
}

} // namespace

ReadOnlyFile::~ReadOnlyFile() { Close(); }

ReadOnlyFile::ReadOnlyFile(ReadOnlyFile&& other) noexcept
    : m_handle(other.m_handle), m_size(other.m_size), m_lastWrite(other.m_lastWrite) {
    other.m_handle = nullptr;
    other.m_size = 0;
    other.m_lastWrite = 0;
}

ReadOnlyFile& ReadOnlyFile::operator=(ReadOnlyFile&& other) noexcept {
    if (this != &other) {
        Close();
        m_handle = other.m_handle;
        m_size = other.m_size;
        m_lastWrite = other.m_lastWrite;
        other.m_handle = nullptr;
        other.m_size = 0;
        other.m_lastWrite = 0;
    }
    return *this;
}

namespace {

// Size, last-write time and whether the file still has a name, from the handle itself (one
// call: about 2 us on the overlapped handle, two GetFileInformationByHandleEx cost more).
bool QueryIdentity(HANDLE h, int64_t& size, int64_t& lastWrite, bool& named) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info))
        return false;
    size = static_cast<int64_t>((static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow);
    lastWrite = static_cast<int64_t>((static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32)
                                     | info.ftLastWriteTime.dwLowDateTime);
    named = info.nNumberOfLinks > 0;   // 0 once deleted (POSIX delete: the name is free again)
    return true;
}

} // namespace

bool ReadOnlyFile::Unchanged() const {
    if (!m_handle)
        return false;
    int64_t size = 0, lastWrite = 0;
    bool named = false;
    return QueryIdentity(static_cast<HANDLE>(m_handle), size, lastWrite, named)
        && named && size == m_size && lastWrite == m_lastWrite;
}

ReadOnlyFile ReadOnlyFile::Open(const std::string& utf8Path, const char* what) {
    std::filesystem::path wide;
    try {
        wide = Utf8Path(utf8Path);
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string(what) + ": " + utf8Path + " (" + ex.what() + ")");
    }
    const HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        throw std::runtime_error(std::string(what) + ": " + utf8Path + " (" + DescribeWin32Error(error) + ")");
    }
    // Taken before anything is read, so a change made after this point is always seen by
    // Unchanged().
    int64_t size = 0, lastWrite = 0;
    bool named = false;
    if (!QueryIdentity(h, size, lastWrite, named)) {
        const DWORD error = GetLastError();
        CloseHandle(h);
        throw std::runtime_error(std::string(what) + ": " + utf8Path + " (" + DescribeWin32Error(error) + ")");
    }
    ReadOnlyFile file;
    file.m_handle = h;
    file.m_size = size;
    file.m_lastWrite = lastWrite;
    return file;
}

size_t ReadOnlyFile::ReadAt(int64_t offset, void* buffer, size_t size) const {
    if (!m_handle)
        throw std::runtime_error("Read from a closed file");
    if (offset < 0)
        throw std::runtime_error("Read at a negative offset");
    auto* out = static_cast<uint8_t*>(buffer);
    size_t total = 0;
    while (total < size) {
        const DWORD want = static_cast<DWORD>((std::min)(size - total, static_cast<size_t>(1u << 30)));
        const int64_t at = offset + static_cast<int64_t>(total);
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(static_cast<uint64_t>(at) & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(at) >> 32);
        ov.hEvent = ReadEvent();
        DWORD got = 0;
        BOOL ok = ReadFile(static_cast<HANDLE>(m_handle), out + total, want, nullptr, &ov);
        DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        if (ok || error == ERROR_IO_PENDING) {
            ok = GetOverlappedResult(static_cast<HANDLE>(m_handle), &ov, &got, TRUE);
            error = ok ? ERROR_SUCCESS : GetLastError();
        }
        if (!ok) {
            if (error == ERROR_HANDLE_EOF)
                break;
            throw std::runtime_error("Read error at offset " + std::to_string(at) + " ("
                                     + DescribeWin32Error(error) + ")");
        }
        if (got == 0)
            break;
        total += got;
    }
    return total;
}

void ReadOnlyFile::ReadExact(int64_t offset, void* buffer, size_t size, const std::string& what) const {
    if (ReadAt(offset, buffer, size) != size)
        throw std::runtime_error(what);
}

void ReadOnlyFile::Close() {
    if (m_handle) {
        CloseHandle(static_cast<HANDLE>(m_handle));
        m_handle = nullptr;
    }
    m_size = 0;
    m_lastWrite = 0;
}

// ============================================================================
// Error descriptions
// ============================================================================

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
        std::filesystem::path target;
        try {
            target = Utf8Path(path).parent_path();
        } catch (...) {
            ec = std::make_error_code(std::errc::invalid_argument);
        }
        const auto info = ec ? std::filesystem::space_info{} : std::filesystem::space(target, ec);
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

// ============================================================================
// Bundle: construction and storage
// ============================================================================

namespace {

// Bundle headers and chunk tables come straight from disk and are fully
// untrusted: a truncated/corrupt file (or a stale custom bundle left behind by
// an interrupted save) must fail with a clean error here — if these values
// leak into size arithmetic they turn into wild memmoves (crash_506).
void ValidateBundleHeader(const BundleHeader& h, const std::string& source) {
    const int64_t cs = h.chunk_size;
    const int64_t cc = h.chunk_count;
    const int64_t u  = h.uncompressed_size;
    const bool ok = cs > 0 && cs <= kMaxBundleChunkSize && cc >= 0 && u >= 0 &&
                    cc * cs >= u &&
                    (cc == 0 ? u == 0 : (cc - 1) * cs < u);
    if (!ok)
        throw std::runtime_error("Corrupt bundle header in " + source
            + " (uncompressed_size=" + std::to_string(h.uncompressed_size)
            + ", chunk_count=" + std::to_string(h.chunk_count)
            + ", chunk_size=" + std::to_string(h.chunk_size) + ")");
}

/// The chunk size table must lie inside the storage before it sizes any buffer
/// (a corrupt count would otherwise allocate up to 8 GB).
void ValidateTableFits(const BundleHeader& h, int64_t storageSize, const std::string& what) {
    const int64_t tableEnd = static_cast<int64_t>(sizeof(BundleHeader))
                           + static_cast<int64_t>(h.chunk_count) * static_cast<int64_t>(sizeof(int32_t));
    if (storageSize >= 0 && tableEnd > storageSize)
        throw std::runtime_error(what);
}

} // namespace

Bundle::Bundle(const std::string& filePath, BundleRecord* record, OpenMode mode)
    : m_record(record)
    , m_filePath(filePath) {
    if (mode == OpenMode::ReadOnly) {
        // Readers never ask for write access: the handle shares read/write/delete, so the
        // running game and its patcher are never blocked by it.
        m_readFile = ReadOnlyFile::Open(filePath, "Failed to open bundle file");
        m_readOnly = true;
        LoadHeaderAndTable(m_readFile.Size(), filePath);
        return;
    }

    // Read-write first: custom bundles (LibGGPK3/*) and _.index.bin need write
    // access for Save(). Regular game bundles are only ever read during patching.
    m_fileStream.open(Utf8Path(filePath), std::ios::in | std::ios::out | std::ios::binary);
    if (!m_fileStream.is_open()) {
        // A running game (or antivirus) holds bundle files with deny-write
        // sharing, and read-only attributes also reject RW opens. Reading does
        // not need write access, so fall back to read-only before failing.
        m_readOnlyReason = DescribeLastOpenError();
        m_fileStream.clear();
        m_fileStream.open(Utf8Path(filePath), std::ios::in | std::ios::binary);
        if (!m_fileStream.is_open())
            throw std::runtime_error("Failed to open bundle file: " + filePath
                                     + " (" + DescribeLastOpenError() + ")");
        m_readOnly = true;
    }

    m_fileStream.seekg(0, std::ios::end);
    const int64_t fileSize = static_cast<int64_t>(m_fileStream.tellg());
    m_fileStream.seekg(0, std::ios::beg);

    // Read header
    m_fileStream.read(reinterpret_cast<char*>(&m_header), sizeof(BundleHeader));
    if (m_fileStream.gcount() != sizeof(BundleHeader))
        throw std::runtime_error("Failed to read bundle header from: " + filePath);

    ValidateBundleHeader(m_header, filePath);
    ValidateTableFits(m_header, fileSize, "Truncated bundle (chunk size table): " + filePath);

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
    BuildChunkOffsets();
}

Bundle::Bundle(std::vector<uint8_t> data, BundleRecord* record)
    : m_record(record)
    , m_memoryData(std::move(data))
    , m_isMemoryBased(true) {
    if (m_memoryData.size() < sizeof(BundleHeader))
        throw std::runtime_error("Bundle data too small for header");

    std::memcpy(&m_header, m_memoryData.data(), sizeof(BundleHeader));

    ValidateBundleHeader(m_header, "memory bundle");
    ValidateTableFits(m_header, static_cast<int64_t>(m_memoryData.size()),
                      "Bundle data too small for chunk size table");

    if (record)
        record->SetUncompressedSize(m_header.uncompressed_size);

    m_compressedChunkSizes.resize(static_cast<size_t>(m_header.chunk_count));
    if (m_header.chunk_count > 0) {
        const size_t tableBytes = static_cast<size_t>(m_header.chunk_count) * sizeof(int32_t);
        std::memcpy(m_compressedChunkSizes.data(),
                     m_memoryData.data() + sizeof(BundleHeader),
                     tableBytes);
    }
    BuildChunkOffsets();
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
    // On disk at once: from its creation the file is a valid empty bundle, never 0 bytes. A
    // record may name it with nothing else ever written into it (a zero-size
    // FileRecord::Write leaves the write buffer empty, so Save never rewrites it) and is
    // read from it before and after Save.
    FlushStorage();
    [[maybe_unused]] auto pos = m_fileStream.tellp();
    // Truncate by closing and reopening is not ideal, but we'll just track the logical end
    BuildChunkOffsets();
}

Bundle::Bundle(const GGPKFile& parent, int64_t baseOffset, int64_t totalSize, BundleRecord* record)
    : m_record(record)
    , m_parent(&parent)
    , m_parentBaseOffset(baseOffset)
    , m_subSize(totalSize < 0 ? 0 : totalSize)
    , m_isSubStream(true) {
    m_readOnly = true;
    if (m_subSize < static_cast<int64_t>(sizeof(BundleHeader)))
        throw std::runtime_error("Truncated bundle header in GGPK sub-stream");
    LoadHeaderAndTable(m_subSize, "GGPK sub-stream");
}

void Bundle::LoadHeaderAndTable(int64_t storageSize, const std::string& source) {
    const bool sub = m_isSubStream;
    try {
        ReadRaw(0, &m_header, sizeof(BundleHeader));
    } catch (const std::exception&) {
        throw std::runtime_error(sub ? std::string("Truncated bundle header in GGPK sub-stream")
                                     : "Failed to read bundle header from: " + source);
    }
    ValidateBundleHeader(m_header, source);
    const std::string truncated = sub ? std::string("Truncated bundle (chunk size table) in GGPK sub-stream")
                                      : "Truncated bundle (chunk size table): " + source;
    ValidateTableFits(m_header, storageSize, truncated);

    if (m_record)
        m_record->SetUncompressedSize(m_header.uncompressed_size);

    m_compressedChunkSizes.resize(static_cast<size_t>(m_header.chunk_count));
    if (m_header.chunk_count > 0) {
        try {
            ReadRaw(sizeof(BundleHeader), m_compressedChunkSizes.data(),
                    static_cast<size_t>(m_header.chunk_count) * sizeof(int32_t));
        } catch (const std::exception&) {
            throw std::runtime_error(truncated);
        }
    }
    BuildChunkOffsets();
}

void Bundle::BuildChunkOffsets() {
    // Header (12 bytes for first 3 ints) + head_size + sum of compressed sizes before i.
    // int64: the table is untrusted; a negative entry is refused when its chunk is read.
    const size_t count = m_compressedChunkSizes.size();
    m_chunkOffsets.resize(count + 1);
    int64_t offset = static_cast<int64_t>(sizeof(int32_t) * 3) + static_cast<int64_t>(m_header.head_size);
    for (size_t i = 0; i < count; ++i) {
        m_chunkOffsets[i] = offset;
        offset += m_compressedChunkSizes[i];
    }
    m_chunkOffsets[count] = offset;
}

void Bundle::CloseStorage() {
    m_readFile.Close();
    if (m_readOnly && m_fileStream.is_open())
        m_fileStream.close();   // a read-only fallback stream is never written
}

void Bundle::ReadRaw(int64_t offset, void* buffer, size_t size) {
    if (offset < 0)
        throw std::runtime_error("Read at a negative offset in bundle "
                                 + (m_filePath.empty() ? std::string("<memory>") : m_filePath));
    if (m_isMemoryBased) {
        if (static_cast<uint64_t>(offset) + size > m_memoryData.size())
            throw std::runtime_error("Read past end of memory bundle");
        std::memcpy(buffer, m_memoryData.data() + offset, size);
    } else if (m_isSubStream) {
        // Never outside this bundle's region of Content.ggpk (a corrupt table must not
        // read the neighbouring records).
        if (offset + static_cast<int64_t>(size) > m_subSize)
            throw std::runtime_error("Read past the end of the bundle in GGPK sub-stream");
        m_parent->ReadExact(m_parentBaseOffset + offset, buffer, size, "Failed to read from GGPK sub-stream");
    } else if (m_readFile.IsOpen()) {
        m_readFile.ReadExact(offset, buffer, size, "Failed to read from bundle file");
    } else {
        if (!m_fileStream.is_open())
            throw std::runtime_error("Bundle storage is closed: " + m_filePath);
        m_fileStream.clear();   // a failed earlier read must not poison this one
        m_fileStream.seekg(static_cast<std::streamoff>(offset));
        m_fileStream.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
        if (m_fileStream.gcount() != static_cast<std::streamsize>(size))
            throw std::runtime_error("Failed to read from bundle file");
    }
}

void Bundle::WriteRaw(int64_t offset, const void* buffer, size_t size) {
    if (m_readOnly && !m_isSubStream)
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
            std::filesystem::resize_file(Utf8Path(m_filePath),
                                         static_cast<std::uintmax_t>(newSize));
            m_fileStream.open(Utf8Path(m_filePath), std::ios::in | std::ios::out | std::ios::binary);
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

// ============================================================================
// Bundle: decoding
// ============================================================================

namespace {

constexpr size_t kCompressedPad = 64;                         // slack after compressed bytes
constexpr int64_t kWindowCompressedBytes = 16ll * 1024 * 1024; // one compressed read at most
constexpr size_t kThreadBufferCap = 2u * 1024 * 1024;          // per-thread buffers kept up to this
constexpr int32_t kParallelMinChunks = 8;                      // parallel decode from 2 MiB (256 KiB chunks)
constexpr unsigned kMaxDecodeThreads = 8;
constexpr int32_t kParallelSaveMinChunks = 4;                  // parallel compression from 4 chunks
constexpr unsigned kMaxSaveThreads = 8;

/// Buffers reused by every read on a thread (no allocation or zero-fill per small read).
struct ThreadBuffers {
    std::vector<uint8_t> compressed;
    std::vector<uint8_t> bounce;
};

ThreadBuffers& LocalBuffers() {
    thread_local ThreadBuffers buffers;
    return buffers;
}

/// A scratch area of `size` bytes: the thread's own buffer when small, else `spill`.
uint8_t* Scratch(std::vector<uint8_t>& threadBuffer, std::vector<uint8_t>& spill, size_t size) {
    std::vector<uint8_t>& target = size <= kThreadBufferCap ? threadBuffer : spill;
    if (target.size() < size)
        target.resize(size);
    return target.data();
}

/// Bounds of a read, in int64 (offset + length must not wrap around int32).
void CheckReadRange(int32_t offset, int32_t length, int32_t size) {
    if (offset < 0 || length < 0 || static_cast<int64_t>(offset) + length > static_cast<int64_t>(size))
        throw std::out_of_range("Read range out of bounds");
}

void InitDecoder(const BundleHeader& h) {
    Oodle::Settings settings;
    settings.chunkSize = h.chunk_size;
    settings.compressor = static_cast<Oodle::Compressor>(h.compressor);
    settings.enableCompressing = false;
    Oodle::Initialize(settings);
}

unsigned DecodeThreads(int32_t chunks) {
    const unsigned hw = std::thread::hardware_concurrency();
    unsigned n = hw == 0 ? 2u : hw;
    n = (std::min)(n, kMaxDecodeThreads);
    n = (std::min)(n, static_cast<unsigned>((chunks + 1) / 2));
    return n < 1 ? 1u : n;
}

unsigned SaveThreads(int32_t chunks) {
    const unsigned hw = std::thread::hardware_concurrency();
    unsigned n = hw == 0 ? 2u : hw;
    n = (std::min)(n, kMaxSaveThreads);
    n = (std::min)(n, static_cast<unsigned>(chunks));
    return n < 1 ? 1u : n;
}

/// Runs fn(k) for k in [0, count) on up to `threads` threads (the caller included).
/// Every thread runs `init` first. The exception of the lowest failing k is rethrown.
template <class Init, class Fn>
void ParallelFor(int32_t count, unsigned threads, Init&& init, Fn&& fn) {
    std::atomic<int32_t> next{0};
    std::atomic<bool> stop{false};
    std::mutex failMutex;
    int32_t failIndex = count;
    std::exception_ptr failure;
    auto body = [&]() {
        try {
            init();
        } catch (...) {
            std::lock_guard<std::mutex> lock(failMutex);
            if (!failure) { failure = std::current_exception(); failIndex = -1; }
            stop = true;
            return;
        }
        for (;;) {
            if (stop.load(std::memory_order_relaxed))
                return;
            const int32_t k = next.fetch_add(1);
            if (k >= count)
                return;
            try {
                fn(k);
            } catch (...) {
                std::lock_guard<std::mutex> lock(failMutex);
                if (k < failIndex) { failIndex = k; failure = std::current_exception(); }
                stop = true;
            }
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(threads > 0 ? threads - 1 : 0);
    try {
        for (unsigned t = 1; t < threads; ++t)
            pool.emplace_back(body);
    } catch (...) {
        // Could not start more threads: the caller still does all the work.
    }
    body();
    for (auto& th : pool)
        th.join();
    if (failure)
        std::rethrow_exception(failure);
}

} // namespace

void Bundle::DecodeChunks(int32_t first, int32_t end, uint8_t* out, bool allowParallel) {
    if (first < 0 || end > m_header.chunk_count || first > end)
        throw std::out_of_range("Chunk range out of bounds");
    if (first == end)
        return;

    Host::SetCrashStep("ggpk:oodle_decompress");
    InitDecoder(m_header);

    // The chunk size table is untrusted file data — a corrupt entry must not
    // overflow the compressed buffer (heap overwrite) below.
    const int32_t compressedBufferSize = Oodle::GetCompressedBufferSize();
    for (int32_t i = first; i < end; ++i) {
        const int32_t compSize = m_compressedChunkSizes[static_cast<size_t>(i)];
        if (compSize < 0 || compSize > compressedBufferSize)
            throw std::runtime_error("Corrupt bundle: chunk " + std::to_string(i)
                + " compressed size " + std::to_string(compSize)
                + " exceeds buffer size " + std::to_string(compressedBufferSize));
    }

    const int64_t stride = m_header.chunk_size;
    std::vector<uint8_t> spill;
    int32_t i = first;
    while (i < end) {
        // A window of consecutive chunks: one contiguous read of their compressed bytes.
        int32_t j = i;
        int64_t bytes = 0;
        while (j < end && (j == i || bytes + m_compressedChunkSizes[static_cast<size_t>(j)] <= kWindowCompressedBytes)) {
            bytes += m_compressedChunkSizes[static_cast<size_t>(j)];
            ++j;
        }
        uint8_t* compressed = Scratch(LocalBuffers().compressed, spill, static_cast<size_t>(bytes) + kCompressedPad);
        ReadRaw(GetChunkDataOffset(i), compressed, static_cast<size_t>(bytes));

        const int32_t windowFirst = i;
        auto decodeOne = [&](int32_t k) {
            const int32_t chunk = windowFirst + k;
            const int64_t src = GetChunkDataOffset(chunk) - GetChunkDataOffset(windowFirst);
            const int32_t expectedSize = GetChunkDecodedSize(chunk);
            uint8_t* dest = out + static_cast<int64_t>(chunk - first) * stride;
            const auto result = Oodle::Decompress(compressed + src,
                m_compressedChunkSizes[static_cast<size_t>(chunk)], dest, expectedSize);
            if (result != expectedSize)
                throw std::runtime_error("Failed to decompress chunk " + std::to_string(chunk));
        };
        const int32_t count = j - i;
        if (allowParallel && count >= kParallelMinChunks) {
            const BundleHeader header = m_header;
            ParallelFor(count, DecodeThreads(count), [&header] { InitDecoder(header); }, decodeOne);
        } else {
            for (int32_t k = 0; k < count; ++k)
                decodeOne(k);
        }
        i = j;
    }
}

void Bundle::ReadInto(int32_t offset, int32_t length, uint8_t* out, bool allowParallel) {
    CheckReadRange(offset, length, m_header.uncompressed_size);
    if (length == 0)
        return;

    const int64_t cs = m_header.chunk_size;
    const int64_t begin = offset;
    const int64_t end = static_cast<int64_t>(offset) + length;
    const int32_t c0 = static_cast<int32_t>(begin / cs);
    const int32_t c1 = static_cast<int32_t>((end - 1) / cs) + 1;
    auto chunkStart = [&](int32_t c) { return static_cast<int64_t>(c) * cs; };
    auto chunkEnd = [&](int32_t c) {
        return (std::min)(static_cast<int64_t>(c + 1) * cs, static_cast<int64_t>(m_header.uncompressed_size));
    };
    // Chunks wholly inside the range decode straight into `out`.
    const int32_t f0 = chunkStart(c0) == begin ? c0 : c0 + 1;
    const int32_t f1 = chunkEnd(c1 - 1) == end ? c1 : c1 - 1;

    std::vector<uint8_t> spill;
    if (f0 >= f1) {
        // Only partial chunks (one, or two straddled): one read, one copy.
        const size_t span = static_cast<size_t>(chunkEnd(c1 - 1) - chunkStart(c0));
        uint8_t* bounce = Scratch(LocalBuffers().bounce, spill, span);
        DecodeChunks(c0, c1, bounce, false);
        std::memcpy(out, bounce + (begin - chunkStart(c0)), static_cast<size_t>(length));
        return;
    }
    if (c0 < f0) {
        uint8_t* bounce = Scratch(LocalBuffers().bounce, spill, static_cast<size_t>(cs));
        DecodeChunks(c0, c0 + 1, bounce, false);
        std::memcpy(out, bounce + (begin - chunkStart(c0)), static_cast<size_t>(chunkEnd(c0) - begin));
    }
    DecodeChunks(f0, f1, out + (chunkStart(f0) - begin), allowParallel);
    if (f1 < c1) {
        uint8_t* bounce = Scratch(LocalBuffers().bounce, spill, static_cast<size_t>(cs));
        DecodeChunks(c1 - 1, c1, bounce, false);
        std::memcpy(out + (chunkStart(c1 - 1) - begin), bounce, static_cast<size_t>(end - chunkStart(c1 - 1)));
    }
}

std::vector<uint8_t> Bundle::ReadAll() {
    if (m_header.uncompressed_size == 0)
        return {};

    std::vector<uint8_t> result(static_cast<size_t>(m_header.uncompressed_size));
    DecodeChunks(0, m_header.chunk_count, result.data(), true);
    return result;
}

std::vector<uint8_t> Bundle::Read(int32_t offset, int32_t length) {
    if (length == 0)
        return {};
    CheckReadRange(offset, length, m_header.uncompressed_size);

    const int64_t cs = m_header.chunk_size;
    const int32_t c0 = static_cast<int32_t>(offset / cs);
    const int32_t c1 = static_cast<int32_t>((static_cast<int64_t>(offset) + length - 1) / cs) + 1;
    if (c1 - c0 <= 2) {
        // Small read: decode into the thread's buffer and copy the range out once (no
        // zero-filled result).
        const int64_t spanEnd = (std::min)(static_cast<int64_t>(c1) * cs, static_cast<int64_t>(m_header.uncompressed_size));
        const size_t span = static_cast<size_t>(spanEnd - static_cast<int64_t>(c0) * cs);
        std::vector<uint8_t> spill;
        uint8_t* bounce = Scratch(LocalBuffers().bounce, spill, span);
        DecodeChunks(c0, c1, bounce, false);
        const uint8_t* from = bounce + (offset - static_cast<int64_t>(c0) * cs);
        return std::vector<uint8_t>(from, from + length);
    }
    std::vector<uint8_t> result(static_cast<size_t>(length));
    ReadInto(offset, length, result.data(), true);
    return result;
}

// ============================================================================
// Bundle: saving
// ============================================================================

void Bundle::Save(const uint8_t* newContent, int32_t contentSize, Oodle::CompressionLevel level) {
    // Refuse before compressing anything: these bundles can never be written.
    if (m_isSubStream)
        throw std::runtime_error("Bundle is a read-only GGPK sub-stream and cannot be written "
            "(its on-disk custom bundle file is missing, so it resolved from inside Content.ggpk)");
    if (m_readOnly)
        throw std::runtime_error("Bundle is read-only (file was locked when opened): " + m_filePath);

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
    int32_t lastChunk = m_header.chunk_count - 1;
    const int32_t count = m_header.chunk_count;

    // Chunks are independent: compress a window of them in parallel (Oodle state is per
    // thread), then validate and write them strictly in order. The bytes written, their
    // offsets and the failure point are exactly the serial loop's (Oodle is deterministic;
    // gate: tools/ggpk_bundle_test save_layout).
    const unsigned threads = count >= kParallelSaveMinChunks ? SaveThreads(count) : 1u;
    const int32_t window = static_cast<int32_t>(threads) * 4;
    std::vector<std::vector<uint8_t>> slots(static_cast<size_t>((std::min)(window, (std::max)(count, 1))));
    for (auto& slot : slots)
        slot.resize(static_cast<size_t>(compBufSize));
    std::vector<int32_t> results(slots.size());

    for (int32_t base = 0; base < count; base += window) {
        const int32_t n = (std::min)(window, count - base);
        auto compressOne = [&](int32_t k) {
            const int32_t i = base + k;
            const int32_t chunkInputSize = (i == lastChunk) ? m_header.GetLastChunkSize() : m_header.chunk_size;
            results[static_cast<size_t>(k)] = Oodle::Compress(
                newContent + static_cast<int64_t>(i) * m_header.chunk_size, chunkInputSize,
                slots[static_cast<size_t>(k)].data());
        };
        if (threads > 1 && n > 1)
            ParallelFor(n, (std::min)(threads, static_cast<unsigned>(n)), [&settings] { Oodle::Initialize(settings); }, compressOne);
        else
            for (int32_t k = 0; k < n; ++k)
                compressOne(k);

        for (int32_t k = 0; k < n; ++k) {
            const int32_t i = base + k;
            const int32_t compSize = results[static_cast<size_t>(k)];
            // Oodle returns the compressed size, or <= 0 on failure. An unchecked
            // negative would become a huge size_t in WriteRaw below and surface as a
            // bogus "disk full" write error (or a heap over-read), so validate here.
            if (compSize <= 0 || compSize > compBufSize)
                throw std::runtime_error("Oodle compression failed for chunk "
                    + std::to_string(i) + " (returned " + std::to_string(compSize)
                    + ", buffer size " + std::to_string(compBufSize) + ")");

            m_compressedChunkSizes[static_cast<size_t>(i)] = compSize;
            m_header.compressed_size += compSize;

            WriteRaw(writeOffset, slots[static_cast<size_t>(k)].data(), static_cast<size_t>(compSize));
            writeOffset += compSize;
        }
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
    BuildChunkOffsets();

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

    int32_t bundleIdx = GetBundleRecord()->GetBundleIndex();
    std::memcpy(out.data() + pos, &bundleIdx, sizeof(int32_t));
    pos += sizeof(int32_t);

    std::memcpy(out.data() + pos, &m_offset, sizeof(int32_t));
    pos += sizeof(int32_t);

    std::memcpy(out.data() + pos, &m_size, sizeof(int32_t));
}

} // namespace GGPK
