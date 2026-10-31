#pragma once
#include <fstream>
#include <memory>
#include <string>

namespace GGPK {

class Bundle;
class BundleRecord;

/// Interface for creating/accessing bundle files.
/// Implementations: DriveBundleFactory (disk), GGPKBundleFactory (inside GGPK).
class IBundleFactory {
public:
    virtual ~IBundleFactory() = default;

    /// Get a Bundle instance for the given record. Caller owns the returned Bundle.
    /// This is the write path's open (read-write first, fresh header): BundleRecord's
    /// TryGetBundle and Index::GetBundleToWrite use it.
    virtual std::unique_ptr<Bundle> GetBundle(BundleRecord* record) = 0;

    /// Open a bundle for reading only: never asks for write access, may be shared by
    /// concurrent readers, never handed to the write path. The returned bundle carries no
    /// record (the caller updates the record's uncompressed size). Default: GetBundle.
    virtual std::unique_ptr<Bundle> OpenForRead(BundleRecord* record) { return GetBundle(record); }

    /// Create a new bundle file at the given relative path (ends with ".bundle.bin").
    /// Returns an open fstream for writing. Creates parent directories as needed.
    /// outFullPath receives the resolved on-disk path so the caller can attach it
    /// to the Bundle (used for write-error messages and truncation).
    virtual std::fstream CreateBundle(const std::string& bundlePath, std::string& outFullPath) = 0;

    /// Delete a bundle file. Returns true if removed, false if not found.
    virtual bool DeleteBundle(const std::string& bundlePath) = 0;
};

} // namespace GGPK
