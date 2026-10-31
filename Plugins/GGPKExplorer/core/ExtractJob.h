// Plugins/GGPKExplorer/core/ExtractJob.h
// The extraction engine, without UI: files are read through the index's batch reader
// (Index::ReadFiles: grouped by bundle, each run of neighbouring files decoded once, a few
// workers), optionally converted on a small pool (Save As: DDS -> PNG, DAT -> JSON/CSV), and
// written by a few writer threads (creating files is the cost now) fed through one
// byte-bounded queue, so memory stays bounded whatever the selection: a file's bytes count
// against the queue's bound until they are written (or converted and passed on), so the
// files the writers hold are inside it too. Every output folder is created once (the
// writers share one guarded set). Two items naming the same output file (case-insensitively)
// are never written at once; the file ends up holding one of them, which one is not defined
// (with several writers not necessarily the later item).
// Output paths are wide (UTF-8 archive paths converted with CP_UTF8, never the ANSI code
// page). A file that cannot be read, converted or written is counted and reported; it never
// stops the job. No exception leaves a thread of the job.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace GGPK {
class Index;
class FileRecord;
}

namespace GGPKExp {

class ExtractJob {
public:
    /// One file: its record and its archive path ('/'-separated UTF-8). The output is
    /// <outputDir>\<path without its first `strip` bytes>. The record and the path string
    /// (a tree node's or the record's own path) must outlive the job.
    struct Item {
        GGPK::FileRecord* record = nullptr;
        const std::string* path = nullptr;
        uint32_t strip = 0;
    };

    enum class Outcome { Write, Skip, Fail };

    /// Turns a file's bytes into the bytes to write (in place); `why` says why on Skip/Fail.
    /// Runs on the convert pool, several calls at once: it must be thread-safe. An
    /// exception counts as Fail.
    using Converter = std::function<Outcome(const Item& item, std::vector<uint8_t>& bytes, std::string& why)>;

    struct Options {
        std::wstring outputDir;          // the folder the archive paths are written under
        std::wstring singleOutput;       // not empty: the (one) item is written to this file
        std::wstring replaceExtension;   // not empty (L".png"): the output name's extension
        int64_t skipLargerThan = -1;     // >= 0: larger files are counted as skipped
        unsigned readWorkers = 4;        // Index::ReadFiles workers (whole bundles each)
        unsigned convertWorkers = 1;     // threads of the converter (when there is one)
        unsigned writeWorkers = 0;       // writer threads; 0 = DefaultWriteWorkers() (1 for singleOutput)
        size_t queueBytes = 256u * 1024 * 1024;   // bytes one queue and its consumers hold at most
        Converter convert;               // empty: the bytes are written as read
    };

    struct Summary {
        size_t total = 0;
        size_t written = 0;     // a zero-byte file written as an empty file counts here
        size_t failed = 0;      // not read, not converted, or not written
        size_t skipped = 0;     // larger than skipLargerThan, or the converter skipped it
        bool cancelled = false;
        double seconds = 0.0;
        std::vector<std::string> sampleErrors;   // the first few "<path>: <why>"
        unsigned writers = 0;                    // writer threads the job ran
        size_t foldersCreated = 0;               // output folders this job made (each once)
        size_t folderAttempts = 0;               // CreateDirectoryW calls (2 for a folder whose parent was missing)
        std::string fatalError;                  // the job itself stopped (never one file)
    };

    using FinishedFn = std::function<void(const Summary&)>;

    ExtractJob() = default;
    ~ExtractJob();
    ExtractJob(const ExtractJob&) = delete;
    ExtractJob& operator=(const ExtractJob&) = delete;

    /// Starts the job on its own thread; false while one is still running (or when no
    /// thread could be started). `index` and every item must outlive the job: cancel and
    /// join it (CancelAndJoin) before the archive closes. `onFinished` runs on the job's
    /// thread once it has ended, cancelled or not, after the read cache was released.
    bool Start(GGPK::Index& index, std::vector<Item> items, Options options, FinishedFn onFinished);
    void Cancel() { m_stop.store(true); }
    void CancelAndJoin();
    bool IsRunning() const { return m_running.load(); }
    size_t Done() const { return m_done.load(); }
    size_t Total() const { return m_total.load(); }

    /// Worker counts for this machine: reads (bounded memory: each holds one run of at most
    /// 16 MiB decoded), conversions (CPU-bound) and writes (min(4, hardware threads): NTFS
    /// file creation, about 150 us a file, is what bounds an extraction).
    static unsigned DefaultReadWorkers();
    static unsigned DefaultConvertWorkers();
    static unsigned DefaultWriteWorkers();

    /// The whole job on the calling thread (Start runs it; tests and benchmarks call it).
    /// Setting `stop` cancels it; `done` counts finished files. Releases the index's read
    /// cache at the end.
    static Summary Run(GGPK::Index& index, const std::vector<Item>& items, const Options& options,
                       std::atomic<bool>& stop, std::atomic<size_t>& done);

private:
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_running{false};
    std::atomic<size_t> m_done{0};
    std::atomic<size_t> m_total{0};
};

/// Writes `size` bytes to `path` (wide; created or truncated). A failed write removes the
/// partial file. False with `error` (English, for the log) on failure.
bool WriteWholeFile(const std::wstring& path, const uint8_t* data, size_t size, std::string& error);

/// `utf8RelativePath` ('/'-separated) under `base`, '\' separated; a name Windows would not
/// keep as one file name in that folder is refused (".", "..", ':' or '\', control
/// characters, invalid UTF-8) with `why`. Empty segments are dropped (as Windows does).
/// `replaceExtension` (when not empty) replaces the file name's extension. Long results get
/// the \\?\ form (`base` must be absolute; ExtractJob makes it so).
bool BuildOutputPath(const std::wstring& base, const std::string& utf8RelativePath,
                     const std::wstring& replaceExtension, std::wstring& out, std::string& why);

} // namespace GGPKExp
