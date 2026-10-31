#include "GGPK.h"
#include "Bundle.h" // DescribeLastOpenError, Utf8Path
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <Windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

namespace GGPK {

GGPKFile::GGPKFile(const std::string& filePath, OpenMode mode)
    : m_filePath(filePath) {
    if (mode == OpenMode::ReadWrite) {
        // Content.ggpk needs read-write access for in-place writes. When that is refused
        // (the running game) it is still readable: open read-only and let the write
        // methods report why.
        m_stream.open(Utf8Path(filePath), std::ios::in | std::ios::out | std::ios::binary);
        if (!m_stream.is_open()) {
            m_readOnlyReason = "the read-write open was refused: " + DescribeLastOpenError();
            m_stream.clear();
            m_readOnly = true;
        }
    } else {
        m_readOnly = true;
        m_readOnlyReason = "opened read-only";
    }
    // Every read goes through this handle (positional, shared read/write/delete).
    m_readFile = ReadOnlyFile::Open(filePath, "Failed to open GGPK file");
    m_scratch.resize(4096);

    // GGPKRecord at offset 0: int32 length, tag, uint32 version, int64 rootDirOffset,
    // int64 firstFreeOffset
    LoadRecord(0, 28);
    int32_t tag = 0;
    if (m_scratchValid >= 8)
        std::memcpy(&tag, m_scratch.data() + 4, sizeof(int32_t));
    if (tag != RecordTag::GGPK || m_scratchValid < 28)
        throw std::runtime_error("Invalid GGPK file: first record is not GGPKRecord");

    int32_t versionInt;
    std::memcpy(&versionInt, m_scratch.data() + 8, sizeof(int32_t));
    m_version = static_cast<uint32_t>(versionInt);
    std::memcpy(&m_rootOffset, m_scratch.data() + 12, sizeof(int64_t));
    std::memcpy(&m_firstFreeOffset, m_scratch.data() + 20, sizeof(int64_t));

    // Read root directory and build tree
    m_root = std::make_unique<GGPKTreeNode>();
    ReadDirectoryRecord(m_rootOffset, *m_root);
    ReadTree(*m_root);
    m_scratch.clear();
    m_scratch.shrink_to_fit();
    m_scratchValid = 0;
}

GGPKFile::~GGPKFile() {
    if (m_stream.is_open())
        m_stream.close();
}

void GGPKFile::ThrowIfReadOnly(const char* operation) const {
    if (m_readOnly)
        throw std::runtime_error(std::string(operation) + ": " + m_filePath + " is open read-only ("
                                 + m_readOnlyReason + "). Close the game and try again.");
}

int32_t GGPKFile::LoadRecord(int64_t offset, size_t needAtLeast) {
    // One positional read covers the header, the name and (for small directories) the
    // entries; EnsureRecordBytes reads the rest of a long record.
    const size_t want = (std::max)(needAtLeast, static_cast<size_t>(4096));
    if (m_scratch.size() < want)
        m_scratch.resize(want);
    m_scratchValid = 0;
    if (offset >= 0) {
        try {
            m_scratchValid = m_readFile.ReadAt(offset, m_scratch.data(), want);
        } catch (const std::exception&) {
            m_scratchValid = 0;
        }
    }
    return static_cast<int32_t>(m_scratchValid);
}

void GGPKFile::EnsureRecordBytes(int64_t offset, size_t size) {
    if (size <= m_scratchValid)
        return;
    if (size > (1u << 30))
        throw std::runtime_error("Corrupt GGPK record at offset " + std::to_string(offset));
    if (m_scratch.size() < size)
        m_scratch.resize(size);
    m_scratchValid += m_readFile.ReadAt(offset + static_cast<int64_t>(m_scratchValid),
                                        m_scratch.data() + m_scratchValid, size - m_scratchValid);
    if (m_scratchValid < size)
        throw std::runtime_error("Truncated GGPK record at offset " + std::to_string(offset));
}

GGPKRecordInfo GGPKFile::ReadRecordInfo(int64_t offset) {
    // A record past the end reads as length 0 / tag 0 (skipped by the walk).
    GGPKRecordInfo info{};
    info.offset = offset;
    LoadRecord(offset, 8);
    if (m_scratchValid >= 8) {
        std::memcpy(&info.length, m_scratch.data(), sizeof(int32_t));
        std::memcpy(&info.tag, m_scratch.data() + 4, sizeof(int32_t));
    }
    return info;
}

namespace {
std::string DecodeName(const uint8_t* p, int32_t chars, uint32_t version) {
    // Simplified: assume ASCII range for directory/file names (UTF-32 on v4, UTF-16LE else)
    std::string name(static_cast<size_t>(chars), '\0');
    for (int32_t i = 0; i < chars; ++i) {
        if (version == 4) {
            uint32_t c;
            std::memcpy(&c, p + static_cast<size_t>(i) * 4, 4);
            name[static_cast<size_t>(i)] = static_cast<char>(c);
        } else {
            uint16_t c;
            std::memcpy(&c, p + static_cast<size_t>(i) * 2, 2);
            name[static_cast<size_t>(i)] = static_cast<char>(c);
        }
    }
    return name;
}
} // namespace

void GGPKFile::ReadDirectoryRecord(int64_t offset, GGPKTreeNode& node) {
    LoadRecord(offset, 48);
    ParseDirectoryRecord(offset, node);
}

void GGPKFile::ParseDirectoryRecord(int64_t offset, GGPKTreeNode& node) {
    // int32 length, tag, nameLength (incl. null), entryCount, SHA-256[32], name, entries
    int32_t length = 0, tag = 0, nameLength = 0, entryCount = 0;
    if (m_scratchValid >= 16) {
        std::memcpy(&length, m_scratch.data(), 4);
        std::memcpy(&tag, m_scratch.data() + 4, 4);
        std::memcpy(&nameLength, m_scratch.data() + 8, 4);
        std::memcpy(&entryCount, m_scratch.data() + 12, 4);
    }
    if (tag != RecordTag::PDIR)
        throw std::runtime_error("Expected PDIR record at offset " + std::to_string(offset));
    if (nameLength < 1 || entryCount < 0)
        throw std::runtime_error("Corrupt GGPK directory record at offset " + std::to_string(offset));

    node.offset = offset;
    node.length = length;
    node.isDirectory = true;

    const int64_t charSize = (m_version == 4) ? 4 : 2;
    const int64_t nameBytes = static_cast<int64_t>(nameLength) * charSize;
    const int64_t total = 48 + nameBytes + static_cast<int64_t>(entryCount) * 12;
    EnsureRecordBytes(offset, static_cast<size_t>(total));

    std::memcpy(node.hash.data(), m_scratch.data() + 16, 32);
    node.name = DecodeName(m_scratch.data() + 48, nameLength - 1, m_version);

    const uint8_t* entries = m_scratch.data() + 48 + nameBytes;
    node.entries.resize(static_cast<size_t>(entryCount));
    for (int32_t i = 0; i < entryCount; ++i) {
        std::memcpy(&node.entries[static_cast<size_t>(i)].nameHash, entries + static_cast<size_t>(i) * 12, sizeof(uint32_t));
        std::memcpy(&node.entries[static_cast<size_t>(i)].offset, entries + static_cast<size_t>(i) * 12 + 4, sizeof(int64_t));
    }
}

void GGPKFile::ReadFileRecord(int64_t offset, GGPKTreeNode& node) {
    LoadRecord(offset, 44);
    ParseFileRecord(offset, node);
}

void GGPKFile::ParseFileRecord(int64_t offset, GGPKTreeNode& node) {
    // int32 length, tag, nameLength (incl. null), SHA-256[32], name, data
    int32_t length = 0, tag = 0, nameLength = 0;
    if (m_scratchValid >= 12) {
        std::memcpy(&length, m_scratch.data(), 4);
        std::memcpy(&tag, m_scratch.data() + 4, 4);
        std::memcpy(&nameLength, m_scratch.data() + 8, 4);
    }
    if (tag != RecordTag::FILE)
        throw std::runtime_error("Expected FILE record at offset " + std::to_string(offset));
    if (nameLength < 1)
        throw std::runtime_error("Corrupt GGPK file record at offset " + std::to_string(offset));

    node.offset = offset;
    node.length = length;
    node.isDirectory = false;

    const int64_t charSize = (m_version == 4) ? 4 : 2;
    const int64_t headerBytes = 44 + static_cast<int64_t>(nameLength) * charSize;
    EnsureRecordBytes(offset, static_cast<size_t>(headerBytes));

    std::memcpy(node.hash.data(), m_scratch.data() + 12, 32);
    node.name = DecodeName(m_scratch.data() + 44, nameLength - 1, m_version);

    // Compute data offset and length
    node.dataOffset = offset + headerBytes;
    node.dataLength = length - static_cast<int32_t>(headerBytes);
}

void GGPKFile::ReadTree(GGPKTreeNode& dirNode) {
    for (auto& entry : dirNode.entries) {
        auto info = ReadRecordInfo(entry.offset);   // one read: the record is now in m_scratch

        if (info.tag == RecordTag::PDIR) {
            auto child = std::make_unique<GGPKTreeNode>();
            ParseDirectoryRecord(entry.offset, *child);
            child->parent = &dirNode;
            ReadTree(*child); // Recurse
            dirNode.children[child->name] = std::move(child);
        } else if (info.tag == RecordTag::FILE) {
            auto child = std::make_unique<GGPKTreeNode>();
            ParseFileRecord(entry.offset, *child);
            child->parent = &dirNode;
            dirNode.children[child->name] = std::move(child);
        }
        // Skip FREE records
    }
}

std::vector<uint8_t> GGPKFile::ReadAt(int64_t offset, int32_t size) {
    if (size <= 0) return {};

    std::vector<uint8_t> buf(static_cast<size_t>(size));
    try {
        buf.resize(m_readFile.ReadAt(offset, buf.data(), buf.size()));
    } catch (const std::exception&) {
        return {};
    }
    return buf;
}

void GGPKFile::ReadExact(int64_t offset, void* buffer, size_t size, const std::string& what) const {
    m_readFile.ReadExact(offset, buffer, size, what);
}

std::vector<uint8_t> GGPKFile::ReadFileContent(const GGPKTreeNode* node) {
    if (!node || node->isDirectory || node->dataLength <= 0)
        return {};

    return ReadAt(node->dataOffset, node->dataLength);
}

void GGPKFile::WriteFileContent(GGPKTreeNode* node, const uint8_t* data, int32_t size) {
    if (!node || node->isDirectory)
        throw std::runtime_error("Cannot write to a non-file node");
    ThrowIfReadOnly("GGPK write");

    if (size != node->dataLength)
        throw std::runtime_error("GGPK in-place write requires same size data (got " +
                                  std::to_string(size) + ", expected " + std::to_string(node->dataLength) + ")");

    m_stream.seekp(static_cast<std::streamoff>(node->dataOffset));
    m_stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));

    // Compute and write SHA-256 hash for the file record (at offset + 12)
    ComputeSHA256(data, static_cast<size_t>(size), node->hash.data());
    WriteNodeHash(node);

    m_stream.flush();
}

