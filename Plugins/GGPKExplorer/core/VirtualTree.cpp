#include "VirtualTree.h"
#include <algorithm>
#include <sstream>

namespace GGPKExp {

// ---------------------------------------------------------------------------
// Public: BuildFromManager
// ---------------------------------------------------------------------------

void VirtualTree::BuildFromManager(const GGPKManager& mgr) {
    Clear();

    m_rootNode = std::make_unique<TreeNode>();
    m_rootNode->name = "Root";
    m_rootNode->fullPath.clear();
    m_rootNode->isDirectory = true;
    m_rootNode->type = FileType::Directory;

    const auto& root = mgr.GetRoot();
    BuildInternalTree(root, *m_rootNode);

    SortChildren(*m_rootNode);
    Relinearize();
    ValidateBookmarks();
}

// ---------------------------------------------------------------------------
// Public: Clear
// ---------------------------------------------------------------------------

void VirtualTree::Clear() {
    m_rootNode.reset();
    m_flat.clear();
    m_pathIndex.clear();
    m_pathIndexLower.clear();
    m_pathIndexLowerBuilt = false;
    m_fileCount = 0;
    m_dirCount = 0;
    m_totalSize = 0;
}

// ---------------------------------------------------------------------------
// Public: ToggleExpand
// ---------------------------------------------------------------------------

void VirtualTree::ToggleExpand(uint32_t flatIndex) {
    if (flatIndex >= static_cast<uint32_t>(m_flat.size()))
        return;

    const auto& entry = m_flat[flatIndex];
    if (!entry.isDirectory)
        return;

    if (entry.isExpanded) {
        CollapseNode(flatIndex);
    } else {
        ExpandNode(flatIndex);
    }
}

// ---------------------------------------------------------------------------
// Public: ExpandNode — incremental expand (insert children at flatIndex+1)
// ---------------------------------------------------------------------------

void VirtualTree::ExpandNode(uint32_t flatIndex) {
    if (flatIndex >= static_cast<uint32_t>(m_flat.size()))
        return;

    auto& entry = m_flat[flatIndex];
    if (!entry.isDirectory || entry.isExpanded)
        return;

    TreeNode* node = FindNode(entry.fullPath);
    if (!node || node->children.empty())
        return;

    // Mark expanded in both TreeNode and TreeEntry
    node->isExpanded = true;
    entry.isExpanded = true;

    // Build child TreeEntry items for immediate children
    std::vector<TreeEntry> children;
    BuildChildEntries(*node, flatIndex, entry.depth + 1, children);

    if (children.empty())
        return;

    size_t insertPos = static_cast<size_t>(flatIndex) + 1;

    // Insert children into flat list
    m_flat.insert(m_flat.begin() + static_cast<ptrdiff_t>(insertPos),
                  std::make_move_iterator(children.begin()),
                  std::make_move_iterator(children.end()));

    // Rebuild path index and fix parentIndex values for shifted entries
    RebuildPathIndex();
}

// ---------------------------------------------------------------------------
// Public: CollapseNode — incremental collapse (remove visible descendants)
// ---------------------------------------------------------------------------

void VirtualTree::CollapseNode(uint32_t flatIndex) {
    if (flatIndex >= static_cast<uint32_t>(m_flat.size()))
        return;

    auto& entry = m_flat[flatIndex];
    if (!entry.isDirectory || !entry.isExpanded)
        return;

    TreeNode* node = FindNode(entry.fullPath);
    if (!node)
        return;

    // Mark collapsed in both TreeNode and TreeEntry
    node->isExpanded = false;
    entry.isExpanded = false;

    // Count visible descendants (everything deeper until we hit same/lesser depth)
    size_t descendantCount = CountVisibleDescendants(flatIndex);
    if (descendantCount == 0)
        return;

    // Also collapse all expanded descendant TreeNodes so re-expanding shows them collapsed
    for (size_t i = static_cast<size_t>(flatIndex) + 1;
         i < static_cast<size_t>(flatIndex) + 1 + descendantCount; ++i) {
        if (m_flat[i].isDirectory && m_flat[i].isExpanded) {
            TreeNode* descNode = FindNode(m_flat[i].fullPath);
            if (descNode)
                descNode->isExpanded = false;
        }
    }

    // Erase the range of descendants from the flat list
    size_t eraseStart = static_cast<size_t>(flatIndex) + 1;
    m_flat.erase(m_flat.begin() + static_cast<ptrdiff_t>(eraseStart),
                 m_flat.begin() + static_cast<ptrdiff_t>(eraseStart + descendantCount));

    // Rebuild path index and fix parentIndex values for shifted entries
    RebuildPathIndex();
}

// ---------------------------------------------------------------------------
// Public: ExpandTo
// ---------------------------------------------------------------------------

void VirtualTree::ExpandTo(const std::string& path) {
    if (!m_rootNode || path.empty())
        return;

    // Split path by '/' and expand each ancestor directory.
    std::vector<std::string> parts;
    std::istringstream stream(path);
    std::string segment;
    while (std::getline(stream, segment, '/')) {
        if (!segment.empty())
            parts.push_back(segment);
    }

    TreeNode* current = m_rootNode.get();
    std::string accumulated;

    for (size_t i = 0; i < parts.size(); ++i) {
        if (!accumulated.empty())
            accumulated += '/';
        accumulated += parts[i];

        TreeNode* found = nullptr;
        for (auto& child : current->children) {
            if (_stricmp(child->name.c_str(), parts[i].c_str()) == 0) {
                found = child.get();
                break;
            }
        }

        if (!found)
            break;

        // Expand all ancestors (everything except the final segment itself,
        // unless the final segment is also a directory).
        if (found->isDirectory) {
            found->isExpanded = true;
        }

        current = found;
    }

    Relinearize();
}

// ---------------------------------------------------------------------------
// Public: SetBookmark / RemoveBookmark / IsBookmarked / GetBookmarkIcon
// ---------------------------------------------------------------------------

void VirtualTree::SetBookmark(const std::string& path, int iconIndex) {
    bool changed = false;
    for (auto& bm : m_bookmarks) {
        if (bm.path == path) {
            if (bm.iconIndex != iconIndex) {
                bm.iconIndex = iconIndex;
                changed = true;
            }
            if (changed) Relinearize();
            return;
        }
    }
    m_bookmarks.push_back({ path, iconIndex });
    if (m_sortMode == SortMode::Bookmarks && m_rootNode)
        SortChildren(*m_rootNode);
    Relinearize();
}

void VirtualTree::RemoveBookmark(const std::string& path) {
    auto it = std::remove_if(m_bookmarks.begin(), m_bookmarks.end(),
        [&path](const BookmarkEntry& bm) { return bm.path == path; });
    if (it != m_bookmarks.end()) {
        m_bookmarks.erase(it, m_bookmarks.end());
        if (m_sortMode == SortMode::Bookmarks && m_rootNode)
            SortChildren(*m_rootNode);
        Relinearize();
    }
}

bool VirtualTree::IsBookmarked(const std::string& path) const {
    for (const auto& bm : m_bookmarks)
        if (bm.path == path) return true;
    return false;
}

int VirtualTree::GetBookmarkIcon(const std::string& path) const {
    for (const auto& bm : m_bookmarks)
        if (bm.path == path) return bm.iconIndex;
    return -1;
}

// ---------------------------------------------------------------------------
// Public: SetSortMode
// ---------------------------------------------------------------------------

void VirtualTree::SetSortMode(SortMode mode) {
    if (m_sortMode == mode)
        return;

    m_sortMode = mode;

    if (m_rootNode)
        SortChildren(*m_rootNode);

    Relinearize();
}

// ---------------------------------------------------------------------------
// Public: FindByPath
// ---------------------------------------------------------------------------

uint32_t VirtualTree::FindByPath(const std::string& path) const {
    auto it = m_pathIndex.find(path);
    return (it != m_pathIndex.end()) ? it->second : UINT32_MAX;
}

// ---------------------------------------------------------------------------
// Public: FindByPathInsensitive — case-insensitive lookup with lazy index build
// ---------------------------------------------------------------------------

uint32_t VirtualTree::FindByPathInsensitive(const std::string& path) const {
    // Fast path: exact match
    auto it = m_pathIndex.find(path);
    if (it != m_pathIndex.end()) return it->second;

    // Build lowercase index lazily
    if (!m_pathIndexLowerBuilt) {
        m_pathIndexLower.clear();
        m_pathIndexLower.reserve(m_flat.size());
        for (size_t i = 0; i < m_flat.size(); ++i) {
            std::string lower;
            lower.reserve(m_flat[i].fullPath.size());
            for (char c : m_flat[i].fullPath)
                lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            m_pathIndexLower.emplace(std::move(lower), static_cast<uint32_t>(i));
        }
        m_pathIndexLowerBuilt = true;
    }

    std::string lowered;
    lowered.reserve(path.size());
    for (char c : path)
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    auto itLower = m_pathIndexLower.find(lowered);
    return (itLower != m_pathIndexLower.end()) ? itLower->second : UINT32_MAX;
}

// ---------------------------------------------------------------------------
// Public: GetPathIndexSize (diagnostic)
// ---------------------------------------------------------------------------

size_t VirtualTree::GetPathIndexSize() const {
    // Prefer lowercase index (reflects "effective" size for case-insensitive
    // lookups). Fall back to case-sensitive index if lowercase not built yet.
    if (m_pathIndexLowerBuilt) return m_pathIndexLower.size();
    return m_pathIndex.size();
}

// ---------------------------------------------------------------------------
// Public: DebugFindKeysWithPrefix (diagnostic)
// ---------------------------------------------------------------------------

std::vector<std::string> VirtualTree::DebugFindKeysWithPrefix(
    const std::string& loweredPrefix, size_t max) const
{
    std::vector<std::string> out;
    if (max == 0) return out;

    // Force-build the lowercase index if needed (mirrors FindByPathInsensitive).
    if (!m_pathIndexLowerBuilt) {
        m_pathIndexLower.clear();
        m_pathIndexLower.reserve(m_flat.size());
        for (size_t i = 0; i < m_flat.size(); ++i) {
            std::string lower;
            lower.reserve(m_flat[i].fullPath.size());
            for (char c : m_flat[i].fullPath)
                lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            m_pathIndexLower.emplace(std::move(lower), static_cast<uint32_t>(i));
        }
        m_pathIndexLowerBuilt = true;
    }

    out.reserve(max);
    for (const auto& kv : m_pathIndexLower) {
        const std::string& key = kv.first;
        if (loweredPrefix.empty() ||
            (key.size() >= loweredPrefix.size() &&
             key.compare(0, loweredPrefix.size(), loweredPrefix) == 0))
        {
            out.push_back(key);
            if (out.size() >= max) break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Public: ForEachFileUnder
// ---------------------------------------------------------------------------

void VirtualTree::ForEachFileUnder(const std::string& path,
    std::function<void(const std::string&, const std::string&, int64_t, void*)> visitor) const
{
    if (!m_rootNode) return;

    // Find the target node in the internal tree
    TreeNode* target = const_cast<VirtualTree*>(this)->FindNode(path);
    if (!target) return;

    // Recursive lambda to walk all children
    std::function<void(const TreeNode&)> walk = [&](const TreeNode& node) {
        if (!node.isDirectory) {
            visitor(node.fullPath, node.name, node.fileSize, node.nodePtr);
            return;
        }
        for (const auto& child : node.children) {
            walk(*child);
        }
    };

    walk(*target);
}

void VirtualTree::ForEachEntryUnder(const std::string& path,
    std::function<void(const std::string&, const std::string&, int64_t, bool, void*)> visitor) const
{
    if (!m_rootNode) return;

    TreeNode* target = const_cast<VirtualTree*>(this)->FindNode(path);
    if (!target) return;

    std::function<void(const TreeNode&)> walk = [&](const TreeNode& node) {
        visitor(node.fullPath, node.name, node.fileSize, node.isDirectory, node.nodePtr);
        if (node.isDirectory) {
            for (const auto& child : node.children)
                walk(*child);
        }
    };

    // Walk children of target (not target itself — it's the scope root)
    for (const auto& child : target->children)
        walk(*child);
}

// ---------------------------------------------------------------------------
// Private: BuildInternalTree
// ---------------------------------------------------------------------------

void VirtualTree::BuildInternalTree(const FileEntry& src, TreeNode& dst) {
    for (const auto& srcChild : src.children) {
        auto child = std::make_unique<TreeNode>();
        child->name = srcChild.name;
        child->fullPath = srcChild.fullPath;
        child->fileSize = srcChild.fileSize;
        child->isDirectory = srcChild.isDirectory;
        child->nodePtr = srcChild.nodePtr;

        if (srcChild.isDirectory) {
            child->type = FileType::Directory;
            ++m_dirCount;
            BuildInternalTree(srcChild, *child);
        } else {
            child->type = DetectFileType(srcChild.name);
            child->fileSize = srcChild.fileSize;
            ++m_fileCount;
            if (srcChild.fileSize > 0)
                m_totalSize += srcChild.fileSize;
        }

        dst.children.push_back(std::move(child));
    }
}

// ---------------------------------------------------------------------------
// Private: SortChildren
// ---------------------------------------------------------------------------

static int CompareNoCase(const std::string& a, const std::string& b) {
    return _stricmp(a.c_str(), b.c_str());
}

void VirtualTree::SortChildren(TreeNode& node) {
    SortMode mode = m_sortMode;

    std::stable_sort(node.children.begin(), node.children.end(),
        [mode, this](const std::unique_ptr<TreeNode>& a, const std::unique_ptr<TreeNode>& b) {
            // Rule 1: directories always come first.
            if (a->isDirectory != b->isDirectory)
                return a->isDirectory;

            // Rule 2: both directories -> preserve GGPK insertion order.
            // stable_sort keeps order of equal elements, so returning false here
            // means the original sequence is retained regardless of sort mode.
            if (a->isDirectory && b->isDirectory)
                return false;

            // Rule 3: both files -> apply the selected mode.
            switch (mode) {
            case SortMode::None:
                return false;  // preserve insertion order within files

            case SortMode::Size:
                if (a->fileSize != b->fileSize)
                    return a->fileSize > b->fileSize;
                return CompareNoCase(a->name, b->name) < 0;

            case SortMode::Type: {
                const char* ta = FileTypeToString(a->type);
                const char* tb = FileTypeToString(b->type);
                int cmp = _stricmp(ta, tb);
                if (cmp != 0)
                    return cmp < 0;
                return CompareNoCase(a->name, b->name) < 0;
            }

            case SortMode::Bookmarks: {
                bool ba = IsBookmarked(a->fullPath);
                bool bb = IsBookmarked(b->fullPath);
                if (ba != bb)
                    return ba;  // bookmarked files first
                return CompareNoCase(a->name, b->name) < 0;
            }

            case SortMode::Name:
            default:
                return CompareNoCase(a->name, b->name) < 0;
            }
        });

    // Recurse into directory children (directories themselves are not reordered,
    // but their files might be).
    for (auto& child : node.children) {
        if (child->isDirectory)
            SortChildren(*child);
    }
}

// ---------------------------------------------------------------------------
// Private: Relinearize / LinearizeRecursive
// ---------------------------------------------------------------------------

void VirtualTree::Relinearize() {
    m_flat.clear();
    m_pathIndex.clear();

    if (!m_rootNode)
        return;

    // Reserve a rough estimate to reduce reallocations.
    m_flat.reserve(m_fileCount + m_dirCount);

    for (auto& child : m_rootNode->children) {
        LinearizeRecursive(*child, 0, UINT32_MAX);
    }
}

void VirtualTree::LinearizeRecursive(TreeNode& node, uint32_t depth, uint32_t parentIndex) {
    TreeEntry entry;
    entry.name = node.name;
    entry.fullPath = node.fullPath;
    entry.depth = depth;
    entry.childCount = static_cast<uint32_t>(node.children.size());
    entry.fileSize = node.fileSize;
    entry.type = node.type;
    entry.isDirectory = node.isDirectory;
    entry.isExpanded = node.isExpanded;
    int bmIcon = GetBookmarkIcon(node.fullPath);
    entry.isBookmarked = (bmIcon >= 0);
    entry.bookmarkIcon = bmIcon;
    entry.parentIndex = parentIndex;
    entry.nodePtr = node.nodePtr;

    uint32_t myIndex = static_cast<uint32_t>(m_flat.size());
    m_pathIndex[node.fullPath] = myIndex;
    m_flat.push_back(std::move(entry));

    if (node.isDirectory && node.isExpanded) {
        for (auto& child : node.children) {
            LinearizeRecursive(*child, depth + 1, myIndex);
        }
    }
}

// ---------------------------------------------------------------------------
// Private: CountVisibleDescendants
// ---------------------------------------------------------------------------

size_t VirtualTree::CountVisibleDescendants(uint32_t flatIndex) const {
    uint32_t baseDepth = m_flat[flatIndex].depth;
    size_t count = 0;
    for (size_t i = static_cast<size_t>(flatIndex) + 1; i < m_flat.size(); ++i) {
        if (m_flat[i].depth <= baseDepth)
            break;
        ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Private: RebuildPathIndex — rebuild m_pathIndex and parentIndex from scratch
// ---------------------------------------------------------------------------

void VirtualTree::RebuildPathIndex() {
    m_pathIndex.clear();
    m_pathIndex.reserve(m_flat.size());
    // Invalidate lowercase index — will be rebuilt on next case-insensitive lookup
    m_pathIndexLower.clear();
    m_pathIndexLowerBuilt = false;

    // Stack tracks the flat index of the most recent ancestor at each depth.
    // parentStack[depth] = flat index of the directory at that depth.
    std::vector<uint32_t> parentStack;

    for (uint32_t i = 0; i < static_cast<uint32_t>(m_flat.size()); ++i) {
        auto& e = m_flat[i];
        m_pathIndex[e.fullPath] = i;

        // Maintain parent stack: trim to current depth, look up parent
        while (parentStack.size() > static_cast<size_t>(e.depth))
            parentStack.pop_back();

        if (parentStack.empty()) {
            e.parentIndex = UINT32_MAX;
        } else {
            e.parentIndex = parentStack.back();
        }

        // If this is a directory, push it as potential parent for deeper items
        if (e.isDirectory) {
            parentStack.push_back(i);
        }
    }
}

// ---------------------------------------------------------------------------
// Private: BuildChildEntries — build TreeEntry items for immediate children
// ---------------------------------------------------------------------------

void VirtualTree::BuildChildEntries(TreeNode& parentNode, uint32_t parentFlatIndex,
                                     uint32_t depth, std::vector<TreeEntry>& out) const {
    out.reserve(parentNode.children.size());

    for (auto& child : parentNode.children) {
        TreeEntry entry;
        entry.name = child->name;
        entry.fullPath = child->fullPath;
        entry.depth = depth;
        entry.childCount = static_cast<uint32_t>(child->children.size());
        entry.fileSize = child->fileSize;
        entry.type = child->type;
        entry.isDirectory = child->isDirectory;
        entry.isExpanded = false;  // Children start collapsed on expand
        int bmIcon = GetBookmarkIcon(child->fullPath);
        entry.isBookmarked = (bmIcon >= 0);
        entry.bookmarkIcon = bmIcon;
        entry.parentIndex = parentFlatIndex;  // Will be corrected by RebuildPathIndex
        entry.nodePtr = child->nodePtr;

        out.push_back(std::move(entry));
    }
}

// ---------------------------------------------------------------------------
// Public: ValidateBookmarks
// ---------------------------------------------------------------------------

void VirtualTree::ValidateBookmarks() {
    for (auto& bm : m_bookmarks) {
        bm.valid = (FindNode(bm.path) != nullptr);
    }
}

// ---------------------------------------------------------------------------
// Private: FindNode — walk internal tree by splitting path on '/'
// ---------------------------------------------------------------------------

VirtualTree::TreeNode* VirtualTree::FindNode(const std::string& path) {
    if (!m_rootNode)
        return nullptr;

    if (path.empty())
        return m_rootNode.get();

    std::vector<std::string> parts;
    std::istringstream stream(path);
    std::string segment;
    while (std::getline(stream, segment, '/')) {
        if (!segment.empty())
            parts.push_back(segment);
    }

    TreeNode* current = m_rootNode.get();

    for (const auto& part : parts) {
        TreeNode* found = nullptr;
        for (auto& child : current->children) {
            if (child->name == part) {
                found = child.get();
                break;
            }
        }

        if (!found)
            return nullptr;

        current = found;
    }

    return current;
}

} // namespace GGPKExp
