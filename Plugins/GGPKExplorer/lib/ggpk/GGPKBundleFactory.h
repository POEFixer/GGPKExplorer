#pragma once
#include "IBundleFactory.h"
#include "GGPK.h"
#include "Bundle.h"
#include "BundleRecord.h"
#include <filesystem>
#include <list>
#include <stdexcept>
#include <unordered_map>

namespace GGPK {

/// Factory that reads bundles from inside a Content.ggpk file.
/// Uses sub-stream mode (no full memory copy) + LRU node lookup cache.
class GGPKBundleFactory : public IBundleFactory {
public:
    GGPKBundleFactory(GGPKFile* ggpk, GGPKTreeNode* bundles2Dir)
        : m_ggpk(ggpk), m_bundles2(bundles2Dir) {}

    std::unique_ptr<Bundle> GetBundle(BundleRecord* record) override {
        // Custom bundles (LibGGPK3/*) are stored on disk alongside Content.ggpk,
        // not inside the GGPK file. Check disk first for these.
        std::string basePath = std::filesystem::path(m_ggpk->GetFilePath()).parent_path().string();
        std::string diskPath = basePath + "/Bundles2/" + record->GetPath();
        if (std::filesystem::exists(diskPath)) {
            return std::make_unique<Bundle>(diskPath, record);
        }

        // Bundle is inside the GGPK — use sub-stream mode
        int64_t dataOffset;
        int32_t dataLength;

        // Check cache first (avoids FindByPath tree traversal)
        auto it = m_cacheMap.find(record);
        if (it != m_cacheMap.end()) {
            m_cacheOrder.splice(m_cacheOrder.begin(), m_cacheOrder, it->second);
            dataOffset = it->second->second.dataOffset;
            dataLength = it->second->second.dataLength;
        } else {
            auto* node = m_bundles2->FindByPath(record->GetPath());
            if (!node || node->isDirectory)
                throw std::runtime_error("Cannot find bundle: Bundles2/"
                                         + record->GetPath() + " in GGPK");

            dataOffset = node->dataOffset;
            dataLength = node->dataLength;

            if (m_cacheOrder.size() >= MAX_CACHE_SIZE) {
                auto last = std::prev(m_cacheOrder.end());
                m_cacheMap.erase(last->first);
                m_cacheOrder.erase(last);
            }

            CachedNodeInfo info{dataOffset, dataLength};
            m_cacheOrder.emplace_front(record, info);
            m_cacheMap[record] = m_cacheOrder.begin();
        }

        return std::make_unique<Bundle>(
            m_ggpk->GetStream(),
            m_ggpk->GetStreamMutex(),
            dataOffset,
            dataLength,
            record);
    }

    std::fstream CreateBundle(const std::string& bundlePath, std::string& outFullPath) override {
        // For GGPK-based bundles, custom bundles go to disk alongside Content.ggpk
        std::string basePath = std::filesystem::path(m_ggpk->GetFilePath()).parent_path().string();
        std::string fullPath = basePath + "/Bundles2/" + bundlePath;
        std::filesystem::create_directories(std::filesystem::path(fullPath).parent_path());
        std::fstream fs(fullPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        if (!fs.is_open())
            throw std::runtime_error("Failed to create bundle: " + fullPath
                                     + " (" + DescribeLastOpenError() + ")");
        outFullPath = fullPath;
        return fs;
    }

    bool DeleteBundle(const std::string& bundlePath) override {
        // Best-effort: never throw (see DriveBundleFactory::DeleteBundle).
        std::error_code ec;
        std::string basePath = std::filesystem::path(m_ggpk->GetFilePath()).parent_path().string();
        std::string fullPath = basePath + "/Bundles2/" + bundlePath;
        if (!std::filesystem::exists(fullPath, ec))
            return false;
        if (!std::filesystem::remove(fullPath, ec) || ec)
            return false;
        auto parentDir = std::filesystem::path(fullPath).parent_path();
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

    using CacheEntry = std::pair<BundleRecord*, CachedNodeInfo>;
    std::list<CacheEntry> m_cacheOrder;
    std::unordered_map<BundleRecord*, std::list<CacheEntry>::iterator> m_cacheMap;

    GGPKFile* m_ggpk;
    GGPKTreeNode* m_bundles2;
};

} // namespace GGPK
