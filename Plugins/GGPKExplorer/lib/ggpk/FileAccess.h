#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace GGPK {

/// How the library opens game files.
///  - ReadWrite (default, unchanged behaviour): read-write first, read-only fallback when the
///    read-write open is refused (the running game, a read-only attribute). Writes then fail
///    with a clear error; reads work.
///  - ReadOnly: never asks for write access. Index / BundledGGPK opened this way refuse
///    FileRecord::Write and Save with a clear error.
enum class OpenMode { ReadWrite, ReadOnly };

/// Every path that crosses the library API is a UTF-8 narrow string. These convert it with
/// CP_UTF8 (never the ANSI code page). Utf8Path throws std::runtime_error on invalid UTF-8.
std::filesystem::path Utf8Path(const std::string& utf8);
std::string PathToUtf8(const std::filesystem::path& path);

/// Human-readable reason for a Win32 error code (with the library's hints for a sharing
/// violation and a missing file).
std::string DescribeWin32Error(unsigned long error);

/// A file opened for reading only: GENERIC_READ with FILE_SHARE_READ | FILE_SHARE_WRITE |
/// FILE_SHARE_DELETE, so it never blocks the game or its patcher (which may write, rename or
/// delete the file while it is open). Reads are positional (OVERLAPPED offsets on an
/// overlapped handle with a per-thread event): no seek state, safe from any number of threads.
class ReadOnlyFile {
public:
    ReadOnlyFile() = default;
    ~ReadOnlyFile();
    ReadOnlyFile(ReadOnlyFile&& other) noexcept;
    ReadOnlyFile& operator=(ReadOnlyFile&& other) noexcept;
    ReadOnlyFile(const ReadOnlyFile&) = delete;
    ReadOnlyFile& operator=(const ReadOnlyFile&) = delete;

    /// Opens `utf8Path`; throws std::runtime_error("<what>: <path> (<reason>)") on failure.
    static ReadOnlyFile Open(const std::string& utf8Path, const char* what = "Failed to open file");

    bool IsOpen() const { return m_handle != nullptr; }
    /// File size when it was opened.
    int64_t Size() const { return m_size; }

    /// True while the file this handle reads still has the size and last-write time it had
    /// when it was opened and still has a name (not deleted). One GetFileInformationByHandle
    /// on the handle (about 2 us); false when it fails or the file is closed.
    bool Unchanged() const;

    /// Reads up to `size` bytes at `offset`; returns the count (short only at the end of the
    /// file). Throws std::runtime_error on an I/O error.
    size_t ReadAt(int64_t offset, void* buffer, size_t size) const;

    /// Reads exactly `size` bytes or throws std::runtime_error(what).
    void ReadExact(int64_t offset, void* buffer, size_t size, const std::string& what) const;

    void Close();

private:
    void* m_handle = nullptr;   // HANDLE; nullptr when closed
    int64_t m_size = 0;
    int64_t m_lastWrite = 0;    // FILETIME ticks when it was opened
};

} // namespace GGPK
