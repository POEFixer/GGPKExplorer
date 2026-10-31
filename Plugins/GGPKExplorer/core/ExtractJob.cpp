// Plugins/GGPKExplorer/core/ExtractJob.cpp
#include "ExtractJob.h"
#include "Index.h"
#include "FileRecord.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_set>

namespace GGPKExp {

namespace {

constexpr size_t kMaxSampleErrors = 8;

std::string Win32Error(DWORD code) {
    // English when the system has it (the log is English), else the user's language.
    wchar_t* text = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD n = FormatMessageW(flags, nullptr, code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                             reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    if (n == 0) {
        if (text) LocalFree(text);
        text = nullptr;
        n = FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    }
    std::string message = "Win32 error " + std::to_string(code);
    if (n > 0 && text) {
        std::wstring wide(text, n);
        while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' || wide.back() == L' ' || wide.back() == L'.'))
            wide.pop_back();
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                              nullptr, 0, nullptr, nullptr);
        if (bytes > 0) {
            std::string utf8(static_cast<size_t>(bytes), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), bytes,
                                nullptr, nullptr);
            message += ": " + utf8;
        }
    }
    if (text) LocalFree(text);
    return message;
}

// The \\?\ form for a path too long for the plain Win32 form (CreateDirectoryW stops at
// MAX_PATH - 12). `path` is absolute with '\' separators and no "." / ".." segments.
std::wstring LongPathForm(std::wstring path) {
    if (path.size() < MAX_PATH - 12 || path.rfind(L"\\\\?\\", 0) == 0)
        return path;
    if (path.rfind(L"\\\\", 0) == 0)
        return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

// The absolute, normalized form of a folder or file chosen in a dialog (the base of every
// output path, so the \\?\ form is valid for it).
std::wstring AbsolutePath(const std::wstring& path) {
    if (path.empty()) return path;
    const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return path;
    std::wstring full(needed, L'\0');
    const DWORD got = GetFullPathNameW(path.c_str(), needed, full.data(), nullptr);
    if (got == 0 || got >= needed) return path;
    full.resize(got);
    return full;
}

bool IsDirectory(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// The output folders of one job, each made (or found) once. Shared by the writer threads:
// guarded, so a folder two writers need is made by one while the other waits and then finds
// it made. The base folder (chosen in a dialog) is made first in its plain form (if it went
// missing); then it counts as existing in both forms, so the folders below it are made below
// it and a long (\\?\) path never climbs to its drive root.
class DirectoryMaker {
public:
    explicit DirectoryMaker(const std::wstring& base) {
        std::wstring b = base;
        while (b.size() > 1 && (b.back() == L'\\' || b.back() == L'/')) b.pop_back();
        if (!b.empty() && Make(b, 0)) {
            m_known.insert(b);
            m_known.insert(LongForm(b));
        }
    }

    bool Ensure(const std::wstring& dir, std::string& why) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (dir.empty() || m_known.count(dir)) return true;
        if (!Make(dir, 0)) {
            why = "cannot create the folder (" + Win32Error(m_lastError) + ")";
            return false;
        }
        return true;
    }

    /// Folders this job made, and CreateDirectoryW calls (a folder whose parent was
    /// missing takes two): the summary's foldersCreated / folderAttempts.
    void Counts(size_t& created, size_t& attempts) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        created = m_created;
        attempts = m_attempts;
    }

private:
    // `path` in the \\?\ form whatever its length (how a long output path names its base).
    static std::wstring LongForm(const std::wstring& path) {
        if (path.rfind(L"\\\\?\\", 0) == 0) return path;
        if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
        return L"\\\\?\\" + path;
    }

    // Caller holds m_mutex (or is the constructor) and found `dir` unknown.
    bool Make(const std::wstring& dir, int depth) {
        ++m_attempts;
        if (CreateDirectoryW(dir.c_str(), nullptr)) {
            ++m_created;
            m_known.insert(dir);
            return true;
        }
        DWORD error = GetLastError();
        if (error == ERROR_PATH_NOT_FOUND && depth < 512) {
            const size_t slash = dir.find_last_of(L'\\');
            if (slash == std::wstring::npos || slash == 0) {
                m_lastError = error;
                return false;
            }
            const std::wstring parent = dir.substr(0, slash);
            if (!m_known.count(parent) && !Make(parent, depth + 1))   // the base stops the climb
                return false;   // m_lastError: the parent's reason
            ++m_attempts;
            if (CreateDirectoryW(dir.c_str(), nullptr)) {
                ++m_created;
                m_known.insert(dir);
                return true;
            }
            error = GetLastError();
        }
        if (IsDirectory(dir)) {   // already there (ERROR_ALREADY_EXISTS), or a root
            m_known.insert(dir);
            return true;
        }
        m_lastError = error;
        return false;
    }

    mutable std::mutex m_mutex;
    std::unordered_set<std::wstring> m_known;
    DWORD m_lastError = 0;
    size_t m_created = 0;
    size_t m_attempts = 0;
};

// Output files a writer is writing now, by case-folded path: a second item naming the same
// file waits until the first is written (never two handles on one file, never one writer
// removing the other's partial file). The job's writers write such items one after the other.
class PathLocks {
public:
    // false: the job stops first (the item is dropped, as a stopping writer drops it).
    bool Acquire(const std::wstring& key, const std::atomic<bool>& stop) {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_held.count(key)) {
            if (stop.load()) return false;
            m_released.wait_for(lock, std::chrono::milliseconds(20));
        }
        m_held.insert(key);
        return true;
    }

