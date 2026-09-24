#include "Index.h"
#include "DriveBundleFactory.h"
#include "TreeNode.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <Windows.h>

// Lightweight logging shim: routes to debugger output instead of POEFixer Logger
namespace {
    inline void GGPKLog(const std::string& msg) {
        OutputDebugStringA(("[GGPKExplorer] " + msg + "\n").c_str());
    }
}

namespace GGPK {

Index::Index(const std::string& filePath, bool parsePaths, std::unique_ptr<IBundleFactory> factory) {
    if (!factory) {
        auto parentDir = std::filesystem::path(filePath).parent_path().string();
        factory = std::make_unique<DriveBundleFactory>(parentDir);
    }
    m_factory = std::move(factory);

    // Open _.index.bin as a bundle
    GGPKLog("[GGPK] Opening index: " + filePath);
    m_baseBundle = std::make_unique<Bundle>(filePath);

    // Decompress the index data
    auto data = m_baseBundle->ReadAll();
    GGPKLog("[GGPK] Index decompressed: " + std::to_string(data.size()) + " bytes");
    ParseIndexData(data);

    GGPKLog("[GGPK] Parsed: " + std::to_string(m_bundles.size()) + " bundles, "
        + std::to_string(m_files.size()) + " files, "
        + std::to_string(m_directories.size()) + " directories"
        + ", hash mode: " + std::string(m_useMurmurHash ? "MurmurHash64A" : "FNV1a64"));

    if (parsePaths) {
        int failed = ParsePaths();
        if (failed != 0) {
            GGPKLog("[GGPK] ParsePaths: " + std::to_string(failed) + " file(s) failed path resolution (orphaned hash entries, safe to ignore)");
        }
    }
}

Index::Index(std::vector<uint8_t> bundleData, bool parsePaths, std::unique_ptr<IBundleFactory> factory)
    : m_factory(std::move(factory)) {

    m_baseBundle = std::make_unique<Bundle>(std::move(bundleData));

    auto data = m_baseBundle->ReadAll();
    GGPKLog("[GGPK] Index decompressed (from memory): " + std::to_string(data.size()) + " bytes");
    ParseIndexData(data);

    GGPKLog("[GGPK] Parsed: " + std::to_string(m_bundles.size()) + " bundles, "
        + std::to_string(m_files.size()) + " files, "
        + std::to_string(m_directories.size()) + " directories");

    if (parsePaths) {
        int failed = ParsePaths();
        if (failed != 0) {
            GGPKLog("[GGPK] ParsePaths: " + std::to_string(failed) + " file(s) failed path resolution (orphaned hash entries, safe to ignore)");
        }
    }
}

Index::~Index() {
    // Clean up any pending writes
    if (m_bundleToWrite) {
        m_bundleToWriteOwned.reset();
        m_bundleToWrite = nullptr;
    }
}

void Index::ParseIndexData(const std::vector<uint8_t>& data) {
    const uint8_t* p = data.data();
    const uint8_t* end = data.data() + data.size();

    auto readInt = [&]() -> int32_t {
        if (p + sizeof(int32_t) > end) throw std::runtime_error("Index data truncated");
        int32_t val;
        std::memcpy(&val, p, sizeof(int32_t));
        p += sizeof(int32_t);
        return val;
    };

    // Read bundles
    int32_t bundleCount = readInt();
    m_bundles.reserve(static_cast<size_t>(bundleCount));

    for (int32_t i = 0; i < bundleCount; ++i) {
        int32_t pathLength = readInt();
        if (p + pathLength > end) throw std::runtime_error("Index data truncated (bundle path)");
        std::string path(reinterpret_cast<const char*>(p), static_cast<size_t>(pathLength));
        p += pathLength;
        int32_t uncompressedSize = readInt();

        auto br = std::make_unique<BundleRecord>(path, uncompressedSize, this, i);
        if (path.find(CUSTOM_BUNDLE_BASE_PATH) == 0)
            m_customBundles.push_back(br.get());
        m_bundles.push_back(std::move(br));
    }

    // Read files
    int32_t fileCount = readInt();
    m_files.reserve(static_cast<size_t>(fileCount));

    for (int32_t i = 0; i < fileCount; ++i) {
        if (p + sizeof(uint64_t) + sizeof(int32_t) * 3 > end)
            throw std::runtime_error("Index data truncated (file record)");

        uint64_t nameHash;
        std::memcpy(&nameHash, p, sizeof(uint64_t));
        p += sizeof(uint64_t);

        int32_t bundleIndex = readInt();
        int32_t offset = readInt();
        int32_t size = readInt();

        if (bundleIndex < 0 || bundleIndex >= static_cast<int32_t>(m_bundles.size()))
            throw std::runtime_error("Invalid bundle index in file record");

        auto* bundle = m_bundles[static_cast<size_t>(bundleIndex)].get();
        auto fr = std::make_unique<FileRecord>(nameHash, bundle, offset, size);
        bundle->AddFile(fr.get());
        auto* frPtr = fr.get();
        m_files.emplace(nameHash, std::move(fr));
        (void)frPtr;
    }

    // Read directories
    int32_t directoryCount = readInt();
    m_directories.resize(static_cast<size_t>(directoryCount));
    size_t dirBytes = static_cast<size_t>(directoryCount) * sizeof(IndexDirectoryRecord);
    if (p + dirBytes > end) throw std::runtime_error("Index data truncated (directories)");
    std::memcpy(m_directories.data(), p, dirBytes);
    p += dirBytes;

    // Remaining data is the directory bundle data
    size_t remaining = static_cast<size_t>(end - p);
    m_directoryBundleData.assign(p, p + remaining);

    // Detect hash algorithm
    if (!m_directories.empty())
        m_useMurmurHash = (m_directories[0].pathHash == NameHash::MURMUR_EMPTY_HASH);
}

int Index::ParsePaths() {
    if (m_pathsParsed)
        return 0;

    // Decompress the directory bundle data (it's a bundle within the index)
    auto dirBundleData = m_directoryBundleData; // copy
    Bundle dirBundle{std::move(dirBundleData)};
    auto directory = dirBundle.ReadAll();

    int failed = 0;
    int skippedDirs = 0;
    const uint8_t* base = directory.data();
    const int64_t dirDataSize = static_cast<int64_t>(directory.size());

    for (const auto& d : m_directories) {
        // Directory records were memcpy'd raw from the index file — validate
        // before forming pointers (corrupt offsets would read wild memory).
        if (d.offset < 0 || d.size < 4 ||
            static_cast<int64_t>(d.offset) + d.size > dirDataSize) {
            ++skippedDirs;
            continue;
        }

        std::vector<std::vector<uint8_t>> temp;
        bool baseMode = false;
        const uint8_t* offset = base + d.offset;
        const uint8_t* ptr = offset;
        const uint8_t* dirEnd = offset + d.size;

        while (ptr - offset <= d.size - 4) {
            if (ptr + 4 > dirEnd) break;

            int32_t index;
            std::memcpy(&index, ptr, sizeof(int32_t));
            ptr += sizeof(int32_t);

            if (index == 0) {
                baseMode = !baseMode;
                if (baseMode)
                    temp.clear();
            } else {
                index -= 1;

                // Read null-terminated string
                const uint8_t* strStart = ptr;
                while (ptr < base + directory.size() && *ptr != 0)
                    ++ptr;
                size_t strLen = static_cast<size_t>(ptr - strStart);
                if (ptr < base + directory.size()) ++ptr; // skip null terminator

                if (index >= 0 && index < static_cast<int32_t>(temp.size())) {
                    // Concatenate prefix + current string
                    std::vector<uint8_t> combined = temp[static_cast<size_t>(index)];
                    combined.insert(combined.end(), strStart, strStart + strLen);

                    if (baseMode) {
                        temp.push_back(std::move(combined));
                    } else {
                        // File path - look up by hash
                        uint64_t hash;
                        if (m_useMurmurHash) {
                            // Lowercase for MurmurHash
                            std::vector<uint8_t> lower(combined.size());
                            std::transform(combined.begin(), combined.end(), lower.begin(),
                                           [](uint8_t c) { return static_cast<uint8_t>(std::tolower(c)); });
                            hash = NameHash::MurmurHash64A(lower.data(), lower.size());
                        } else {
                            hash = NameHash::FNV1a64Hash(combined.data(), combined.size());
                        }

                        auto it = m_files.find(hash);
                        if (it != m_files.end()) {
                            it->second->SetPath(std::string(
                                reinterpret_cast<const char*>(combined.data()), combined.size()));
                        } else {
                            ++failed;
                        }
                    }
                } else {
                    // No prefix, just the string itself
                    if (baseMode) {
                        temp.emplace_back(strStart, strStart + strLen);
                    } else {
                        uint64_t hash;
                        if (m_useMurmurHash) {
                            std::vector<uint8_t> lower(strLen);
                            std::transform(strStart, strStart + strLen, lower.begin(),
                                           [](uint8_t c) { return static_cast<uint8_t>(std::tolower(c)); });
                            hash = NameHash::MurmurHash64A(lower.data(), lower.size());
                        } else {
                            hash = NameHash::FNV1a64Hash(strStart, strLen);
                        }

                        auto it = m_files.find(hash);
                        if (it != m_files.end()) {
                            it->second->SetPath(std::string(
                                reinterpret_cast<const char*>(strStart), strLen));
                        } else {
                            ++failed;
                        }
                    }
                }
            }
        }
    }

    if (skippedDirs != 0)
        GGPKLog("[GGPK] ParsePaths: skipped " + std::to_string(skippedDirs)
            + " directory record(s) with out-of-range offsets (corrupt index data)");

    m_pathsParsed = true;
    return failed;
}

std::unique_ptr<DirectoryNode> Index::BuildTree(bool ignoreNullPath) {
    if (!ignoreNullPath && !m_pathsParsed)
        throw std::runtime_error("ParsePaths() must be called before BuildTree()");

    auto root = std::make_unique<DirectoryNode>("");

    // Collect and sort files by path for deterministic tree building
    std::vector<FileRecord*> sortedFiles;
    sortedFiles.reserve(m_files.size());
    for (auto& [hash, fr] : m_files) {
        if (fr->GetPath().empty()) {
            if (!ignoreNullPath)
                throw std::runtime_error("File has empty path - Index may be broken");
            continue;
        }
        sortedFiles.push_back(fr.get());
    }
    std::sort(sortedFiles.begin(), sortedFiles.end(),
              [](const FileRecord* a, const FileRecord* b) { return a->GetPath() < b->GetPath(); });

    for (auto* fr : sortedFiles) {
        const auto& path = fr->GetPath();

        // Split path by '/'
        DirectoryNode* parent = root.get();
        size_t start = 0;
        size_t lastSlash = path.rfind('/');

        if (lastSlash != std::string::npos) {
            // Navigate/create directory hierarchy
            size_t pos = 0;
            while (pos < lastSlash) {
                size_t nextSlash = path.find('/', pos);
                if (nextSlash == std::string::npos || nextSlash > lastSlash)
                    nextSlash = lastSlash;
                std::string dirName = path.substr(pos, nextSlash - pos);
                parent = parent->GetOrCreateChildDir(dirName);
                pos = nextSlash + 1;
            }
            start = lastSlash + 1;
        }

        // Add file node
        std::string fileName = path.substr(start);
        parent->AddChild(std::make_unique<FileNode>(fileName, fr, parent));
    }

    return root;
}

FileRecord* Index::TryGetFile(const std::string& path) {
    uint64_t hash = ComputeNameHash(path);
    auto it = m_files.find(hash);
    return (it != m_files.end()) ? it->second.get() : nullptr;
}

uint64_t Index::ComputeNameHash(const std::string& path) const {
    return NameHash::ComputeHash(path, m_useMurmurHash);
}

Bundle* Index::GetBundleToWrite(int32_t& originalSize) {
    originalSize = 0;

    // Check existing custom bundles for one with space
    for (auto* cb : m_customBundles) {
        // Calculate current size from files. int64: index records are untrusted
        // disk data — corrupt offsets must not overflow into a bogus small size.
        int64_t maxEnd = 0;
        for (auto* f : cb->GetFiles()) {
            int64_t end = static_cast<int64_t>(f->GetOffset()) + f->GetSize();
            if (end > maxEnd) maxEnd = end;
        }

        if (maxEnd >= maxBundleSize)
            continue;

        auto bundle = cb->TryGetBundle();
        // Skip any custom bundle we can't actually write to, and create a fresh
        // on-disk one instead. Two cases: a read-only fallback open (file locked by
        // the running game), and a GGPK sub-stream — when the custom bundle's
        // on-disk file is missing, the factory resolves it read-only from inside
        // Content.ggpk; writing to that stream fails with a cryptic badbit and no
        // OS error (the "<custom bundle> ... disk full or file locked?" report).
        if (!bundle || !bundle->IsWritable())
            continue;

        // The index can claim more file data in this bundle than the file on
        // disk actually contains (stale state after an interrupted previous
        // save: bundle rewritten but index not, or vice versa). Seeding the
        // write buffer from such a bundle reads past the end of its content
        // (crash_506) — skip it; files re-patched this session are redirected
        // to a fresh bundle, healing the index as they are written.
        if (maxEnd > bundle->GetUncompressedSize()) {
            GGPKLog("[GGPK] Custom bundle " + cb->GetPath() + " holds "
                + std::to_string(bundle->GetUncompressedSize())
                + " bytes but the index expects " + std::to_string(maxEnd)
                + " — skipping it for writes (stale bundle from an interrupted save)");
            continue;
        }

        originalSize = static_cast<int32_t>(maxEnd);
        m_bundleToWriteOwned = std::move(bundle);
        m_bundleToWrite = m_bundleToWriteOwned.get();
        return m_bundleToWrite;
    }

    // Create a new custom bundle
    std::string path = std::string(CUSTOM_BUNDLE_BASE_PATH) + std::to_string(m_customBundles.size());

    // Check for name collision
    for (auto* cb : m_customBundles) {
        if (cb->GetPathBase() == path) {
            for (int i = 0; ; ++i) {
                path = std::string(CUSTOM_BUNDLE_BASE_PATH) + std::to_string(i);
                bool exists = false;
                for (auto* cb2 : m_customBundles) {
                    if (cb2->GetPathBase() == path) { exists = true; break; }
                }
                if (!exists) break;
            }
            break;
        }
    }

    auto* bundle = CreateBundle(path);
    m_customBundles.push_back(bundle->GetRecord());
    originalSize = 0;
    return bundle;
}

Bundle* Index::CreateBundle(const std::string& bundlePath) {
    auto br = std::make_unique<BundleRecord>(bundlePath, 0, this, static_cast<int32_t>(m_bundles.size()));
    std::string fullPath;
    auto stream = m_factory->CreateBundle(bundlePath + ".bundle.bin", fullPath);
    m_bundleToWriteOwned = std::make_unique<Bundle>(std::move(stream), br.get(), std::move(fullPath));
    m_bundleToWrite = m_bundleToWriteOwned.get();

    m_baseBundle->SetUncompressedSize(m_baseBundle->GetUncompressedSize() + br->GetRecordLength());
    m_bundles.push_back(std::move(br));
    return m_bundleToWrite;
}

void Index::Save() {
    // Save any pending bundle writes
    if (m_bundleToWrite && !m_writeBuffer.empty()) {
        m_bundleToWrite->Save(m_writeBuffer);
        m_bundleToWriteOwned.reset();
        m_bundleToWrite = nullptr;
        m_writeBuffer.clear();
    }
    m_bundleToWriteOwned.reset();
    m_bundleToWrite = nullptr;

    // Remove empty custom bundles
    for (int i = static_cast<int>(m_customBundles.size()) - 1; i >= 0; --i) {
        auto* cb = m_customBundles[static_cast<size_t>(i)];
        if (cb->GetFiles().empty()) {
            m_customBundles.erase(m_customBundles.begin() + i);
            // Remove from m_bundles too
            for (auto it = m_bundles.begin(); it != m_bundles.end(); ++it) {
                if (it->get() == cb) {
                    std::string bundlePath = cb->GetPath();
                    m_baseBundle->SetUncompressedSize(m_baseBundle->GetUncompressedSize() - cb->GetRecordLength());
                    m_bundles.erase(it);
                    m_factory->DeleteBundle(bundlePath);
                    break;
                }
            }
        }
    }

    // Reindex bundle indices
    for (int32_t i = 0; i < static_cast<int32_t>(m_bundles.size()); ++i) {
        // BundleIndex is const in the record, but we need to update references
        // This is handled by serialization using the current vector order
    }

    // Serialize all data
    std::vector<uint8_t> indexData;
    // Reserve approximate size
    indexData.reserve(static_cast<size_t>(m_baseBundle->GetUncompressedSize()));

    // Write bundle count + bundles
    int32_t bundleCount = static_cast<int32_t>(m_bundles.size());
    indexData.insert(indexData.end(),
                     reinterpret_cast<uint8_t*>(&bundleCount),
                     reinterpret_cast<uint8_t*>(&bundleCount) + sizeof(int32_t));

    for (auto& b : m_bundles)
        b->Serialize(indexData);

    // Write file count + files
    int32_t fileCount = static_cast<int32_t>(m_files.size());
    indexData.insert(indexData.end(),
                     reinterpret_cast<uint8_t*>(&fileCount),
                     reinterpret_cast<uint8_t*>(&fileCount) + sizeof(int32_t));

    // We need to serialize FileRecords with updated bundle indices.
    // Build a map from BundleRecord* to new index.
    std::unordered_map<BundleRecord*, int32_t> bundleIndexMap;
    for (int32_t i = 0; i < static_cast<int32_t>(m_bundles.size()); ++i)
        bundleIndexMap[m_bundles[static_cast<size_t>(i)].get()] = i;

    for (auto& [hash, fr] : m_files) {
        // Serialize with correct bundle index
        size_t pos = indexData.size();
        indexData.resize(pos + FileRecord::RECORD_LENGTH);

        uint64_t pathHash = fr->GetPathHash();
        std::memcpy(indexData.data() + pos, &pathHash, sizeof(uint64_t));
        pos += sizeof(uint64_t);

        int32_t bundleIdx = bundleIndexMap[fr->GetBundleRecord()];
        std::memcpy(indexData.data() + pos, &bundleIdx, sizeof(int32_t));
        pos += sizeof(int32_t);

        int32_t offset = fr->GetOffset();
        std::memcpy(indexData.data() + pos, &offset, sizeof(int32_t));
        pos += sizeof(int32_t);

        int32_t size = fr->GetSize();
        std::memcpy(indexData.data() + pos, &size, sizeof(int32_t));
    }

    // Write directory count + directories
    int32_t dirCount = static_cast<int32_t>(m_directories.size());
    indexData.insert(indexData.end(),
                     reinterpret_cast<uint8_t*>(&dirCount),
                     reinterpret_cast<uint8_t*>(&dirCount) + sizeof(int32_t));

    size_t dirBytes = m_directories.size() * sizeof(IndexDirectoryRecord);
    indexData.insert(indexData.end(),
                     reinterpret_cast<uint8_t*>(m_directories.data()),
                     reinterpret_cast<uint8_t*>(m_directories.data()) + dirBytes);

    // Write directory bundle data
    indexData.insert(indexData.end(), m_directoryBundleData.begin(), m_directoryBundleData.end());

    // Save the index as a bundle
    m_baseBundle->Save(indexData);
}

// FileRecord::Read implementation (needs Index and Bundle)
std::vector<uint8_t> FileRecord::Read(Bundle* bundle) const {
    if (bundle) {
        return bundle->Read(m_offset, m_size);
    }

    // Check if this file was redirected to the pending write buffer.
    // During patching, Write() redirects files to a custom bundle (e.g. LibGGPK3/0)
    // and stores data in Index::m_writeBuffer. The custom bundle is NOT saved to disk
    // until Index::Save(), so subsequent reads must pull from the buffer directly.
    auto* index = m_bundleRecord->GetIndex();
    if (index->m_bundleToWrite &&
        m_bundleRecord == index->m_bundleToWrite->GetRecord() &&
        !index->m_writeBuffer.empty()) {
        int32_t endPos = m_offset + m_size;
        if (m_offset >= 0 && m_size >= 0 &&
            endPos <= static_cast<int32_t>(index->m_writeBuffer.size())) {
            return std::vector<uint8_t>(
                index->m_writeBuffer.begin() + m_offset,
                index->m_writeBuffer.begin() + endPos);
        }
    }

    // Try to get bundle from factory (bundle is on disk).
    // Call the factory directly (not TryGetBundle) so the real failure reason
    // (sharing violation from a running game, missing file, ...) reaches the UI.
    std::unique_ptr<Bundle> owned;
    try {
        owned = index->GetFactory()->GetBundle(m_bundleRecord);
    } catch (const std::exception& ex) {
        throw std::runtime_error("Failed to get bundle: " + m_bundleRecord->GetPath()
                                 + " (" + std::string(ex.what()) + ")");
    }
    if (!owned)
        throw std::runtime_error("Failed to get bundle: " + m_bundleRecord->GetPath());
    return owned->Read(m_offset, m_size);
}

// FileRecord::Write implementation
void FileRecord::Write(const std::vector<uint8_t>& newContent, bool saveIndex) {
    Write(newContent.data(), static_cast<int32_t>(newContent.size()), saveIndex);
}

void FileRecord::Write(const uint8_t* data, int32_t size, bool saveIndex) {
    auto* index = m_bundleRecord->GetIndex();
    std::lock_guard<std::mutex> lock(index->ioMutex);

    if (!index->m_bundleToWrite) {
        int32_t originalSize = 0;
        index->GetBundleToWrite(originalSize);

        // Read existing data from the bundle being written to
        if (originalSize > 0) {
            auto existingData = index->m_bundleToWrite->ReadAll();
            // Defense in depth: GetBundleToWrite() validates this, but the copy
            // below is driven by two independent untrusted sources (index file
            // records vs bundle header) — never read past the actual content
            // (crash_506: 56MB assign from a 9MB bundle).
            if (static_cast<size_t>(originalSize) > existingData.size())
                throw std::runtime_error("Custom bundle "
                    + index->m_bundleToWrite->GetRecord()->GetPath()
                    + " is inconsistent with the index (contains "
                    + std::to_string(existingData.size()) + " bytes, index expects "
                    + std::to_string(originalSize)
                    + "). Restore the game files and re-apply patches.");
            index->m_writeBuffer.assign(existingData.begin(),
                                         existingData.begin() + originalSize);
        } else {
            index->m_writeBuffer.clear();
        }
    }

    // Redirect this file to the write buffer
    Redirect(index->m_bundleToWrite->GetRecord(),
             static_cast<int32_t>(index->m_writeBuffer.size()), size);

    // Append data to write buffer
    index->m_writeBuffer.insert(index->m_writeBuffer.end(), data, data + size);

    // Check if bundle is full
    if (static_cast<int32_t>(index->m_writeBuffer.size()) >= index->maxBundleSize) {
        index->m_bundleToWrite->Save(index->m_writeBuffer);
        index->m_bundleToWriteOwned.reset();
        index->m_bundleToWrite = nullptr;
        index->m_writeBuffer.clear();
    }

    if (saveIndex)
        index->Save();
}

void FileRecord::Redirect(BundleRecord* bundle, int32_t offset, int32_t size) {
    if (m_bundleRecord != bundle) {
        if (bundle->GetIndex() != m_bundleRecord->GetIndex())
            throw std::runtime_error("Cannot redirect file to bundle from different index");
        m_bundleRecord->RemoveFile(this);
        m_bundleRecord = bundle;
        bundle->AddFile(this);
    }
    m_offset = offset;
    m_size = size;
}

// BundleRecord::TryGetBundle implementation
std::unique_ptr<Bundle> BundleRecord::TryGetBundle() const {
    try {
        return m_index->GetFactory()->GetBundle(const_cast<BundleRecord*>(this));
    } catch (const std::exception& ex) {
        GGPKLog("[GGPK] TryGetBundle(" + GetPath() + ") failed: " + ex.what());
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

} // namespace GGPK