void GGPKFile::ReplaceFileContent(GGPKTreeNode* node, const uint8_t* data, int32_t size,
                                   GGPKTreeNode* parentDir) {
    if (!node || node->isDirectory)
        throw std::runtime_error("Cannot write to a non-file node");
    ThrowIfReadOnly("GGPK write");

    std::lock_guard<std::mutex> lock(m_streamMutex);

    if (size <= node->dataLength) {
        // In-place write — data fits in existing slot
        m_stream.seekp(static_cast<std::streamoff>(node->dataOffset));
        m_stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));

        // Compute and write SHA-256 hash for the file record
        ComputeSHA256(data, static_cast<size_t>(size), node->hash.data());
        WriteNodeHash(node);
        if (node->parent) m_dirtyHashes.insert(node->parent);

        m_stream.flush();
        if (!m_stream.good())
            throw std::runtime_error("GGPK in-place write failed for: " + node->name);
        return;
    }

    // === Relocation: append new FILE record at end of file ===

    // 1. Determine end of file
    m_stream.seekg(0, std::ios::end);
    int64_t newOffset = static_cast<int64_t>(m_stream.tellg());

    // 2. Build record header
    int32_t charSize = (m_version == 4) ? static_cast<int32_t>(sizeof(uint32_t))
                                        : static_cast<int32_t>(sizeof(uint16_t));
    int32_t nameChars = static_cast<int32_t>(node->name.size()) + 1; // include null terminator
    int32_t nameBytes = nameChars * charSize;
    int32_t headerOverhead = 8 + 4 + 32 + nameBytes; // length+tag + nameLength + hash + name
    int32_t recordLength = headerOverhead + size;

    int32_t tag = RecordTag::FILE;
    int32_t nameLengthField = nameChars;

    // Compute SHA-256 hash of the new content
    ComputeSHA256(data, static_cast<size_t>(size), node->hash.data());

    m_stream.seekp(static_cast<std::streamoff>(newOffset));
    m_stream.write(reinterpret_cast<const char*>(&recordLength), sizeof(int32_t));
    m_stream.write(reinterpret_cast<const char*>(&tag), sizeof(int32_t));
    m_stream.write(reinterpret_cast<const char*>(&nameLengthField), sizeof(int32_t));

    // Write computed SHA-256 hash
    m_stream.write(reinterpret_cast<const char*>(node->hash.data()), 32);

    // Write name in UTF-16LE (v3) or UTF-32 (v4)
    if (m_version == 4) {
        for (char c : node->name) {
            uint32_t wc = static_cast<uint32_t>(static_cast<uint8_t>(c));
            m_stream.write(reinterpret_cast<const char*>(&wc), sizeof(uint32_t));
        }
        uint32_t null32 = 0;
        m_stream.write(reinterpret_cast<const char*>(&null32), sizeof(uint32_t));
    } else {
        for (char c : node->name) {
            uint16_t wc = static_cast<uint16_t>(static_cast<uint8_t>(c));
            m_stream.write(reinterpret_cast<const char*>(&wc), sizeof(uint16_t));
        }
        uint16_t null16 = 0;
        m_stream.write(reinterpret_cast<const char*>(&null16), sizeof(uint16_t));
    }

    // Write data
    m_stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    m_stream.flush();

    if (!m_stream.good())
        throw std::runtime_error("GGPK relocation write failed for: " + node->name);

    // 3. Mark old record as FREE
    int64_t oldOffset = node->offset;
    int32_t oldLength = node->length;
    int32_t freeTag = RecordTag::FREE;
    int64_t nextFreeOffset = 0;
    m_stream.seekp(static_cast<std::streamoff>(oldOffset));
    m_stream.write(reinterpret_cast<const char*>(&oldLength), sizeof(int32_t));
    m_stream.write(reinterpret_cast<const char*>(&freeTag), sizeof(int32_t));
    m_stream.write(reinterpret_cast<const char*>(&nextFreeOffset), sizeof(int64_t));
    m_stream.flush();

    // 4. Update node metadata
    int64_t oldNodeOffset = node->offset;
    node->offset = newOffset;
    node->length = recordLength;
    node->dataOffset = newOffset + headerOverhead;
    node->dataLength = size;

    // 5. Update parent directory entry to point to new offset
    if (parentDir) {
        for (size_t entryIdx = 0; entryIdx < parentDir->entries.size(); ++entryIdx) {
            if (parentDir->entries[entryIdx].offset == oldNodeOffset) {
                parentDir->entries[entryIdx].offset = newOffset;

                int32_t parentCharSize = (m_version == 4) ? static_cast<int32_t>(sizeof(uint32_t))
                                                          : static_cast<int32_t>(sizeof(uint16_t));
                m_stream.seekg(static_cast<std::streamoff>(parentDir->offset + 8));
                int32_t parentNameLen;
                m_stream.read(reinterpret_cast<char*>(&parentNameLen), sizeof(int32_t));

                int64_t entriesStart = parentDir->offset + 8 + 4 + 4 + 32
                                       + static_cast<int64_t>(parentNameLen) * parentCharSize;
                int64_t entryFilePos = entriesStart
                    + static_cast<int64_t>(entryIdx) * (sizeof(uint32_t) + sizeof(int64_t))
                    + sizeof(uint32_t);

                m_stream.seekp(static_cast<std::streamoff>(entryFilePos));
                m_stream.write(reinterpret_cast<const char*>(&newOffset), sizeof(int64_t));
                m_stream.flush();

                if (!m_stream.good())
                    throw std::runtime_error("GGPK parent directory entry update failed");
                break;
            }
        }

        // Mark parent directory as dirty for hash cascade
        m_dirtyHashes.insert(parentDir);
    } else if (node->parent) {
        // No explicit parentDir — use node's tracked parent
        m_dirtyHashes.insert(node->parent);
    }
}

