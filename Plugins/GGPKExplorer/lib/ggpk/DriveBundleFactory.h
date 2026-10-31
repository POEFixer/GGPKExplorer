#pragma once
#include "IBundleFactory.h"
#include "Bundle.h"
#include "BundleRecord.h"
#include <filesystem>
#include <string>

namespace GGPK {

/// Factory for bundles stored on disk (Steam/Epic installation).
/// The base directory is the "Bundles2" folder containing _.index.bin (UTF-8 path).
class DriveBundleFactory : public IBundleFactory {
public:
    explicit DriveBundleFactory(const std::string& baseDirectory) {
        m_baseDir = PathToUtf8(Utf8Path(baseDirectory).make_preferred());
        if (!m_baseDir.empty() && m_baseDir.back() != '\\' && m_baseDir.back() != '/')
            m_baseDir += '\\';
    }

    std::unique_ptr<Bundle> GetBundle(BundleRecord* record) override {
        std::string path = m_baseDir + record->GetPath();
        return std::make_unique<Bundle>(path, record);
    }

    std::unique_ptr<Bundle> OpenForRead(BundleRecord* record) override {
        return std::make_unique<Bundle>(m_baseDir + record->GetPath(), nullptr, OpenMode::ReadOnly);
    }

    std::fstream CreateBundle(const std::string& bundlePath, std::string& outFullPath) override {
        std::string fullPath = m_baseDir + bundlePath;
        std::filesystem::create_directories(Utf8Path(fullPath).parent_path());
        std::fstream fs(Utf8Path(fullPath), std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        if (!fs.is_open())
            throw std::runtime_error("Failed to create bundle: " + fullPath
                                     + " (" + DescribeLastOpenError() + ")");
        outFullPath = fullPath;
        return fs;
    }

    bool DeleteBundle(const std::string& bundlePath) override {
        // Best-effort: deletion may fail when the running game still holds the
        // file. A leftover orphan bundle is harmless (the index no longer
        // references it), so never throw from here — use error_code overloads.
        std::error_code ec;
        std::filesystem::path fullPath;
        try {
            fullPath = Utf8Path(m_baseDir + bundlePath);
        } catch (...) {
            return false;
        }
        if (!std::filesystem::exists(fullPath, ec))
            return false;
        if (!std::filesystem::remove(fullPath, ec) || ec)
            return false;

        // Remove parent directory if empty
        auto parentDir = fullPath.parent_path();
        if (std::filesystem::is_empty(parentDir, ec) && !ec)
            std::filesystem::remove(parentDir, ec);

        return true;
    }

    const std::string& GetBaseDirectory() const { return m_baseDir; }

private:
    std::string m_baseDir;
};

} // namespace GGPK