    void Release(const std::wstring& key) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_held.erase(key);
        }
        m_released.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_released;
    std::unordered_set<std::wstring> m_held;
};

// The case-insensitive identity of an output path (NTFS compares names case-insensitively).
std::wstring PathKey(const std::wstring& path) {
    std::wstring key = path;
    if (!key.empty()) {
        const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, path.c_str(), static_cast<int>(path.size()),
                                    key.data(), static_cast<int>(key.size()), nullptr, nullptr, 0);
        if (n != static_cast<int>(key.size()))
            key = path;   // could not fold: the exact path still serializes exact duplicates
    }
    return key;
}

// A FIFO of (item index, bytes) bounding the bytes of its items from Push until the consumer
// is done with each one (Release, after writing or converting it): the queued items and the
// ones its consumers hold together stay within `capacity`, whatever the number of consumers.
// One larger item is let in when the queue holds nothing, so a big file never blocks it.
// Every wait also ends on `stop`.
class ByteQueue {
public:
    ByteQueue(size_t capacity, const std::atomic<bool>& stop) : m_capacity(capacity), m_stop(stop) {}

    // false: the job stops (the item is dropped).
    bool Push(size_t index, std::vector<uint8_t>&& bytes) {
        std::unique_lock<std::mutex> lock(m_mutex);
        const size_t size = bytes.size();
        while (!m_aborted && m_bytes > 0 && m_bytes + size > m_capacity) {
            if (m_stop.load()) return false;
            m_notFull.wait_for(lock, std::chrono::milliseconds(20));
        }
        if (m_aborted || m_stop.load()) return false;
        m_bytes += size;
        m_items.push_back(Entry{ index, std::move(bytes) });
        m_notEmpty.notify_one();
        return true;
    }

    // false: closed and empty, or the job stops. The item's bytes stay counted until the
    // caller Releases them (its bytes.size() as popped).
    bool Pop(size_t& index, std::vector<uint8_t>& bytes) {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (!m_aborted && !m_closed && m_items.empty()) {
            if (m_stop.load()) return false;
            m_notEmpty.wait_for(lock, std::chrono::milliseconds(20));
        }
        if (m_aborted || m_items.empty()) return false;
        Entry& front = m_items.front();
        index = front.index;
        bytes = std::move(front.bytes);
        m_items.pop_front();
        return true;
    }

    // The consumer is done with a popped item of `size` bytes (written, or converted and
    // passed on, and its memory given back).
    void Release(size_t size) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_aborted) return;   // Abort dropped every count
            m_bytes -= (std::min)(m_bytes, size);
        }
        m_notFull.notify_all();
    }

    // No more pushes: Pop drains what is queued, then returns false.
    void Close() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        m_notEmpty.notify_all();
    }

    // Drop everything; every waiter returns false.
    void Abort() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_aborted = true;
        m_items.clear();
        m_bytes = 0;
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

private:
    struct Entry {
        size_t index;
        std::vector<uint8_t> bytes;
    };
    std::mutex m_mutex;
    std::condition_variable m_notEmpty, m_notFull;
    std::deque<Entry> m_items;
    size_t m_bytes = 0;
    const size_t m_capacity;
    const std::atomic<bool>& m_stop;
    bool m_closed = false;
    bool m_aborted = false;
};

// What every thread of one Run shares.
struct RunState {
    const std::vector<ExtractJob::Item>& items;
    const ExtractJob::Options& options;
    std::wstring base;                  // absolute output folder
    std::wstring single;                // absolute single output file
    std::atomic<bool>& stop;
    std::atomic<size_t>& done;
    std::atomic<size_t> written{0}, failed{0}, skipped{0};
    std::mutex errorMutex;
    std::vector<std::string> sampleErrors;
    std::string fatalError;

