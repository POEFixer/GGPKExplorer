#include "Index.h"
#include "DriveBundleFactory.h"
#include "TreeNode.h"
#include "GGPKHost.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace GGPK {

Index::Index(const std::string& filePath, bool parsePaths, std::unique_ptr<IBundleFactory> factory,
             OpenMode mode) {
    if (!factory) {
        auto parentDir = PathToUtf8(Utf8Path(filePath).parent_path());
        factory = std::make_unique<DriveBundleFactory>(parentDir);
    }
    m_factory = std::move(factory);

    // Open _.index.bin as a bundle
    Host::LogInfo("[GGPK] Opening index: " + filePath
                  + (mode == OpenMode::ReadOnly ? " (read-only)" : ""));
    m_baseBundle = std::make_unique<Bundle>(filePath, nullptr, mode);
    if (mode == OpenMode::ReadOnly) {
        MarkReadOnly("opened read-only");
    } else if (m_baseBundle->IsReadOnly()) {
        MarkReadOnly("_.index.bin could only be opened read-only: " + m_baseBundle->GetReadOnlyReason());
        Host::LogWarn("[GGPK] Index opened read-only (" + m_readOnlyReason + "); writes are refused");
    }

    // Decompress the index data
    auto data = m_baseBundle->ReadAll();
    if (m_readOnly)
        m_baseBundle->CloseStorage();   // never written: do not keep _.index.bin open
    Host::LogInfo("[GGPK] Index decompressed: " + std::to_string(data.size()) + " bytes");
    ParseIndexData(data);

    Host::LogInfo("[GGPK] Parsed: " + std::to_string(m_bundles.size()) + " bundles, "
        + std::to_string(m_files.size()) + " files, "
        + std::to_string(m_directories.size()) + " directories"
        + ", hash mode: " + std::string(m_useMurmurHash ? "MurmurHash64A" : "FNV1a64"));

    if (parsePaths) {
        int failed = ParsePaths();
        if (failed != 0) {
            Host::LogWarn("[GGPK] ParsePaths: " + std::to_string(failed) + " file(s) failed path resolution (orphaned hash entries, safe to ignore)");
        }
    }
}

