#pragma once
#include "GGPK.h"
#include "GGPKBundleFactory.h"
#include "Index.h"
#include <memory>
#include <stdexcept>
#include <string>

namespace GGPK {

/// Combines GGPK (Content.ggpk) with Index (Bundles2/_.index.bin).
/// Entry point for standalone client patching.
class BundledGGPK {
public:
    /// Open a Content.ggpk file and parse the internal bundle index.
    explicit BundledGGPK(const std::string& filePath, bool parsePaths = true) {
        m_ggpk = std::make_unique<GGPKFile>(filePath);

        auto* root = m_ggpk->GetRoot();
        auto* bundles2 = root->FindChild("Bundles2");
        if (!bundles2 || !bundles2->isDirectory)
            throw std::runtime_error("Cannot find directory 'Bundles2' in GGPK");

        auto* indexNode = bundles2->FindChild("_.index.bin");
        if (!indexNode || indexNode->isDirectory)
            throw std::runtime_error("Cannot find file 'Bundles2/_.index.bin' in GGPK");

        // Read the _.index.bin bundle data from the GGPK
        auto indexData = m_ggpk->ReadFileContent(indexNode);

        // Create factory for reading bundles from inside the GGPK
        auto factory = std::make_unique<GGPKBundleFactory>(m_ggpk.get(), bundles2);

        // Create the Index from the bundle data
        m_index = std::make_unique<Index>(std::move(indexData), parsePaths, std::move(factory));

        m_indexNode = indexNode;
    }

    ~BundledGGPK() = default;

    BundledGGPK(const BundledGGPK&) = delete;
    BundledGGPK& operator=(const BundledGGPK&) = delete;

    /// Get the GGPK file handler
    GGPKFile* GetGGPK() { return m_ggpk.get(); }

    /// Get the bundle index
    Index* GetIndex() { return m_index.get(); }

    /// Save the index back into the GGPK.
    /// Must be called after modifying files through the Index.
    /// After writing, renews the SHA-256 hash chain while preserving Root hash
    /// (prevents the game from detecting modifications and triggering re-patching).
    void SaveIndex() {
        m_index->Save();

        auto* baseBundle = m_index->GetBaseBundle();
        if (baseBundle && baseBundle->IsMemoryBased()) {
            const auto& rawData = baseBundle->GetRawMemoryData();
            if (!rawData.empty()) {
                auto* root = m_ggpk->GetRoot();
                auto* bundles2 = root->FindChild("Bundles2");
                m_ggpk->ReplaceFileContent(m_indexNode,
                                            rawData.data(),
                                            static_cast<int32_t>(rawData.size()),
                                            bundles2);
            }
        }

        // Cascade directory hashes but preserve Root hash (stealth mode)
        m_ggpk->RenewHashes(/*forceRenewRoot=*/false);
    }

private:
    std::unique_ptr<GGPKFile> m_ggpk;
    std::unique_ptr<Index> m_index;
    GGPKTreeNode* m_indexNode = nullptr;
};

} // namespace GGPK