    RunState(const std::vector<ExtractJob::Item>& i, const ExtractJob::Options& o,
             std::atomic<bool>& s, std::atomic<size_t>& d)
        : items(i), options(o), stop(s), done(d) {}

    std::string PathOf(size_t index) const {
        const ExtractJob::Item& item = items[index];
        return item.path ? *item.path : std::string("<no path>");
    }

    void Fail(size_t index, const std::string& why) {
        failed.fetch_add(1);
        done.fetch_add(1);
        try {
            std::lock_guard<std::mutex> lock(errorMutex);
            if (sampleErrors.size() < kMaxSampleErrors)
                sampleErrors.push_back(PathOf(index) + ": " + why);
        } catch (...) {}
    }

    void Skip() {
        skipped.fetch_add(1);
        done.fetch_add(1);
    }

    void Fatal(const std::string& why) {
        try {
            std::lock_guard<std::mutex> lock(errorMutex);
            if (fatalError.empty()) fatalError = why;
        } catch (...) {}
        stop.store(true);
    }

    // The output file of item `index`.
    bool OutputPath(size_t index, std::wstring& out, std::string& why) const {
        if (!single.empty()) {
            out = LongPathForm(single);
            return true;
        }
        const ExtractJob::Item& item = items[index];
        if (!item.path) {
            why = "no archive path";
            return false;
        }
        const std::string& path = *item.path;
        const std::string rel = item.strip <= path.size() ? path.substr(item.strip) : path;
        return BuildOutputPath(base, rel, options.replaceExtension, out, why);
    }
};

// One of the job's writer threads (they share the queue, the folders and the path locks).
void WriterLoop(RunState& state, ByteQueue& queue, DirectoryMaker& dirs, PathLocks& paths) {
    try {
        size_t index = 0;
        std::vector<uint8_t> bytes;
        while (queue.Pop(index, bytes)) {
            if (state.stop.load()) break;
            const size_t held = bytes.size();   // counted in the queue's bound until written
            try {
                std::wstring out;
                std::string why;
                bool ok = state.OutputPath(index, out, why);
                if (ok) {
                    const size_t slash = out.find_last_of(L'\\');
                    if (slash != std::wstring::npos)
                        ok = dirs.Ensure(out.substr(0, slash), why);
                }
                if (ok) {
                    const std::wstring key = PathKey(out);
                    if (!paths.Acquire(key, state.stop))
                        break;   // stopping: dropped like the rest of the queue
                    try {
                        ok = WriteWholeFile(out, bytes.data(), bytes.size(), why);
                    } catch (...) {
                        paths.Release(key);
                        throw;
                    }
                    paths.Release(key);
                }
                if (ok) {
                    state.written.fetch_add(1);
                    state.done.fetch_add(1);
                } else {
                    state.Fail(index, why);
                }
            } catch (const std::exception& ex) {
                state.Fail(index, ex.what());
            } catch (...) {
                state.Fail(index, "unknown error");
            }
            bytes = std::vector<uint8_t>();   // give the memory back before waiting
            queue.Release(held);
        }
    } catch (const std::exception& ex) {
        state.Fatal(std::string("writer: ") + ex.what());
    } catch (...) {
        state.Fatal("writer: unknown error");
    }
}