Index::Index(std::vector<uint8_t> bundleData, bool parsePaths, std::unique_ptr<IBundleFactory> factory,
             OpenMode mode)
    : m_factory(std::move(factory)) {
    if (mode == OpenMode::ReadOnly)
        MarkReadOnly("opened read-only");

    m_baseBundle = std::make_unique<Bundle>(std::move(bundleData));

    auto data = m_baseBundle->ReadAll();
    Host::LogInfo("[GGPK] Index decompressed (from memory): " + std::to_string(data.size()) + " bytes");
    ParseIndexData(data);

    Host::LogInfo("[GGPK] Parsed: " + std::to_string(m_bundles.size()) + " bundles, "
        + std::to_string(m_files.size()) + " files, "
        + std::to_string(m_directories.size()) + " directories");

    if (parsePaths) {
        int failed = ParsePaths();
        if (failed != 0) {
            Host::LogWarn("[GGPK] ParsePaths: " + std::to_string(failed) + " file(s) failed path resolution (orphaned hash entries, safe to ignore)");
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
    const uint8_t* dataEnd = base + directory.size();
    const int64_t dirDataSize = static_cast<int64_t>(directory.size());

    // Reused for every path (no allocation per path once warm): the directory record's
    // prefixes (the first tempCount entries), the path being built and its lowered copy.
    std::vector<std::string> temp;
    size_t tempCount = 0;
    std::string combined;
    std::string lower;

    for (const auto& d : m_directories) {
        // Directory records were memcpy'd raw from the index file — validate
        // before forming pointers (corrupt offsets would read wild memory).
        if (d.offset < 0 || d.size < 4 ||
            static_cast<int64_t>(d.offset) + d.size > dirDataSize) {
            ++skippedDirs;
            continue;
        }

        tempCount = 0;   // each directory record starts with no prefixes
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
                    tempCount = 0;
            } else {
                index -= 1;

                // Read null-terminated string
                const uint8_t* strStart = ptr;
                while (ptr < dataEnd && *ptr != 0)
                    ++ptr;
                size_t strLen = static_cast<size_t>(ptr - strStart);
                if (ptr < dataEnd) ++ptr; // skip null terminator

                // Prefix + current string (no prefix when the index is out of range)
                const bool hasPrefix = index >= 0 && index < static_cast<int32_t>(tempCount);
                if (baseMode) {
                    if (tempCount == temp.size())
                        temp.emplace_back();
                    std::string& slot = temp[tempCount];
                    if (hasPrefix)
                        slot = temp[static_cast<size_t>(index)];   // another entry: index < tempCount
                    else
                        slot.clear();
                    slot.append(reinterpret_cast<const char*>(strStart), strLen);
                    ++tempCount;
                } else {
                    if (hasPrefix)
                        combined = temp[static_cast<size_t>(index)];
                    else
                        combined.clear();
                    combined.append(reinterpret_cast<const char*>(strStart), strLen);

                    // File path - look up by hash
                    uint64_t hash;
                    if (m_useMurmurHash) {
                        // Lowercase for MurmurHash (the same rule as NameHash::ComputeHash)
                        lower.resize(combined.size());
                        for (size_t i = 0; i < combined.size(); ++i)
                            lower[i] = static_cast<char>(NameHash::AsciiLower(static_cast<uint8_t>(combined[i])));
                        hash = NameHash::MurmurHash64A(reinterpret_cast<const uint8_t*>(lower.data()), lower.size());
                    } else {
                        hash = NameHash::FNV1a64Hash(reinterpret_cast<const uint8_t*>(combined.data()), combined.size());
                    }

                    auto it = m_files.find(hash);
                    if (it != m_files.end()) {
                        it->second->SetPath(combined);
                    } else {
                        ++failed;
                    }
                }
            }
        }
    }

    if (skippedDirs != 0)
        Host::LogWarn("[GGPK] ParsePaths: skipped " + std::to_string(skippedDirs)
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

    // The directories of the previous path, reused for the common leading segments (paths
    // are sorted, so neighbours share most of their directories), and the most recently
    // created child directory of each directory.
    struct Level {
        std::string_view name;
        DirectoryNode* node;
    };
    std::vector<Level> chain;
    std::vector<std::string_view> segments;
    std::unordered_map<const DirectoryNode*, DirectoryNode*> lastChildDir;

    for (auto* fr : sortedFiles) {
        const auto& path = fr->GetPath();

        // Split path by '/' (exactly as before: the directory segments are what the loop
        // below yields up to the last '/', the file name is what follows it)
        segments.clear();
        size_t start = 0;
        size_t lastSlash = path.rfind('/');

        if (lastSlash != std::string::npos) {
            size_t pos = 0;
            while (pos < lastSlash) {
                size_t nextSlash = path.find('/', pos);
                if (nextSlash == std::string::npos || nextSlash > lastSlash)
                    nextSlash = lastSlash;
                segments.emplace_back(path.data() + pos, nextSlash - pos);
                pos = nextSlash + 1;
            }
            start = lastSlash + 1;
        }

        size_t depth = 0;
        while (depth < chain.size() && depth < segments.size() && chain[depth].name == segments[depth])
            ++depth;
        chain.resize(depth);
        DirectoryNode* parent = depth ? chain[depth - 1].node : root.get();
        for (size_t i = depth; i < segments.size(); ++i) {
            // Sorted paths keep each directory's files contiguous: the only existing child
            // directory this segment can name is the parent's most recently created one
            // (the chain leaves it only for files of the parent itself, e.g. "a//x"
            // between "a//b/c" and "a//d/e"). Same tree, children and order as the old
            // GetOrCreateChildDir scan over every child (gate: ggpk_bundle_test build_tree).
            DirectoryNode*& last = lastChildDir[parent];
            if (!last || last->GetName() != segments[i])
                last = parent->AddChildDir(std::string(segments[i]));
            parent = last;
            chain.push_back({segments[i], parent});
        }

        // Add file node
        parent->AddChild(std::make_unique<FileNode>(path.substr(start), fr, parent));
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
            Host::LogWarn("[GGPK] Custom bundle " + cb->GetPath() + " holds "
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

void Index::ThrowIfReadOnly() const {
    if (m_readOnly)
        throw std::runtime_error("Cannot write game files: the index is read-only (" + m_readOnlyReason
                                 + "). Close the game and try again.");
}

void Index::Save() {
    // A read-only index fails before anything is written.
    ThrowIfReadOnly();
    std::unique_lock<std::shared_mutex> storage(m_storageMutex);
    std::lock_guard<std::mutex> io(ioMutex);
    SaveLocked();
}

void Index::SaveLocked() {
    ThrowIfReadOnly();
    // Bundles are rewritten and BundleRecords may be erased below: nothing read before
    // may be served afterwards (and no cached handle is kept on a file being rewritten).
    m_readCache.Clear();
    ++m_storageGeneration;

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
                    m_retiredBundles.push_back(std::move(*it));   // never freed while the Index lives
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

    auto* index = GetBundleRecord()->GetIndex();
    std::shared_lock<std::shared_mutex> storage(index->m_storageMutex);
    BundleRecord* record;
    int32_t offset, size;
    {
        // Check if this file was redirected to the pending write buffer.
        // During patching, Write() redirects files to a custom bundle (e.g. LibGGPK3/0)
        // and stores data in Index::m_writeBuffer. The custom bundle is NOT saved to disk
        // until Index::Save(), so subsequent reads must pull from the buffer directly —
        // before any cache or bundle on disk is consulted.
        std::lock_guard<std::mutex> io(index->ioMutex);
        record = GetBundleRecord();
        offset = m_offset;
        size = m_size;
        if (index->m_bundleToWrite &&
            record == index->m_bundleToWrite->GetRecord() &&
            !index->m_writeBuffer.empty()) {
            const int64_t endPos = static_cast<int64_t>(offset) + size;
            if (offset >= 0 && size >= 0 &&
                endPos <= static_cast<int64_t>(index->m_writeBuffer.size())) {
                return std::vector<uint8_t>(
                    index->m_writeBuffer.begin() + offset,
                    index->m_writeBuffer.begin() + endPos);
            }
        }
    }
    return index->ReadFromBundle(record, offset, size);
}

std::shared_ptr<Bundle> Index::AcquireReadBundle(BundleRecord* record) {
    // Read-only open from the factory (never write access: the running game and its
    // patcher are not blocked). Call the factory directly (not TryGetBundle) so the real
    // failure reason (sharing violation, missing file, ...) reaches the UI.
    std::shared_ptr<Bundle> bundle;
    // Custom bundles are the ones another Index or program rewrites (Smoother's Apply while
    // GGPK Explorer holds one, a second POEFixer): re-checked against their file per acquire.
    const bool mayChange = record->GetPathBase().compare(0, std::char_traits<char>::length(CUSTOM_BUNDLE_BASE_PATH),
                                                         CUSTOM_BUNDLE_BASE_PATH) == 0;
    try {
        bundle = m_readCache.AcquireBundle(record, *m_factory, mayChange);
    } catch (const std::exception& ex) {
        throw std::runtime_error("Failed to get bundle: " + record->GetPath()
                                 + " (" + std::string(ex.what()) + ")");
    }
    if (!bundle)
        throw std::runtime_error("Failed to get bundle: " + record->GetPath());
    return bundle;
}

std::shared_ptr<const DecodedChunk> Index::GetChunk(Bundle& bundle, const BundleRecord* record, int32_t chunk) {
    if (auto hit = m_readCache.FindChunk(record, chunk, bundle.GetChunkDecodedSize(chunk))) {
        m_readCache.CountChunkHit();
        return hit;
    }
    auto decoded = std::make_shared<DecodedChunk>();
    decoded->size = bundle.GetChunkDecodedSize(chunk);
    decoded->data.reset(new uint8_t[static_cast<size_t>(decoded->size)]);   // no zero-fill
    bundle.DecodeChunks(chunk, chunk + 1, decoded->data.get(), false);
    m_readCache.CountDecoded(1, static_cast<uint64_t>(decoded->size));
    m_readCache.PutChunk(record, chunk, decoded);
    return decoded;
}

std::vector<uint8_t> Index::ReadFromBundle(BundleRecord* record, int32_t offset, int32_t size) {
    auto bundle = AcquireReadBundle(record);
    if (size == 0)
        return {};
    if (offset < 0 || size < 0 ||
        static_cast<int64_t>(offset) + size > static_cast<int64_t>(bundle->GetUncompressedSize()))
        throw std::out_of_range("Read range out of bounds");

    const int64_t cs = bundle->GetChunkSize();
    const int64_t begin = offset;
    const int64_t end = begin + size;
    const int32_t c0 = static_cast<int32_t>(begin / cs);
    const int32_t c1 = static_cast<int32_t>((end - 1) / cs) + 1;
    auto chunkStart = [&](int32_t c) { return static_cast<int64_t>(c) * cs; };
    auto chunkEnd = [&](int32_t c) { return chunkStart(c) + bundle->GetChunkDecodedSize(c); };

    std::vector<uint8_t> result;
    if (c1 - c0 <= 2) {
        // Small file: one or two chunks, from the cache (decoded once, shared by the
        // neighbouring files of the same chunk). No zero-filled buffer, one copy.
        result.reserve(static_cast<size_t>(size));
        for (int32_t c = c0; c < c1; ++c) {
            auto chunk = GetChunk(*bundle, record, c);
            const int64_t from = (std::max)(begin, chunkStart(c)) - chunkStart(c);
            const int64_t to = (std::min)(end, chunkEnd(c)) - chunkStart(c);
            result.insert(result.end(), chunk->data.get() + from, chunk->data.get() + to);
        }
        return result;
    }

    // Large file: chunks wholly inside decode straight into the result (in parallel, not
    // cached: they belong to this file alone); the partial first/last chunk may hold
    // neighbouring files and goes through the cache.
    result.resize(static_cast<size_t>(size));
    const int32_t f0 = chunkStart(c0) == begin ? c0 : c0 + 1;
    const int32_t f1 = chunkEnd(c1 - 1) == end ? c1 : c1 - 1;
    if (c0 < f0) {
        auto chunk = GetChunk(*bundle, record, c0);
        std::memcpy(result.data(), chunk->data.get() + (begin - chunkStart(c0)),
                    static_cast<size_t>(chunkEnd(c0) - begin));
    }
    bundle->DecodeChunks(f0, f1, result.data() + (chunkStart(f0) - begin), true);
    m_readCache.CountDecoded(static_cast<uint64_t>(f1 - f0),
                             static_cast<uint64_t>((std::min)(chunkStart(f1), static_cast<int64_t>(bundle->GetUncompressedSize())) - chunkStart(f0)));
    if (f1 < c1) {
        auto chunk = GetChunk(*bundle, record, c1 - 1);
        std::memcpy(result.data() + (chunkStart(c1 - 1) - begin), chunk->data.get(),
                    static_cast<size_t>(end - chunkStart(c1 - 1)));
    }
    return result;
}

// ============================================================================
// Batch read
// ============================================================================

namespace {

struct BatchItem {
    size_t position;
    FileRecord* record;
    BundleRecord* bundle;     // where the snapshot found the file (dereferenced only while a
                              // file still names it, under the storage lock)
    int32_t bundleIndex;      // the snapshot's, so sorting never touches the record
    int32_t offset;
    int32_t size;
};

} // namespace

ReadFilesResult Index::ReadFiles(const std::vector<FileRecord*>& records, const ReadFilesSink& sink,
                                 const std::atomic<bool>* cancel, unsigned workers) {
    ReadFilesOptions options;
    options.workers = workers;
    return ReadFiles(records, sink, cancel, options);
}

ReadFilesResult Index::ReadFiles(const std::vector<FileRecord*>& records, const ReadFilesSink& sink,
                                 const std::atomic<bool>* cancel, const ReadFilesOptions& options) {
    if (!sink)
        throw std::invalid_argument("ReadFiles needs a sink");

    std::atomic<size_t> delivered{0}, failed{0};
    std::atomic<bool> stop{false};          // cancelled or a sink threw
    std::atomic<bool> cancelled{false};
    std::mutex sinkMutex, stateMutex;
    std::exception_ptr sinkFailure;
    std::vector<size_t> viaRead;            // positions read through FileRecord::Read

    auto isCancelled = [&]() {
        if (stop.load(std::memory_order_relaxed))
            return true;
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            cancelled = true;
            stop = true;
            return true;
        }
        return false;
    };
    // Calls the sink; an exception from it stops the batch (rethrown at the end).
    auto deliver = [&](size_t position, FileRecord* record, std::vector<uint8_t>&& bytes, const std::string* error) {
        if (isCancelled())
            return;
        try {
            if (options.serializeSink) {
                std::lock_guard<std::mutex> lock(sinkMutex);
                sink(position, record, std::move(bytes), error);
            } else {
                sink(position, record, std::move(bytes), error);
            }
            ++(error ? failed : delivered);
        } catch (...) {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (!sinkFailure)
                sinkFailure = std::current_exception();
            stop = true;
        }
    };

    // 1. Snapshot every record's location. (Files in the pending write buffer are routed to
    //    the normal read path by the per-group check below, like files that move later.)
    std::vector<BatchItem> items;
    std::vector<std::pair<size_t, std::string>> refused;
    items.reserve(records.size());
    {
        std::shared_lock<std::shared_mutex> storage(m_storageMutex);
        std::lock_guard<std::mutex> io(ioMutex);
        for (size_t i = 0; i < records.size(); ++i) {
            FileRecord* r = records[i];
            if (!r) {
                refused.emplace_back(i, "null file record");
                continue;
            }
            BundleRecord* br = r->GetBundleRecord();
            if (br->GetIndex() != this) {
                refused.emplace_back(i, "file record of another index");
                continue;
            }
            items.push_back({i, r, br, br->GetBundleIndex(), r->GetOffset(), r->GetSize()});
        }
    }

    // Sorted on the snapshot alone: a sink call below may already Write or Save.
    std::sort(items.begin(), items.end(), [](const BatchItem& a, const BatchItem& b) {
        if (a.bundleIndex != b.bundleIndex) return a.bundleIndex < b.bundleIndex;
        if (a.bundle != b.bundle) return a.bundle < b.bundle;
        if (a.offset != b.offset) return a.offset < b.offset;
        if (a.size != b.size) return a.size < b.size;
        return a.position < b.position;
    });
    std::vector<std::pair<size_t, size_t>> groups;   // [begin, end) per bundle
    for (size_t i = 0; i < items.size();) {
        size_t j = i + 1;
        while (j < items.size() && items[j].bundle == items[i].bundle) ++j;
        groups.emplace_back(i, j);
        i = j;
    }
    for (auto& [position, why] : refused)
        deliver(position, records[position], {}, &why);

    // 2. Workers take whole bundles. A sink call may Write or Save meanwhile: files move,
    //    bundle files are rewritten and Save may remove an emptied custom BundleRecord. So
    //    every file is checked against its snapshot under the storage lock before its
    //    bundle is opened and before its run is decoded (a file that moved takes the normal
    //    read path), and a group's BundleRecord is touched only under that lock while a file
    //    of the group still names it: Save removes a BundleRecord only once no file names it
    //    (and retires it rather than freeing it).
    static const std::string kOutOfRange = "Read range out of bounds";
    std::atomic<size_t> nextGroup{0};
    auto processGroup = [&](size_t g, std::vector<size_t>& moved) {
        const size_t first = groups[g].first, last = groups[g].second;
        BundleRecord* const br = items[first].bundle;
        std::shared_ptr<Bundle> bundle;
        uint64_t bundleGeneration = 0;
        int64_t cs = 1, total = 0;
        std::string groupError;
        // Caller holds m_storageMutex shared and ioMutex: the file is where the snapshot found
        // it and not in the pending write buffer.
        auto unchanged = [&](const BatchItem& it) {
            return it.record->GetBundleRecord() == it.bundle
                && it.record->GetOffset() == it.offset && it.record->GetSize() == it.size
                && !(m_bundleToWrite && it.bundle == m_bundleToWrite->GetRecord() && !m_writeBuffer.empty());
        };
        // Caller holds m_storageMutex shared and has seen a file of the group still in br.
        auto acquire = [&]() {
            bundle = AcquireReadBundle(br);
            bundleGeneration = m_storageGeneration;
            cs = bundle->GetChunkSize();
            total = bundle->GetUncompressedSize();
        };
        std::vector<char> gone(last - first, 0);   // moved before the bundle was opened
        {
            std::shared_lock<std::shared_mutex> storage(m_storageMutex);
            bool any = false;
            {
                std::lock_guard<std::mutex> io(ioMutex);
                for (size_t k = first; k < last; ++k) {
                    if (unchanged(items[k])) {
                        any = true;
                    } else {
                        gone[k - first] = 1;
                        moved.push_back(items[k].position);
                    }
                }
            }
            if (!any)
                return;   // every file left br (which Save may have removed): never opened
            try {
                acquire();
            } catch (const std::exception& ex) {
                groupError = ex.what();
            } catch (...) {
                groupError = "Failed to get bundle: " + br->GetPath();
            }
        }
        if (!groupError.empty()) {
            // The open failed while these files were still in br: FileRecord::Read fails alike.
            for (size_t k = first; k < last; ++k)
                if (!gone[k - first])
                    deliver(items[k].position, items[k].record, {}, &groupError);
            return;
        }
        auto chunkOf = [&](int64_t at) { return static_cast<int32_t>(at / cs); };

        size_t k = first;
        while (k < last) {
            if (isCancelled())
                return;
            // A run: neighbouring files whose chunks touch, within maxRunBytes decoded.
            size_t runEnd = k;
            int32_t r0 = 0, r1 = 0;
            std::vector<size_t> runErrors;   // out of range in the plan's geometry
            std::vector<size_t> runItems;
            bool haveData = false;           // runItems holds a non-empty file: [r0, r1) is set
            for (; runEnd < last; ++runEnd) {
                if (gone[runEnd - first])
                    continue;
                const BatchItem& it = items[runEnd];
                if (it.size == 0) {
                    // Empty once the bundle opens, whatever its offset: FileRecord::Read's
                    // outcome (ReadFromBundle returns before its range check).
                    runItems.push_back(runEnd);
                    continue;
                }
                const int64_t end = static_cast<int64_t>(it.offset) + it.size;
                if (it.offset < 0 || it.size < 0 || end > total) {
                    if (runItems.empty()) runErrors.push_back(runEnd);
                    else break;   // reported with the next run, in order
                    continue;
                }
                const int32_t c0 = chunkOf(it.offset), c1 = chunkOf(end - 1) + 1;
                if (!haveData) {
                    haveData = true;
                    r0 = c0;
                    r1 = c1;
                    runItems.push_back(runEnd);
                    if ((static_cast<int64_t>(r1) - r0) * cs > static_cast<int64_t>(options.maxRunBytes)) {
                        ++runEnd;   // a large file alone
                        break;
                    }
                    continue;
                }
                const int32_t n1 = (std::max)(r1, c1);
                if (c0 > r1 || (static_cast<int64_t>(n1) - r0) * cs > static_cast<int64_t>(options.maxRunBytes))
                    break;
                r1 = n1;
                runItems.push_back(runEnd);
            }
            if (runItems.empty() && runErrors.empty()) {
                k = runEnd;
                continue;
            }

            // Decode the run under the storage lock (every file re-checked against its
            // snapshot first), then hand the files out without any lock.
            std::vector<std::pair<size_t, std::vector<uint8_t>>> ready;
            std::vector<size_t> errors;   // runErrors still in br
            ready.reserve(runItems.size());
            {
                std::shared_lock<std::shared_mutex> storage(m_storageMutex);
                std::vector<size_t> still;
                {
                    std::lock_guard<std::mutex> io(ioMutex);
                    for (size_t x : runErrors) {
                        if (unchanged(items[x])) errors.push_back(x);
                        else moved.push_back(items[x].position);
                    }
                    for (size_t x : runItems) {
                        if (unchanged(items[x])) still.push_back(x);
                        else moved.push_back(items[x].position);
                    }
                }
                if ((!still.empty() || !errors.empty()) && bundleGeneration != m_storageGeneration) {
                    // Bundle files were saved since this bundle was opened (a file still names
                    // br): its header and table may be stale. Reopen; a changed geometry voids
                    // the run plan, and its files take the normal read path.
                    const int64_t oldCs = cs, oldTotal = total;
                    try {
                        acquire();
                    } catch (...) {
                        bundle.reset();
                    }
                    if (!bundle || cs != oldCs || total != oldTotal) {
                        for (size_t x : errors) moved.push_back(items[x].position);
                        for (size_t x : still) moved.push_back(items[x].position);
                        errors.clear();
                        still.clear();
                    }
                }
                try {
                    size_t dataItems = 0, lastData = 0;
                    for (size_t x : still)
                        if (items[x].size > 0) { ++dataItems; lastData = x; }
                    if (dataItems == 1 &&
                        (static_cast<int64_t>(r1) - r0) * cs > static_cast<int64_t>(options.maxRunBytes)) {
                        // One large file: straight into its own buffer (never a run buffer).
                        for (size_t x : still) {
                            if (x != lastData) { ready.emplace_back(x, std::vector<uint8_t>()); continue; }
                            const BatchItem& it = items[x];
                            std::vector<uint8_t> bytes(static_cast<size_t>(it.size));
                            bundle->ReadInto(it.offset, it.size, bytes.data(), false);
                            m_readCache.CountDecoded(static_cast<uint64_t>(r1 - r0), static_cast<uint64_t>(it.size));
                            ready.emplace_back(x, std::move(bytes));
                        }
                    } else {
                        const bool anyData = dataItems > 0;
                        std::unique_ptr<uint8_t[]> run;
                        int64_t runStart = static_cast<int64_t>(r0) * cs;
                        if (anyData) {
                            const int64_t runBytes = (std::min)(static_cast<int64_t>(r1) * cs, total) - runStart;
                            run.reset(new uint8_t[static_cast<size_t>(runBytes)]);   // no zero-fill
                            bundle->DecodeChunks(r0, r1, run.get(), false);
                            m_readCache.CountDecoded(static_cast<uint64_t>(r1 - r0), static_cast<uint64_t>(runBytes));
                        }
                        for (size_t x : still) {
                            const BatchItem& it = items[x];
                            if (it.size == 0) {
                                ready.emplace_back(x, std::vector<uint8_t>());
                                continue;
                            }
                            const uint8_t* from = run.get() + (it.offset - runStart);
                            ready.emplace_back(x, std::vector<uint8_t>(from, from + it.size));
                        }
                    }
                } catch (...) {
                    // A corrupt chunk fails the whole run: each of its files is read on its
                    // own instead (the normal path), so a file's outcome is exactly what
                    // FileRecord::Read gives it.
                    m_readCache.CountBatchRunFailure();
                    ready.clear();
                    for (size_t x : still)
                        moved.push_back(items[x].position);
                }
            }
            for (size_t x : errors)
                deliver(items[x].position, items[x].record, {}, &kOutOfRange);
            for (auto& [x, bytes] : ready) {
                if (isCancelled())
                    return;
                deliver(items[x].position, items[x].record, std::move(bytes), nullptr);
            }
            k = runEnd;
        }
    };

    auto worker = [&]() {
        std::vector<size_t> moved;
        try {
            for (;;) {
                if (isCancelled())
                    break;
                const size_t g = nextGroup.fetch_add(1);
                if (g >= groups.size())
                    break;
                processGroup(g, moved);
            }
        } catch (...) {
            // processGroup reports its own errors; this only keeps an unexpected one
            // (e.g. bad_alloc building a run) from escaping the thread.
            std::lock_guard<std::mutex> lock(stateMutex);
            if (!sinkFailure)
                sinkFailure = std::current_exception();
            stop = true;
        }
        std::lock_guard<std::mutex> lock(stateMutex);
        viaRead.insert(viaRead.end(), moved.begin(), moved.end());
    };

    unsigned threads = options.workers;
    if (threads == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        threads = (std::min)(hw == 0 ? 2u : hw, 4u);
    }
    threads = static_cast<unsigned>((std::min)(static_cast<size_t>(threads), (std::max)(groups.size(), static_cast<size_t>(1))));
    if (threads <= 1) {
        worker();
    } else {
        std::vector<std::thread> pool;
        pool.reserve(threads);
        try {
            for (unsigned t = 0; t < threads; ++t)
                pool.emplace_back(worker);
        } catch (...) {
            // Fewer threads than asked: the started ones (and this thread) finish the work.
            if (pool.empty())
                worker();
        }
        for (auto& th : pool)
            th.join();
    }

    // 3. Files in the pending write buffer (or moved meanwhile): the normal read path.
    std::sort(viaRead.begin(), viaRead.end());
    for (size_t position : viaRead) {
        if (isCancelled())
            break;
        std::vector<uint8_t> bytes;
        std::string error;
        try {
            bytes = records[position]->Read();
        } catch (const std::exception& ex) {
            error = ex.what();
        } catch (...) {
            error = "unknown error";
        }
        deliver(position, records[position], std::move(bytes), error.empty() ? nullptr : &error);
    }

    if (sinkFailure)
        std::rethrow_exception(sinkFailure);
    if (cancel && cancel->load(std::memory_order_relaxed))
        cancelled = true;
    ReadFilesResult result;
    result.delivered = delivered.load();
    result.failed = failed.load();
    result.cancelled = cancelled.load();
    return result;
}

// ============================================================================
// ReadCache
// ============================================================================

namespace {

int64_t SteadyTicks() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

} // namespace

std::shared_ptr<Bundle> ReadCache::AcquireBundle(BundleRecord* record, IBundleFactory& factory, bool validate) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_bundles.find(record);
        if (it != m_bundles.end()) {
            m_bundleOrder.splice(m_bundleOrder.begin(), m_bundleOrder, it->second);
            BundleEntry& entry = *it->second;
            bool current = true;
            if (entry.validate) {
                // Its file may have been rewritten by another Index or program since it was
                // opened (a stale header and table would decode the new bytes with the old
                // layout): checked on the handle, at most once per kRecheckInterval.
                const int64_t now = SteadyTicks();
                if (now - entry.checkedAt >= m_recheckTicks) {
                    current = entry.bundle->IsUnchangedOnDisk();
                    entry.checkedAt = now;
                }
            }
            if (current) {
                m_bundleHits.fetch_add(1, std::memory_order_relaxed);
                return entry.bundle;
            }
            m_staleBundles.fetch_add(1, std::memory_order_relaxed);
            DropLocked(record);   // and reopened below
        }
    }
    // Open outside the lock (I/O); another thread may open the same bundle meanwhile.
    std::shared_ptr<Bundle> opened(factory.OpenForRead(record).release());
    if (!opened)
        return nullptr;
    m_bundleOpens.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_mutex);
    // Every bundle open updates the record's size from the fresh header (Save writes it).
    record->SetUncompressedSize(opened->GetUncompressedSize());
    if (!opened->IsConcurrentReadable() || m_maxBundles == 0) {
        // Not kept (a read-write stream is never shared between readers; no bundle cache):
        // every acquire opens the file again, and chunks of an earlier open may be stale.
        if (validate)
            DropLocked(record);
        return opened;
    }
    auto it = m_bundles.find(record);
    if (it != m_bundles.end()) {
        m_bundleOrder.splice(m_bundleOrder.begin(), m_bundleOrder, it->second);
        return it->second->bundle;
    }
    if (validate)
        DropLocked(record);   // chunks decoded from an earlier open of a file that may have changed
    m_bundleOrder.push_front(BundleEntry{record, opened, validate, SteadyTicks()});
    m_bundles[record] = m_bundleOrder.begin();
    TrimLocked();
    return opened;
}