// ============================================================================
// Hash chain management (matches LibGGPK3 approach)
// ============================================================================

bool GGPKFile::ComputeSHA256(const uint8_t* data, size_t size, uint8_t outHash[32]) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    bool ok = false;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) == 0) {
            if (BCryptHashData(hHash, const_cast<PUCHAR>(data),
                               static_cast<ULONG>(size), 0) == 0) {
                if (BCryptFinishHash(hHash, outHash, 32, 0) == 0) {
                    ok = true;
                }
            }
            BCryptDestroyHash(hHash);
        }
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }
    return ok;
}

void GGPKFile::WriteNodeHash(GGPKTreeNode* node) {
    // FILE records: hash at offset + 12 (after length + tag + nameLength)
    // PDIR records: hash at offset + 16 (after length + tag + nameLength + entryCount)
    int64_t hashOffset = node->isDirectory
        ? (node->offset + 16)
        : (node->offset + 12);

    m_stream.seekp(static_cast<std::streamoff>(hashOffset));
    m_stream.write(reinterpret_cast<const char*>(node->hash.data()), 32);
}

void GGPKFile::RenewDirectoryHash(GGPKTreeNode* dirNode) {
    if (!dirNode || !dirNode->isDirectory) return;

    // Concatenate children hashes in entry order (matching LibGGPK3 RenewHash)
    std::vector<uint8_t> combined;
    combined.reserve(dirNode->entries.size() * 32);

    for (const auto& entry : dirNode->entries) {
        // Find child node matching this entry's offset
        bool found = false;
        for (const auto& [name, child] : dirNode->children) {
            if (child->offset == entry.offset) {
                combined.insert(combined.end(), child->hash.begin(), child->hash.end());
                found = true;
                break;
            }
        }
        if (!found) {
            // Entry points to a FREE or unloaded record — can't compute accurate hash,
            // append zeros to maintain correct concatenation length
            combined.resize(combined.size() + 32, 0);
        }
    }

    // SHA256(child[0].hash || child[1].hash || ... || child[n].hash)
    ComputeSHA256(combined.data(), combined.size(), dirNode->hash.data());
    WriteNodeHash(dirNode);
}

void GGPKFile::RenewHashes(bool forceRenewRoot) {
    ThrowIfReadOnly("GGPK hash update");
    std::lock_guard<std::mutex> lock(m_streamMutex);

    m_dirtyHashes.erase(nullptr);

    // Cascade: process dirty directories, adding parents until stable.
    // By default, Root and Root's direct children are NOT renewed —
    // this preserves the official hash chain so the game won't re-patch.
    size_t prevCount = 0;
    while (m_dirtyHashes.size() != prevCount) {
        prevCount = m_dirtyHashes.size();

        auto snapshot = std::vector<GGPKTreeNode*>(m_dirtyHashes.begin(), m_dirtyHashes.end());
        for (auto* dr : snapshot) {
            if (forceRenewRoot || (dr != m_root.get() && dr->parent != m_root.get())) {
                RenewDirectoryHash(dr);
                m_dirtyHashes.erase(dr);
                if (dr->parent) {
                    m_dirtyHashes.insert(dr->parent);
                }
            }
        }

        m_dirtyHashes.erase(nullptr);
    }

    m_stream.flush();

    // Clear remaining un-processable entries (Root / Root's children)
    m_dirtyHashes.clear();
}

} // namespace GGPK
