#pragma once
#include "IBundleFactory.h"
#include "GGPK.h"
#include "Bundle.h"
#include "BundleRecord.h"
#include <filesystem>
#include <list>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace GGPK {

/// Factory that reads bundles from inside a Content.ggpk file.
/// Uses sub-stream mode (no full memory copy) + LRU node lookup cache. Thread-safe: the
/// node LRU and the on-disk cache are guarded.
class GGPKBundleFactory : public IBundleFactory {
public:
    GGPKBundleFactory(GGPKFile* ggpk, GGPKTreeNode* bundles2Dir)
        : m_ggpk(ggpk), m_bundles2(bundles2Dir) {
        // Custom bundles (LibGGPK3/*) are stored on disk alongside Content.ggpk.
        m_diskBundles2 = PathToUtf8(Utf8Path(m_ggpk->GetFilePath()).parent_path()) + "/Bundles2/";
    }

    /// Write path: looks at the disk afresh on every call (read-write first).
    std::unique_ptr<Bundle> GetBundle(BundleRecord* record) override {
        // Custom bundles (LibGGPK3/*) are stored on disk alongside Content.ggpk,
        // not inside the GGPK file. Check disk first for these.
        std::string diskPath = m_diskBundles2 + record->GetPath();
        if (std::filesystem::exists(Utf8Path(diskPath))) {
            return std::make_unique<Bundle>(diskPath, record);
        }
        const auto location = Locate(record);
        return std::make_unique<Bundle>(*m_ggpk, location.dataOffset, location.dataLength, record);
    }

    /// Read path: whether a bundle exists on disk is looked up once per record (forgotten
    /// whenever this factory creates or deletes a bundle file).
    std::unique_ptr<Bundle> OpenForRead(BundleRecord* record) override {
        std::string diskPath = m_diskBundles2 + record->GetPath();
        if (IsOnDisk(record, diskPath))
            return std::make_unique<Bundle>(diskPath, nullptr, OpenMode::ReadOnly);
        const auto location = Locate(record);
        return std::make_unique<Bundle>(*m_ggpk, location.dataOffset, location.dataLength, nullptr);
    }

    std::fstream CreateBundle(const std::string& bundlePath, std::string& outFullPath) override {
        // For GGPK-based bundles, custom bundles go to disk alongside Content.ggpk
        std::string fullPath = m_diskBundles2 + bundlePath;
        std::filesystem::create_directories(Utf8Path(fullPath).parent_path());
        std::fstream fs(Utf8Path(fullPath), std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        ForgetDisk();
        if (!fs.is_open())
            throw std::runtime_error("Failed to create bundle: " + fullPath
                                     + " (" + DescribeLastOpenError() + ")");
        outFullPath = fullPath;
        return fs;
    }

    bool DeleteBundle(const std::string& bundlePath) override {
        // Best-effort: never throw (see DriveBundleFactory::DeleteBundle).
        ForgetDisk();
        std::error_code ec;
        std::filesystem::path fullPath;
        try {
            fullPath = Utf8Path(m_diskBundles2 + bundlePath);
        } catch (...) {
            return false;
        }
        if (!std::filesystem::exists(fullPath, ec))
            return false;
        if (!std::filesystem::remove(fullPath, ec) || ec)
            return false;
        auto parentDir = fullPath.parent_path();
        if (std::filesystem::is_empty(parentDir, ec) && !ec)
            std::filesystem::remove(parentDir, ec);
        return true;
    }

private:
    static constexpr size_t MAX_CACHE_SIZE = 32;

    struct CachedNodeInfo {
        int64_t dataOffset;
        int32_t dataLength;
    };

    /// Where the record's bundle lies inside Content.ggpk (LRU of node lookups).
    CachedNodeInfo Locate(BundleRecord* record) {
        // Readers on several threads share this factory: the LRU list and map are guarded.
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto it = m_cacheMap.find(record);
        if (it != m_cacheMap.end()) {
            m_cacheOrder.splice(m_cacheOrder.begin(), m_cacheOrder, it->second);
            return it->second->second;
        }
        auto* node = m_bundles2->FindByPath(record->GetPath());
        if (!node || node->isDirectory)
            throw std::runtime_error("Cannot find bundle: Bundles2/"
                                     + record->GetPath() + " in GGPK");

        if (m_cacheOrder.size() >= MAX_CACHE_SIZE) {
            auto last = std::prev(m_cacheOrder.end());
            m_cacheMap.erase(last->first);
            m_cacheOrder.erase(last);
        }

        CachedNodeInfo info{node->dataOffset, node->dataLength};
        m_cacheOrder.emplace_front(record, info);
        m_cacheMap[record] = m_cacheOrder.begin();
        return info;
    }

    bool IsOnDisk(BundleRecord* record, const std::string& diskPath) {
        uint64_t generation;
        {
            std::lock_guard<std::mutex> lock(m_diskMutex);
            auto it = m_onDisk.find(record);
            if (it != m_onDisk.end())
                return it->second;
            generation = m_diskGeneration;
        }
        std::error_code ec;
        const bool exists = std::filesystem::exists(Utf8Path(diskPath), ec) && !ec;
        std::lock_guard<std::mutex> lock(m_diskMutex);
        if (generation == m_diskGeneration)   // no bundle file created/deleted meanwhile
            m_onDisk[record] = exists;
        return exists;
    }

    void ForgetDisk() {
        std::lock_guard<std::mutex> lock(m_diskMutex);
        m_onDisk.clear();
        ++m_diskGeneration;
    }

    using CacheEntry = std::pair<BundleRecord*, CachedNodeInfo>;
    std::mutex m_cacheMutex;
    std::list<CacheEntry> m_cacheOrder;
    std::unordered_map<BundleRecord*, std::list<CacheEntry>::iterator> m_cacheMap;

    std::mutex m_diskMutex;
    std::unordered_map<BundleRecord*, bool> m_onDisk;
    uint64_t m_diskGeneration = 0;

    GGPKFile* m_ggpk;
    GGPKTreeNode* m_bundles2;
    std::string m_diskBundles2;
};

} // namespace GGPK