std::shared_ptr<const DecodedChunk> ReadCache::FindChunk(const BundleRecord* record, int32_t chunk, int32_t decodedSize) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_chunks.find(ChunkKey{record, chunk});
    if (it == m_chunks.end())
        return nullptr;
    if (it->second->second->size != decodedSize) {
        // Decoded from an older layout of the bundle's file: never served.
        m_chunkBytes -= static_cast<size_t>(it->second->second->size);
        m_chunkOrder.erase(it->second);
        m_chunks.erase(it);
        return nullptr;
    }
    m_chunkOrder.splice(m_chunkOrder.begin(), m_chunkOrder, it->second);
    return it->second->second;
}

void ReadCache::PutChunk(const BundleRecord* record, int32_t chunk, std::shared_ptr<const DecodedChunk> data) {
    if (!data)
        return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (static_cast<size_t>(data->size) > m_chunkBudget)
        return;
    const ChunkKey key{record, chunk};
    if (m_chunks.count(key))
        return;
    m_chunkBytes += static_cast<size_t>(data->size);
    m_chunkOrder.emplace_front(key, std::move(data));
    m_chunks[key] = m_chunkOrder.begin();
    TrimLocked();
}

void ReadCache::TrimLocked() {
    while (m_bundleOrder.size() > m_maxBundles) {
        m_bundles.erase(m_bundleOrder.back().record);
        m_bundleOrder.pop_back();
    }
    while (m_chunkBytes > m_chunkBudget && !m_chunkOrder.empty()) {
        m_chunkBytes -= static_cast<size_t>(m_chunkOrder.back().second->size);
        m_chunks.erase(m_chunkOrder.back().first);
        m_chunkOrder.pop_back();
    }
}

