#pragma once
#include "FileRecord.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace GGPK {

/// Base class for tree nodes (directories and files).
class ITreeNode {
public:
    virtual ~ITreeNode() = default;
    virtual const std::string& GetName() const = 0;
    virtual bool IsDirectory() const = 0;
};

/// A directory node in the file tree.
class DirectoryNode : public ITreeNode {
public:
    explicit DirectoryNode(std::string name, DirectoryNode* parent = nullptr)
        : m_name(std::move(name)), m_parent(parent) {}

    const std::string& GetName() const override { return m_name; }
    bool IsDirectory() const override { return true; }

    DirectoryNode* GetParent() const { return m_parent; }

    std::vector<std::unique_ptr<ITreeNode>>& GetChildren() { return m_children; }
    const std::vector<std::unique_ptr<ITreeNode>>& GetChildren() const { return m_children; }

    /// Find a child directory by name. Returns nullptr if not found.
    DirectoryNode* FindChildDir(const std::string& name) {
        for (auto& child : m_children) {
            if (child->IsDirectory() && child->GetName() == name)
                return static_cast<DirectoryNode*>(child.get());
        }
        return nullptr;
    }

    /// Find a child file by name. Returns nullptr if not found.
    class FileNode* FindChildFile(const std::string& name);

    /// Get or create a child directory.
    DirectoryNode* GetOrCreateChildDir(const std::string& name) {
        auto* existing = FindChildDir(name);
        if (existing) return existing;
        auto node = std::make_unique<DirectoryNode>(name, this);
        auto* ptr = node.get();
        m_children.push_back(std::move(node));
        return ptr;
    }

    /// Add a child node.
    void AddChild(std::unique_ptr<ITreeNode> child) {
        m_children.push_back(std::move(child));
    }

    /// Recursively collect all FileNode descendants.
    void CollectFiles(std::vector<class FileNode*>& out);

    /// Recursively collect all FileNode descendants matching a filter.
    void CollectFiles(std::vector<class FileNode*>& out,
                      const std::function<bool(const FileNode&)>& filter);

    /// Recursively collect all FileNode descendants with given extension.
    void CollectFilesByExtension(std::vector<class FileNode*>& out, const std::string& ext);

private:
    std::string m_name;
    DirectoryNode* m_parent;
    std::vector<std::unique_ptr<ITreeNode>> m_children;
};

/// A file node in the file tree, wrapping a FileRecord.
class FileNode : public ITreeNode {
public:
    FileNode(std::string name, FileRecord* record, DirectoryNode* parent)
        : m_name(std::move(name)), m_record(record), m_parent(parent) {}

    const std::string& GetName() const override { return m_name; }
    bool IsDirectory() const override { return false; }

    FileRecord* GetRecord() const { return m_record; }
    DirectoryNode* GetParent() const { return m_parent; }

    /// Check if the file name ends with the given extension (case-insensitive).
    bool HasExtension(const std::string& ext) const {
        if (ext.size() > m_name.size()) return false;
        auto nameEnd = m_name.substr(m_name.size() - ext.size());
        return std::equal(ext.begin(), ext.end(), nameEnd.begin(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    }

private:
    std::string m_name;
    FileRecord* m_record;
    DirectoryNode* m_parent;
};

// Inline implementations that need FileNode to be complete
inline FileNode* DirectoryNode::FindChildFile(const std::string& name) {
    for (auto& child : m_children) {
        if (!child->IsDirectory() && child->GetName() == name)
            return static_cast<FileNode*>(child.get());
    }
    return nullptr;
}

inline void DirectoryNode::CollectFiles(std::vector<FileNode*>& out) {
    for (auto& child : m_children) {
        if (child->IsDirectory())
            static_cast<DirectoryNode*>(child.get())->CollectFiles(out);
        else
            out.push_back(static_cast<FileNode*>(child.get()));
    }
}

inline void DirectoryNode::CollectFiles(std::vector<FileNode*>& out,
                                         const std::function<bool(const FileNode&)>& filter) {
    for (auto& child : m_children) {
        if (child->IsDirectory())
            static_cast<DirectoryNode*>(child.get())->CollectFiles(out, filter);
        else {
            auto* fn = static_cast<FileNode*>(child.get());
            if (filter(*fn))
                out.push_back(fn);
        }
    }
}

inline void DirectoryNode::CollectFilesByExtension(std::vector<FileNode*>& out, const std::string& ext) {
    CollectFiles(out, [&ext](const FileNode& fn) { return fn.HasExtension(ext); });
}

} // namespace GGPK
