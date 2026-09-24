#include "AocResolver.h"

#include "AocLightsParser.h"
#include "AocParser.h"
#include "AocSoundEventsParser.h"
#include "AocParticleEffectsParser.h"
#include "AocTokenizer.h"
#include "../GGPKManager.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
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

// Deep-copy AocBlock (unique_ptr fields — must rebuild typed views from properties).
AocBlock CloneBlock(const AocBlock& src) {
    AocBlock dst;
    dst.name = src.name;
    dst.sourceLine = src.sourceLine;
    dst.sourceAocPath = src.sourceAocPath;
    dst.properties = src.properties;  // AocRawNode is copyable (std::string + std::vector)
    // Typed views will be rebuilt by caller via ParseAoc-internal helpers; for resolver
    // we only use properties for merging. But for final merged AocDocument we need typed
    // views — we'll re-populate them after merging all blocks.
    return dst;
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

// Rebuild typed views from merged properties.
void RebuildTypedViews(AocBlock& block, std::vector<std::string>& warnings) {
    // Hijack ParseAoc's typed builders by going through AocParser.cpp helpers — but
    // those are anonymous-namespace static. So re-invoke the typed builders inline
    // by constructing a fake AocFile and parsing it. Simplest: call ParseAoc on a
    // synthetic token stream. Alternative: duplicate the builder logic here.
    //
    // Chosen approach: expose helpers via a shared header, but to avoid extra plumbing
    // in this sprint, we re-derive typed views by a small inline scan.
    if (block.name == "ClientAnimationController") {
        block.clientAnim = std::make_unique<ClientAnimCtrl>();
        for (const AocRawNode& node : block.properties) {
            if (node.key == "skeleton") {
                block.clientAnim->skeletonPath = node.value;
                block.clientAnim->skeletonPathSource = node.sourceAocPath;
            } else if (node.key == "socket") {
                Socket s;
                s.name = node.value;
                s.sourceLine = node.sourceLine;
                s.sourceAocPath = node.sourceAocPath;
                for (const AocRawNode& c : node.children) {
                    if (c.key == "parent") s.parentBoneName = c.value;
                    else if (c.key == "translation") {
                        float v[3] = {0,0,0};
                        std::istringstream is(c.value);
                        is >> v[0] >> v[1] >> v[2];
                        s.translation[0] = v[0]; s.translation[1] = v[1]; s.translation[2] = v[2];
                    }
                    else if (c.key == "rotation") {
                        float v[3] = {0,0,0};
                        std::istringstream is(c.value);
                        is >> v[0] >> v[1] >> v[2];
                        s.rotation[0] = v[0]; s.rotation[1] = v[1]; s.rotation[2] = v[2];
                    }
                    else if (c.key == "scale") {
                        float v[3] = {1,1,1};
                        std::istringstream is(c.value);
                        is >> v[0] >> v[1] >> v[2];
                        s.scale[0] = v[0]; s.scale[1] = v[1]; s.scale[2] = v[2];
                    }
                    else if (c.key == "ignore_parent_axis")     s.ignoreParentAxis = c.value;
                    else if (c.key == "ignore_parent_rotation") s.ignoreParentRot  = c.value;
                }
                block.clientAnim->sockets.push_back(std::move(s));
            } else if (node.key == "bone_group") {
                BoneGroup g;
                std::istringstream is(node.value);
                std::string tok;
                if (!(is >> g.name)) { warnings.push_back("bone_group without name"); continue; }
                if (is >> tok) g.maskFlag = (tok == "true" || tok == "1");
                while (is >> tok) g.boneNames.push_back(tok);
                g.sourceLine = node.sourceLine;
                g.sourceAocPath = node.sourceAocPath;
                block.clientAnim->boneGroups.push_back(std::move(g));
            } else {
                KeyValue kv;
                kv.key = node.key; kv.value = node.value;
                kv.sourceLine = node.sourceLine; kv.sourceAocPath = node.sourceAocPath;
                block.clientAnim->otherProperties.push_back(std::move(kv));
            }
        }
    } else if (block.name == "SkinMesh") {
        block.skinMesh = std::make_unique<SkinMeshBlock>();
        for (const AocRawNode& node : block.properties) {
            if (node.key == "skin") {
                block.skinMesh->skinPath = node.value;
                block.skinMesh->skinPathSource = node.sourceAocPath;
                for (const AocRawNode& c : node.children) {
                    KeyValue kv;
                    kv.key = c.key; kv.value = c.value;
                    kv.sourceLine = c.sourceLine; kv.sourceAocPath = c.sourceAocPath;
                    if (c.key.size() >= 5 && c.key.compare(c.key.size() - 5, 5, "Shape") == 0) {
                        block.skinMesh->shapeOverrides.push_back(std::move(kv));
                    } else {
                        block.skinMesh->otherProperties.push_back(std::move(kv));
                    }
                }
            } else {
                KeyValue kv;
                kv.key = node.key; kv.value = node.value;
                kv.sourceLine = node.sourceLine; kv.sourceAocPath = node.sourceAocPath;
                block.skinMesh->otherProperties.push_back(std::move(kv));
            }
        }
    } else if (block.name == "AnimationController") {
        block.animController = std::make_unique<AnimCtrlBlock>();
        for (const AocRawNode& node : block.properties) {
            KeyValue kv;
            kv.key = node.key; kv.value = node.value;
            kv.sourceLine = node.sourceLine; kv.sourceAocPath = node.sourceAocPath;
            // Match any `on_<name>` handler (real .aoc files use `on_open`,
            // `on_close`, `on_death`, etc. directly — the `on_event_` form
            // is a rarer variant). Everything else goes to otherProperties.
            if (node.key.rfind("on_", 0) == 0) {
                block.animController->eventHandlers.push_back(std::move(kv));
            } else {
                block.animController->otherProperties.push_back(std::move(kv));
            }
        }
    } else if (block.name == "Lights") {
        // ParseLights expects a synthetic AocRawNode whose `children` are the
        // block's body lines. AocBlock stores properties directly, so wrap them.
        AocRawNode synthetic;
        synthetic.key = block.name;
        synthetic.sourceLine = block.sourceLine;
        synthetic.sourceAocPath = block.sourceAocPath;
        synthetic.children = block.properties;
        auto lb = std::make_unique<LightsBlock>();
        std::vector<std::string> errors;
        if (ParseLights(synthetic, *lb, warnings, errors)) {
            block.lights = std::move(lb);
        }
        for (std::string& e : errors) warnings.push_back(std::move(e));
    } else if (block.name == "SoundEvents") {
        AocRawNode synthetic;
        synthetic.key = block.name;
        synthetic.sourceLine = block.sourceLine;
        synthetic.sourceAocPath = block.sourceAocPath;
        synthetic.children = block.properties;
        auto sb = std::make_unique<SoundEventsBlock>();
        std::vector<std::string> errors;
        if (ParseSoundEvents(synthetic, *sb, warnings, errors)) {
            block.soundEvents = std::move(sb);
        }
        for (std::string& e : errors) warnings.push_back(std::move(e));
    } else if (block.name == "ParticleEffects") {
        AocRawNode synthetic;
        synthetic.key = block.name;
        synthetic.sourceLine = block.sourceLine;
        synthetic.sourceAocPath = block.sourceAocPath;
        synthetic.children = block.properties;
        auto pe = std::make_unique<ParticleEffectsBlock>();
        std::vector<std::string> errors;
        if (ParseParticleEffects(synthetic, *pe, warnings, errors)) {
            block.particleEffects = std::move(pe);
        }
        for (std::string& e : errors) warnings.push_back(std::move(e));
    }
}

// Merge one block from child on top of parent's (or add it).
void MergeBlockInto(std::vector<AocBlock>& target,
                    const AocBlock& childBlock,
                    const std::string& childSource) {
    auto it = std::find_if(target.begin(), target.end(),
                           [&](const AocBlock& b) { return b.name == childBlock.name; });
    if (it == target.end()) {
        AocBlock added = CloneBlock(childBlock);
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

Result<AocDocument> ResolveExtends(const AocFile&    leaf,
                                    GGPKManager*      ggpkMgr,
                                    AocResolverCache* cache) {
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

        std::shared_ptr<AocFile> parent;
        if (cache) {
            auto it = cache->find(normParent);
            if (it != cache->end()) parent = it->second;
        }
        if (!parent) {
            parent = ParseParent(parentPath, ggpkMgr, doc.warnings);
            if (!parent) break;
            if (cache) cache->emplace(normParent, parent);
        }

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
        // Collect parser warnings (but tag with file path for clarity)
        for (const std::string& w : f.warnings) {
            merged.warnings.push_back(f.sourcePath + ": " + w);
        }
    }

    // Rebuild typed views for merged blocks (they came from MergeBlockInto which only
    // touched .properties; typed views are nullptr on those).
    for (AocBlock& b : merged.blocks) {
        RebuildTypedViews(b, doc.warnings);
    }

    // Post-pass: some .aoc files (e.g. clamstash) define bone_group entries in a
    // standalone `BoneGroups` top-level block rather than nested inside
    // ClientAnimationController. Merge those into CAC so downstream consumers
    // (particle/emitter bone resolution, etc.) see a unified list.
    {
        ClientAnimCtrl* cac = nullptr;
        for (AocBlock& b : merged.blocks) {
            if (b.clientAnim) { cac = b.clientAnim.get(); break; }
        }
        if (cac) {
            for (const AocBlock& b : merged.blocks) {
                if (b.name != "BoneGroups") continue;
                for (const AocRawNode& node : b.properties) {
                    if (node.key != "bone_group") continue;
                    BoneGroup g;
                    std::istringstream is(node.value);
                    std::string tok;
                    if (!(is >> g.name)) {
                        doc.warnings.push_back(b.sourceAocPath + ": bone_group without name");
                        continue;
                    }
                    if (is >> tok) g.maskFlag = (tok == "true" || tok == "1");
                    while (is >> tok) g.boneNames.push_back(tok);
                    g.sourceLine = node.sourceLine;
                    g.sourceAocPath = node.sourceAocPath;
                    cac->boneGroups.push_back(std::move(g));
                }
            }
        }
    }

    doc.merged = std::move(merged);
    return Result<AocDocument>::Ok(std::move(doc));
}

}} // namespace GGPKExp::Parsers