void ReadCache::InvalidateBundle(const BundleRecord* record) {
    std::lock_guard<std::mutex> lock(m_mutex);
    DropLocked(record);
}

void ReadCache::DropLocked(const BundleRecord* record) {
    auto it = m_bundles.find(record);
    if (it != m_bundles.end()) {
        m_bundleOrder.erase(it->second);
        m_bundles.erase(it);
    }
    for (auto c = m_chunkOrder.begin(); c != m_chunkOrder.end();) {
        if (c->first.record == record) {
            m_chunkBytes -= static_cast<size_t>(c->second->size);
            m_chunks.erase(c->first);
            c = m_chunkOrder.erase(c);
        } else {
            ++c;
        }
    }
}

void ReadCache::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_bundles.clear();
    m_bundleOrder.clear();
    m_chunks.clear();
    m_chunkOrder.clear();
    m_chunkBytes = 0;
}

void ReadCache::SetRecheckInterval(std::chrono::nanoseconds interval) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_recheckTicks = (std::max)(int64_t{0},
        static_cast<int64_t>(std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval).count()));
}

void ReadCache::SetLimits(size_t maxBundles, size_t chunkBudgetBytes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_maxBundles = maxBundles;
    m_chunkBudget = chunkBudgetBytes;
    TrimLocked();
}