void ConvertLoop(RunState& state, ByteQueue& in, ByteQueue& out) {
    try {
        size_t index = 0;
        std::vector<uint8_t> bytes;
        while (in.Pop(index, bytes)) {
            if (state.stop.load()) break;
            const size_t held = bytes.size();   // counted in `in` until passed on to `out`
            ExtractJob::Outcome outcome = ExtractJob::Outcome::Fail;
            std::string why;
            try {
                outcome = state.options.convert(state.items[index], bytes, why);
            } catch (const std::exception& ex) {
                outcome = ExtractJob::Outcome::Fail;
                why = ex.what();
            } catch (...) {
                outcome = ExtractJob::Outcome::Fail;
                why = "unknown error";
            }
            switch (outcome) {
            case ExtractJob::Outcome::Write:
                out.Push(index, std::move(bytes));   // false: stopping, the file is dropped
                break;
            case ExtractJob::Outcome::Skip:
                state.Skip();
                break;
            case ExtractJob::Outcome::Fail:
                state.Fail(index, why.empty() ? std::string("conversion failed") : why);
                break;
            }
            bytes = std::vector<uint8_t>();
            in.Release(held);
        }
    } catch (const std::exception& ex) {
        state.Fatal(std::string("converter: ") + ex.what());
    } catch (...) {
        state.Fatal("converter: unknown error");
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Output paths and files
// ---------------------------------------------------------------------------

bool BuildOutputPath(const std::wstring& base, const std::string& utf8RelativePath,
                     const std::wstring& replaceExtension, std::wstring& out, std::string& why) {
    std::wstring rel;
    if (!utf8RelativePath.empty()) {
        const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8RelativePath.data(),
                                          static_cast<int>(utf8RelativePath.size()), nullptr, 0);
        if (n <= 0) {
            why = "the file name is not valid UTF-8";
            return false;
        }
        rel.resize(static_cast<size_t>(n));
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8RelativePath.data(),
                            static_cast<int>(utf8RelativePath.size()), rel.data(), n);
    }

    out = base;
    if (!out.empty() && out.back() != L'\\' && out.back() != L'/')
        out.push_back(L'\\');
    const size_t baseLength = out.size();

    size_t start = 0;
    std::wstring segment;
    bool any = false;
    while (start <= rel.size()) {
        size_t end = rel.find(L'/', start);
        if (end == std::wstring::npos) end = rel.size();
        segment.assign(rel, start, end - start);
        start = end + 1;
        if (segment.empty()) {
            if (end == rel.size()) break;
            continue;
        }
        if (segment == L"." || segment == L"..") {
            why = "the archive path has a \".\" or \"..\" folder";
            return false;
        }
        for (wchar_t c : segment) {
            if (c < 0x20 || c == L':' || c == L'\\') {
                why = "the file name has a character Windows does not allow";
                return false;
            }
        }
        if (any) out.push_back(L'\\');
        const bool last = end >= rel.size();
        if (last && !replaceExtension.empty()) {
            std::filesystem::path name(segment);
            name.replace_extension(replaceExtension);
            out += name.native();
        } else {
            out += segment;
        }
        any = true;
        if (last) break;
    }
    if (!any || out.size() == baseLength) {
        why = "empty archive path";
        return false;
    }
    out = LongPathForm(std::move(out));
    return true;
}

