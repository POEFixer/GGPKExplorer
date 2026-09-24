#pragma once
#include "FileTypeDetector.h"
#include "GGPKManager.h"
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <functional>

namespace GGPKExp {

struct TreeEntry {
    std::string name;
    std::string fullPath;
    uint32_t depth = 0;
    uint32_t childCount = 0;
    int64_t fileSize = -1;
    FileType type = FileType::Unknown;
    bool isDirectory = false;
    bool isExpanded = false;
    bool isBookmarked = false;
    int bookmarkIcon = -1;
    uint32_t parentIndex = UINT32_MAX;
    void* nodePtr = nullptr;
};

enum class SortMode {
    Name      = 0,   // alphabetical (existing)
    Size      = 1,   // largest first (existing)
    Type      = 2,   // by FileType (existing)
    None      = 3,   // insertion order from GGPK/bundle (NEW — default for new installs)
    Bookmarks = 4    // bookmarked files first within each folder (NEW)
};

struct BookmarkEntry {
    std::string path;
    int iconIndex = 0;  // 0-9
    bool valid = true;   // set false if path not found after tree rebuild
};

class VirtualTree {
public:
    void BuildFromManager(const GGPKManager& mgr);
    void Clear();

    // Expand/collapse a directory node at the given flat index.
    void ToggleExpand(uint32_t flatIndex);
    void ExpandNode(uint32_t flatIndex);
    void CollapseNode(uint32_t flatIndex);
    void ExpandTo(const std::string& path); // Expand all ancestors of path

    // Bookmarks
    void SetBookmark(const std::string& path, int iconIndex);
    void RemoveBookmark(const std::string& path);
    bool IsBookmarked(const std::string& path) const;
    int GetBookmarkIcon(const std::string& path) const; // -1 if not bookmarked
    const std::vector<BookmarkEntry>& GetBookmarks() const { return m_bookmarks; }
    void SetBookmarks(std::vector<BookmarkEntry> bm) { m_bookmarks = std::move(bm); }
    void ValidateBookmarks();

    // Sorting
    void SetSortMode(SortMode mode);
    SortMode GetSortMode() const { return m_sortMode; }

    // Access the linearized flat list (for ImGuiListClipper).
    const std::vector<TreeEntry>& GetFlatList() const { return m_flat; }

    // Find flat index by path (returns UINT32_MAX if not found).
    uint32_t FindByPath(const std::string& path) const;

    /// Case-insensitive variant. Builds a lowercase index lazily on first call.
    uint32_t FindByPathInsensitive(const std::string& path) const;

    /// Debug: path index size (always-compiled, cheap). Returns the number of
    /// entries in the lowercase path index if it has been built, otherwise the
    /// size of the case-sensitive path index (which mirrors m_flat.size()).
    /// Used by the DatViewer "[Dev] Inspect path resolution" context menu.
    size_t GetPathIndexSize() const;

    /// Debug: enumerate up to `max` keys from the lowercase path index whose
    /// lowered form starts with the given prefix. Forces a build of the
    /// lowercase index if it hasn't been built yet (same as FindByPathInsensitive).
    /// Intended for diagnostics only — not on any hot path.
    std::vector<std::string> DebugFindKeysWithPrefix(const std::string& loweredPrefix,
                                                     size_t max) const;

    /// Collect all files under the given path recursively WITHOUT expanding nodes.
    /// Calls visitor(fullPath, name, fileSize, nodePtr) for each file.
    void ForEachFileUnder(const std::string& path,
        std::function<void(const std::string& fullPath, const std::string& name, int64_t fileSize, void* nodePtr)> visitor) const;

    /// Walk all entries (files AND directories) under the given path recursively,
    /// regardless of expansion state. Calls visitor(fullPath, name, fileSize, isDirectory, nodePtr).
    void ForEachEntryUnder(const std::string& path,
        std::function<void(const std::string& fullPath, const std::string& name, int64_t fileSize, bool isDirectory, void* nodePtr)> visitor) const;

    // Stats
    uint32_t GetFileCount() const { return m_fileCount; }
    uint32_t GetDirCount() const { return m_dirCount; }
    int64_t GetTotalSize() const { return m_totalSize; }

private:
    struct TreeNode {
        std::string name;
        std::string fullPath;
        int64_t fileSize = -1;
        FileType type = FileType::Unknown;
        bool isDirectory = false;
        bool isExpanded = false;
        void* nodePtr = nullptr;
        std::vector<std::unique_ptr<TreeNode>> children;
    };

    std::unique_ptr<TreeNode> m_rootNode;
    std::vector<TreeEntry> m_flat;
    std::unordered_map<std::string, uint32_t> m_pathIndex;
    // Lazy lowercase-key path index for case-insensitive lookup. Built on first
    // FindByPathInsensitive call; invalidated whenever m_pathIndex is rebuilt.
    mutable std::unordered_map<std::string, uint32_t> m_pathIndexLower;
    mutable bool m_pathIndexLowerBuilt = false;
    std::vector<BookmarkEntry> m_bookmarks;
    SortMode m_sortMode = SortMode::None;
    uint32_t m_fileCount = 0;
    uint32_t m_dirCount = 0;
    int64_t m_totalSize = 0;

    void BuildInternalTree(const FileEntry& src, TreeNode& dst);
    void SortChildren(TreeNode& node);
    void Relinearize();
    void LinearizeRecursive(TreeNode& node, uint32_t depth, uint32_t parentIndex);
    TreeNode* FindNode(const std::string& path);
    size_t CountVisibleDescendants(uint32_t flatIndex) const;
    void RebuildPathIndex();
    void BuildChildEntries(TreeNode& parentNode, uint32_t parentFlatIndex, uint32_t depth,
                           std::vector<TreeEntry>& out) const;
};

} // namespace GGPKExp