ReadCacheStats ReadCache::Stats() const {
    ReadCacheStats s;
    s.bundleOpens = m_bundleOpens.load(std::memory_order_relaxed);
    s.bundleHits = m_bundleHits.load(std::memory_order_relaxed);
    s.chunksDecoded = m_chunksDecoded.load(std::memory_order_relaxed);
    s.chunkHits = m_chunkHits.load(std::memory_order_relaxed);
    s.bytesDecoded = m_bytesDecoded.load(std::memory_order_relaxed);
    s.staleBundles = m_staleBundles.load(std::memory_order_relaxed);
    s.batchRunFailures = m_batchRunFailures.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_mutex);
    s.openBundles = m_bundleOrder.size();
    s.cachedChunks = m_chunkOrder.size();
    s.cachedChunkBytes = m_chunkBytes;
    return s;
}

// FileRecord::Write implementation
void FileRecord::Write(const std::vector<uint8_t>& newContent, bool saveIndex) {
    Write(newContent.data(), static_cast<int32_t>(newContent.size()), saveIndex);
}

void FileRecord::Write(const uint8_t* data, int32_t size, bool saveIndex) {
    auto* index = GetBundleRecord()->GetIndex();
    index->ThrowIfReadOnly();
    // Readers wait while a write moves records and (at a rollover) rewrites a bundle.
    std::unique_lock<std::shared_mutex> storage(index->m_storageMutex);
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
        // The bundle's file is rewritten: drop what was read from it before.
        index->m_readCache.InvalidateBundle(index->m_bundleToWrite->GetRecord());
        ++index->m_storageGeneration;
        index->m_bundleToWrite->Save(index->m_writeBuffer);
        index->m_bundleToWriteOwned.reset();
        index->m_bundleToWrite = nullptr;
        index->m_writeBuffer.clear();
    }

    if (saveIndex)
        index->SaveLocked();
}

void FileRecord::Redirect(BundleRecord* bundle, int32_t offset, int32_t size) {
    BundleRecord* current = GetBundleRecord();
    if (current != bundle) {
        if (bundle->GetIndex() != current->GetIndex())
            throw std::runtime_error("Cannot redirect file to bundle from different index");
        current->RemoveFile(this);
        m_bundleRecord.store(bundle, std::memory_order_relaxed);
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
        Host::LogWarn("[GGPK] TryGetBundle(" + GetPath() + ") failed: " + ex.what());
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

} // namespace GGPK
