#include "GGPK.h"
#include "Bundle.h" // DescribeLastOpenError
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <Windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

namespace GGPK {

GGPKFile::GGPKFile(const std::string& filePath)
    : m_filePath(filePath) {
    // Content.ggpk genuinely needs read-write access (writes happen in-place),
    // so no read-only fallback here — but report WHY the open failed.
    m_stream.open(std::filesystem::path(filePath), std::ios::in | std::ios::out | std::ios::binary);
    if (!m_stream.is_open())
        throw std::runtime_error("Failed to open GGPK file: " + filePath
                                 + " (" + DescribeLastOpenError() + ")");

    // Read GGPKRecord at offset 0
    auto ggpkInfo = ReadRecordInfo(0);
    if (ggpkInfo.tag != RecordTag::GGPK)
        throw std::runtime_error("Invalid GGPK file: first record is not GGPKRecord");

    // GGPKRecord data: uint32 version, int64 rootDirOffset, int64 firstFreeOffset
    m_stream.seekg(8); // After length + tag
    int32_t versionInt;
    m_stream.read(reinterpret_cast<char*>(&versionInt), sizeof(int32_t));
    m_version = static_cast<uint32_t>(versionInt);

    m_stream.read(reinterpret_cast<char*>(&m_rootOffset), sizeof(int64_t));
    m_stream.read(reinterpret_cast<char*>(&m_firstFreeOffset), sizeof(int64_t));

    // Read root directory and build tree
    m_root = std::make_unique<GGPKTreeNode>();
    ReadDirectoryRecord(m_rootOffset, *m_root);
    ReadTree(*m_root);
}

GGPKFile::~GGPKFile() {
    if (m_stream.is_open())
        m_stream.close();
}

GGPKRecordInfo GGPKFile::ReadRecordInfo(int64_t offset) {
    GGPKRecordInfo info{};
    info.offset = offset;
    m_stream.seekg(static_cast<std::streamoff>(offset));

    m_stream.read(reinterpret_cast<char*>(&info.length), sizeof(int32_t));
    m_stream.read(reinterpret_cast<char*>(&info.tag), sizeof(int32_t));

    return info;
}

void GGPKFile::ReadDirectoryRecord(int64_t offset, GGPKTreeNode& node) {
    m_stream.seekg(static_cast<std::streamoff>(offset));

    int32_t length, tag;
    m_stream.read(reinterpret_cast<char*>(&length), sizeof(int32_t));
    m_stream.read(reinterpret_cast<char*>(&tag), sizeof(int32_t));

    if (tag != RecordTag::PDIR)
        throw std::runtime_error("Expected PDIR record at offset " + std::to_string(offset));

    node.offset = offset;
    node.length = length;
    node.isDirectory = true;

    int32_t nameLength; // Includes null terminator
    m_stream.read(reinterpret_cast<char*>(&nameLength), sizeof(int32_t));
    nameLength -= 1; // Exclude null

    int32_t entryCount;
    m_stream.read(reinterpret_cast<char*>(&entryCount), sizeof(int32_t));

    // Read SHA-256 hash (32 bytes)
    m_stream.read(reinterpret_cast<char*>(node.hash.data()), 32);

    // Read name (UTF-16 on PC/v3)
    if (m_version == 4) {
        // UTF-32
        std::vector<uint32_t> nameChars(static_cast<size_t>(nameLength));
        m_stream.read(reinterpret_cast<char*>(nameChars.data()),
                      static_cast<std::streamsize>(nameLength) * sizeof(uint32_t));
        // Simplified: assume ASCII range
        node.name.resize(static_cast<size_t>(nameLength));
        for (size_t i = 0; i < static_cast<size_t>(nameLength); ++i)
            node.name[i] = static_cast<char>(nameChars[i]);
        m_stream.seekg(sizeof(uint32_t), std::ios::cur); // Null terminator
    } else {
        // UTF-16LE
        std::vector<uint16_t> nameChars(static_cast<size_t>(nameLength));
        m_stream.read(reinterpret_cast<char*>(nameChars.data()),
                      static_cast<std::streamsize>(nameLength) * sizeof(uint16_t));
        // Simplified: assume ASCII range for directory/file names
        node.name.resize(static_cast<size_t>(nameLength));
        for (size_t i = 0; i < static_cast<size_t>(nameLength); ++i)
            node.name[i] = static_cast<char>(nameChars[i]);
        m_stream.seekg(sizeof(uint16_t), std::ios::cur); // Null terminator
    }

    // Read entries
    node.entries.resize(static_cast<size_t>(entryCount));
    for (int32_t i = 0; i < entryCount; ++i) {
        m_stream.read(reinterpret_cast<char*>(&node.entries[static_cast<size_t>(i)].nameHash),
                      sizeof(uint32_t));
        m_stream.read(reinterpret_cast<char*>(&node.entries[static_cast<size_t>(i)].offset),
                      sizeof(int64_t));
    }
}

void GGPKFile::ReadFileRecord(int64_t offset, GGPKTreeNode& node) {
    m_stream.seekg(static_cast<std::streamoff>(offset));

    int32_t length, tag;
    m_stream.read(reinterpret_cast<char*>(&length), sizeof(int32_t));
    m_stream.read(reinterpret_cast<char*>(&tag), sizeof(int32_t));

    if (tag != RecordTag::FILE)
        throw std::runtime_error("Expected FILE record at offset " + std::to_string(offset));

    node.offset = offset;
    node.length = length;
    node.isDirectory = false;

    int32_t nameLength;
    m_stream.read(reinterpret_cast<char*>(&nameLength), sizeof(int32_t));
    nameLength -= 1; // Exclude null

    // Read SHA-256 hash (32 bytes)
    m_stream.read(reinterpret_cast<char*>(node.hash.data()), 32);

    // Read name (UTF-16 on PC/v3)
    if (m_version == 4) {
        std::vector<uint32_t> nameChars(static_cast<size_t>(nameLength));
        m_stream.read(reinterpret_cast<char*>(nameChars.data()),
                      static_cast<std::streamsize>(nameLength) * sizeof(uint32_t));
        node.name.resize(static_cast<size_t>(nameLength));
        for (size_t i = 0; i < static_cast<size_t>(nameLength); ++i)
            node.name[i] = static_cast<char>(nameChars[i]);
        m_stream.seekg(sizeof(uint32_t), std::ios::cur);
    } else {
        std::vector<uint16_t> nameChars(static_cast<size_t>(nameLength));
        m_stream.read(reinterpret_cast<char*>(nameChars.data()),
                      static_cast<std::streamsize>(nameLength) * sizeof(uint16_t));
        node.name.resize(static_cast<size_t>(nameLength));
        for (size_t i = 0; i < static_cast<size_t>(nameLength); ++i)
            node.name[i] = static_cast<char>(nameChars[i]);
        m_stream.seekg(sizeof(uint16_t), std::ios::cur);
    }

    // Compute data offset and length
    auto currentPos = m_stream.tellg();
    node.dataOffset = static_cast<int64_t>(currentPos);
    node.dataLength = length - static_cast<int32_t>(currentPos - offset);
}

void GGPKFile::ReadTree(GGPKTreeNode& dirNode) {
    for (auto& entry : dirNode.entries) {
        auto info = ReadRecordInfo(entry.offset);

        if (info.tag == RecordTag::PDIR) {
            auto child = std::make_unique<GGPKTreeNode>();
            ReadDirectoryRecord(entry.offset, *child);
            child->parent = &dirNode;
            ReadTree(*child); // Recurse
            dirNode.children[child->name] = std::move(child);
        } else if (info.tag == RecordTag::FILE) {
            auto child = std::make_unique<GGPKTreeNode>();
            ReadFileRecord(entry.offset, *child);
            child->parent = &dirNode;
            dirNode.children[child->name] = std::move(child);
        }
        // Skip FREE records
    }
}

std::vector<uint8_t> GGPKFile::ReadAt(int64_t offset, int32_t size) {
    if (size <= 0) return {};

    std::lock_guard<std::mutex> lock(m_streamMutex);
    m_stream.seekg(static_cast<std::streamoff>(offset));
    if (!m_stream.good()) return {};
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    m_stream.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
    if (!m_stream.good()) {
        buf.resize(static_cast<size_t>(m_stream.gcount()));
    }
    return buf;
}

std::vector<uint8_t> GGPKFile::ReadFileContent(const GGPKTreeNode* node) {
    if (!node || node->isDirectory || node->dataLength <= 0)
        return {};

    return ReadAt(node->dataOffset, node->dataLength);
}

void GGPKFile::WriteFileContent(GGPKTreeNode* node, const uint8_t* data, int32_t size) {
    if (!node || node->isDirectory)
        throw std::runtime_error("Cannot write to a non-file node");

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