bool WriteWholeFile(const std::wstring& path, const uint8_t* data, size_t size, std::string& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "cannot create the file (" + Win32Error(GetLastError()) + ")";
        return false;
    }
    size_t offset = 0;
    while (offset < size) {
        const DWORD chunk = static_cast<DWORD>((std::min)(size - offset, static_cast<size_t>(1) << 30));
        DWORD wrote = 0;
        if (!::WriteFile(file, data + offset, chunk, &wrote, nullptr) || wrote != chunk) {
            const DWORD code = GetLastError();
            CloseHandle(file);
            DeleteFileW(path.c_str());
            error = "write failed (" + Win32Error(code) + ")";
            return false;
        }
        offset += chunk;
    }
    if (!CloseHandle(file)) {
        const DWORD code = GetLastError();
        DeleteFileW(path.c_str());
        error = "write failed (" + Win32Error(code) + ")";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Run: read (batch) -> [convert pool] -> writer threads
// ---------------------------------------------------------------------------

ExtractJob::Summary ExtractJob::Run(GGPK::Index& index, const std::vector<Item>& items, const Options& options,
                                    std::atomic<bool>& stop, std::atomic<size_t>& done) {
    const auto started = std::chrono::steady_clock::now();
    Summary summary;
    summary.total = items.size();

    RunState state(items, options, stop, done);
    state.base = AbsolutePath(options.outputDir);
    state.single = AbsolutePath(options.singleOutput);
    if (state.base.empty() && state.single.empty()) {
        // Never a path relative to the current folder.
        summary.fatalError = "no output folder";
        return summary;
    }

    const bool converting = static_cast<bool>(options.convert);
    ByteQueue writeQueue(options.queueBytes, stop);
    ByteQueue convertQueue(options.queueBytes, stop);

    std::vector<std::thread> writers;
    std::vector<std::thread> converters;
    auto stopThreads = [&](bool drain) {
        if (drain) {
            convertQueue.Close();
            for (auto& t : converters) if (t.joinable()) t.join();
            writeQueue.Close();
            for (auto& t : writers) if (t.joinable()) t.join();
        } else {
            convertQueue.Abort();
            writeQueue.Abort();
            for (auto& t : converters) if (t.joinable()) t.join();
            for (auto& t : writers) if (t.joinable()) t.join();
        }
    };

    std::unique_ptr<DirectoryMaker> dirs;
    PathLocks paths;
    try {
        dirs = std::make_unique<DirectoryMaker>(state.base);
        unsigned writerCount = options.writeWorkers ? options.writeWorkers : DefaultWriteWorkers();
        if (!state.single.empty()) writerCount = 1;   // one output file
        writerCount = (std::max)(writerCount, 1u);
        for (unsigned i = 0; i < writerCount; ++i) {
            try {
                writers.emplace_back(WriterLoop, std::ref(state), std::ref(writeQueue), std::ref(*dirs), std::ref(paths));
            } catch (...) {
                if (writers.empty()) throw;   // not even one writer
                break;                        // fewer writers than asked: the started ones do the work
            }
        }
        summary.writers = static_cast<unsigned>(writers.size());
        if (converting) {
            const unsigned count = (std::max)(options.convertWorkers, 1u);
            for (unsigned i = 0; i < count; ++i)
                converters.emplace_back(ConvertLoop, std::ref(state), std::ref(convertQueue), std::ref(writeQueue));
        }

        std::vector<GGPK::FileRecord*> records;
        records.reserve(items.size());
        for (const Item& item : items) records.push_back(item.record);

        ByteQueue& firstQueue = converting ? convertQueue : writeQueue;
        GGPK::ReadFilesOptions readOptions;
        readOptions.workers = (std::max)(options.readWorkers, 1u);
        index.ReadFiles(records,
            [&](size_t position, GGPK::FileRecord* /*record*/, std::vector<uint8_t>&& bytes, const std::string* error) {
                if (error) {
                    state.Fail(position, *error);
                    return;
                }
                if (options.skipLargerThan >= 0 && static_cast<int64_t>(bytes.size()) > options.skipLargerThan) {
                    state.Skip();
                    return;
                }
                firstQueue.Push(position, std::move(bytes));   // false: stopping, the file is dropped
            },
            &stop, readOptions);

        stopThreads(!stop.load());
    } catch (const std::exception& ex) {
        state.Fatal(ex.what());
        stopThreads(false);
    } catch (...) {
        state.Fatal("unknown error");
        stopThreads(false);
    }

    try {
        index.TrimReadCache();
    } catch (...) {}

    if (dirs)
        dirs->Counts(summary.foldersCreated, summary.folderAttempts);
    summary.written = state.written.load();
    summary.failed = state.failed.load();
    summary.skipped = state.skipped.load();
    summary.sampleErrors = std::move(state.sampleErrors);
    summary.fatalError = std::move(state.fatalError);
    summary.cancelled = stop.load() && summary.fatalError.empty();
    summary.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return summary;
}

// ---------------------------------------------------------------------------
// Start / CancelAndJoin
// ---------------------------------------------------------------------------

unsigned ExtractJob::DefaultReadWorkers() {
    const unsigned hw = std::thread::hardware_concurrency();
    return (std::clamp)(hw == 0 ? 2u : hw, 1u, 4u);
}

unsigned ExtractJob::DefaultConvertWorkers() {
    const unsigned hw = std::thread::hardware_concurrency();
    return (std::clamp)(hw == 0 ? 2u : hw / 2, 1u, 4u);
}

unsigned ExtractJob::DefaultWriteWorkers() {
    const unsigned hw = std::thread::hardware_concurrency();
    return (std::clamp)(hw == 0 ? 2u : hw, 1u, 4u);
}

ExtractJob::~ExtractJob() {
    CancelAndJoin();
}

bool ExtractJob::Start(GGPK::Index& index, std::vector<Item> items, Options options, FinishedFn onFinished) {
    if (m_running.load()) return false;
    if (m_thread.joinable()) m_thread.join();   // a finished job not joined yet

    m_stop.store(false);
    m_done.store(0);
    m_total.store(items.size());
    m_running.store(true);
    try {
        m_thread = std::thread([this, &index, items = std::move(items), options = std::move(options),
                                onFinished = std::move(onFinished)]() {
            Summary summary;
            try {
                summary = Run(index, items, options, m_stop, m_done);
            } catch (const std::exception& ex) {
                summary.total = items.size();
                summary.fatalError = ex.what();
            } catch (...) {
                summary.total = items.size();
                summary.fatalError = "unknown error";
            }
            try {
                if (onFinished) onFinished(summary);
            } catch (...) {}
            m_running.store(false);
        });
    } catch (...) {
        m_running.store(false);
        return false;
    }
    return true;
}

void ExtractJob::CancelAndJoin() {
    m_stop.store(true);
    if (m_thread.joinable()) m_thread.join();
}

} // namespace GGPKExp
