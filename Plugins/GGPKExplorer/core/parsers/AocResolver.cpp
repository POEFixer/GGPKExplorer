#include "AocResolver.h"

#include "AocParser.h"
#include "AocTokenizer.h"
#include "../GGPKManager.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <unordered_set>

namespace GGPKExp { namespace Parsers {

namespace {

constexpr int kMaxDepth = 16;

std::string NormalizePath(const std::string& p) {
    std::string out;
    out.reserve(p.size());
    for (char c : p) {
        if (c == '\\') out.push_back('/');
        else out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// Try "<extendsPath>.aoc" then "<extendsPath>" verbatim (in case caller already had extension).
std::string TryFindAoc(const std::string& extendsPath, GGPKManager* mgr) {
    if (!mgr) return {};
    const std::string norm = NormalizePath(extendsPath);
    std::string withExt = norm + ".aoc";
    if (void* node = mgr->FindNodeByPath(withExt)) {
        (void)node;
        return withExt;
    }
    if (void* node = mgr->FindNodeByPath(norm)) {
        (void)node;
        return norm;
    }
    return {};
}

// Parse parent .aoc bytes via GGPKManager::ReadFile.
std::shared_ptr<AocFile> ParseParent(const std::string& path, GGPKManager* mgr,
                                     std::vector<std::string>& warnings) {
    void* node = mgr->FindNodeByPath(path);
    if (!node) {
        warnings.push_back("parent node lookup failed: " + path);
        return {};
    }
    auto r = mgr->ReadFile(node);
    if (!r) {
        warnings.push_back("parent read failed: " + path + " — " + r.error);
        return {};
    }
    auto tok = TokenizeAoc(r.value.data(), r.value.size());
    if (!tok) {
        warnings.push_back("parent tokenize failed: " + path + " — " + tok.error);
        return {};
    }
    auto parse = ParseAoc(tok.value);
    if (!parse) {
        warnings.push_back("parent parse failed: " + path + " — " + parse.error);
        return {};
    }
    parse.value.sourcePath = path;
    return std::make_shared<AocFile>(std::move(parse.value));
}

// Merge two generic property lists. For each key:
//   - if both have the key → child wins (replace value, keep children from child)
//   - if only parent has → keep parent
//   - new keys from child → append at end
// Provenance: child items get `childSource` as sourceAocPath; parent items keep theirs.
std::vector<AocRawNode> MergeProperties(const std::vector<AocRawNode>& parent,
                                         const std::vector<AocRawNode>& child,
                                         const std::string& childSource) {
    std::vector<AocRawNode> out = parent;
    for (const AocRawNode& cn : child) {
        // Find same key in `out`; if key is "socket" or "bone_group" → match by value (name).
        auto sameIdentity = [&](const AocRawNode& a) {
            if (a.key != cn.key) return false;
            if (cn.key == "socket" || cn.key == "bone_group") return a.value == cn.value;
            return true;  // plain key, any match
        };
        auto it = std::find_if(out.begin(), out.end(), sameIdentity);
        if (it != out.end()) {
            *it = cn;
            it->sourceAocPath = childSource;
        } else {
            AocRawNode copy = cn;
            copy.sourceAocPath = childSource;
            out.push_back(std::move(copy));
        }
    }
    return out;
}

// Merge one block from child on top of parent's (or add it).
void MergeBlockInto(std::vector<AocBlock>& target,
                    const AocBlock& childBlock,
                    const std::string& childSource) {
    auto it = std::find_if(target.begin(), target.end(),
                           [&](const AocBlock& b) { return b.name == childBlock.name; });
    if (it == target.end()) {
        AocBlock added = childBlock;
        added.sourceAocPath = childSource;
        // Re-tag properties if not already
        for (auto& p : added.properties) {
            if (p.sourceAocPath.empty()) p.sourceAocPath = childSource;
        }
        target.push_back(std::move(added));
    } else {
        it->properties = MergeProperties(it->properties, childBlock.properties, childSource);
    }
}

} // anonymous namespace

Result<AocDocument> ResolveExtends(const AocFile& leaf, GGPKManager* ggpkMgr) {
    AocDocument doc;
    doc.inheritanceChain.push_back(leaf.sourcePath);

    // Walk the chain from leaf to root, collecting into `chain` in order.
    std::vector<const AocFile*> chain;
    chain.push_back(&leaf);

    const AocFile* current = &leaf;
    std::unordered_set<std::string> visited;
    visited.insert(NormalizePath(leaf.sourcePath));

    // Local storage for parents fetched this call (so pointers stay valid).
    std::vector<std::shared_ptr<AocFile>> fetched;

    int depth = 0;
    while (current && !current->extendsPath.empty()) {
        ++depth;
        if (depth > kMaxDepth) {
            doc.warnings.push_back("extends chain exceeds depth 16, truncating");
            break;
        }
        if (current->extendsPath == "Metadata/Parent") break;

        std::string parentPath = TryFindAoc(current->extendsPath, ggpkMgr);
        if (parentPath.empty()) {
            doc.warnings.push_back("parent '" + current->extendsPath + "' not found in GGPK");
            break;
        }
        std::string normParent = NormalizePath(parentPath);
        if (visited.count(normParent)) {
            doc.warnings.push_back("cycle detected at " + parentPath);
            break;
        }
        visited.insert(normParent);

        std::shared_ptr<AocFile> parent = ParseParent(parentPath, ggpkMgr, doc.warnings);
        if (!parent) break;

        fetched.push_back(parent);
        chain.push_back(parent.get());
        doc.inheritanceChain.push_back(parentPath);
        current = parent.get();
    }

    // Merge from root (oldest, at chain.back()) down to leaf (at chain.front()).
    // Start merged = root's view, then apply children one by one.
    AocFile merged;
    merged.version = leaf.version;        // leaf wins for version (usually same everywhere)
    merged.extendsPath = leaf.extendsPath;
    merged.sourcePath = leaf.sourcePath;

    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const AocFile& f = **it;
        for (const AocBlock& b : f.blocks) {
            MergeBlockInto(merged.blocks, b, f.sourcePath);
        }
    }

    doc.merged = std::move(merged);
    return Result<AocDocument>::Ok(std::move(doc));
}

AocModelRefs FindModelRefs(const AocFile& merged) {
    // Block names are unique after the merge; within a block the last line wins.
    AocModelRefs refs;
    for (const AocBlock& b : merged.blocks) {
        const bool skin = (b.name == "SkinMesh");
        const bool rig  = (b.name == "ClientAnimationController");
        if (!skin && !rig) continue;
        for (const AocRawNode& node : b.properties) {
            if (skin && node.key == "skin")     refs.skinPath = node.value;
            if (rig  && node.key == "skeleton") refs.skeletonPath = node.value;
        }
    }
    return refs;
}

}} // namespace GGPKExp::Parsers
