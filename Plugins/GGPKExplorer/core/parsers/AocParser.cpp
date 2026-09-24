#include "AocParser.h"

#include <cctype>
#include <cstring>
#include <memory>
#include <sstream>
#include <unordered_set>

namespace GGPKExp { namespace Parsers {

namespace {

// Split a whitespace-delimited string into tokens (bone_group value parser).
std::vector<std::string> SplitWhitespace(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) { out.push_back(std::move(cur)); cur.clear(); }
        } else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

// Parse "x y z" → float[3]. Returns false on bad format; on failure, `out` is left untouched.
bool ParseVec3(const std::string& value, float out[3]) {
    auto toks = SplitWhitespace(value);
    if (toks.size() != 3) return false;
    float tmp[3];
    for (int i = 0; i < 3; ++i) {
        try {
            tmp[i] = std::stof(toks[static_cast<std::size_t>(i)]);
        } catch (...) { return false; }
    }
    out[0] = tmp[0]; out[1] = tmp[1]; out[2] = tmp[2];
    return true;
}

// Stack entry: pointer to node + its indent level.
struct StackFrame {
    AocRawNode* node;
    int         indent;
};

// Build the raw AST for one block body. Reads Property lines until BlockClose.
//
// If insideBlock == true (we're between `{` and `}`), accept Property lines at ANY indent.
// Some game files use indent 0 inside a block body (e.g. `SoundEvents {` opened on one line
// with body lines having no leading tabs). The stack-based tree construction uses indent to
// reconstruct child/parent hierarchy within the block.
//
// If insideBlock == false (legacy, nested raw subtree), stop when indent <= parentIndent.
void BuildRawSubtree(const std::vector<AocLine>& lines, std::size_t& i,
                     int parentIndent, std::vector<AocRawNode>& out,
                     bool insideBlock = false) {
    std::vector<StackFrame> stack;

    while (i < lines.size()) {
        const AocLine& line = lines[i];
        if (line.kind == AocLine::Kind::Empty) { ++i; continue; }
        if (line.kind == AocLine::Kind::BlockClose) break;
        if (line.kind != AocLine::Kind::Property) break;   // header or blockopen shouldn't appear here
        if (!insideBlock && line.indent <= parentIndent) break;  // back up to caller

        // Pop stack back to appropriate level.
        // CRITICAL: pop ALL stack entries with indent >= line.indent before any
        // push_back below. This guarantees that when we push to out/parent->children,
        // no live stack pointer aliases the vector being mutated, so reallocation
        // is safe.
        while (!stack.empty() && stack.back().indent >= line.indent) stack.pop_back();

        AocRawNode node;
        node.key = line.key;
        node.value = line.value;
        node.sourceLine = line.lineNumber;

        if (stack.empty()) {
            out.push_back(std::move(node));
            stack.push_back({&out.back(), line.indent});
        } else {
            AocRawNode* parent = stack.back().node;
            parent->children.push_back(std::move(node));
            stack.push_back({&parent->children.back(), line.indent});
        }
        ++i;
    }
}

// Forward decls for typed post-processing.
std::unique_ptr<ClientAnimCtrl> BuildClientAnim(const std::vector<AocRawNode>& properties,
                                                 std::vector<std::string>& warnings);
std::unique_ptr<SkinMeshBlock>  BuildSkinMesh(const std::vector<AocRawNode>& properties,
                                               std::vector<std::string>& warnings);
std::unique_ptr<AnimCtrlBlock>  BuildAnimCtrl(const std::vector<AocRawNode>& properties,
                                               std::vector<std::string>& warnings);

} // anonymous namespace

Result<AocFile> ParseAoc(const std::vector<AocLine>& lines) {
    AocFile out;

    std::size_t i = 0;

    // --- Header ---
    while (i < lines.size()) {
        const AocLine& line = lines[i];
        if (line.kind == AocLine::Kind::Empty) { ++i; continue; }
        if (line.kind != AocLine::Kind::HeaderKV) break;
        if (line.key == "version") {
            try { out.version = std::stoi(line.value); }
            catch (...) {
                std::ostringstream os;
                os << "Invalid version number at line " << line.lineNumber;
                return Result<AocFile>::Fail(os.str());
            }
        } else if (line.key == "extends") {
            out.extendsPath = line.value;
        }
        ++i;
    }

    // --- Top-level blocks ---
    while (i < lines.size()) {
        const AocLine& line = lines[i];
        if (line.kind == AocLine::Kind::Empty) { ++i; continue; }

        if (line.kind != AocLine::Kind::BlockOpen) {
            std::ostringstream os;
            os << "Expected top-level block name at line " << line.lineNumber
               << ", got: " << line.raw;
            return Result<AocFile>::Fail(os.str());
        }

        AocBlock block;
        block.name = line.key;
        block.sourceLine = line.lineNumber;
        std::size_t openLine = line.lineNumber;
        ++i;

        // Skip Empty lines until BraceOpen
        while (i < lines.size() && lines[i].kind == AocLine::Kind::Empty) ++i;
        if (i >= lines.size() || lines[i].kind != AocLine::Kind::BraceOpen) {
            std::ostringstream os;
            os << "Expected '{' after block name '" << block.name << "' at line " << openLine;
            return Result<AocFile>::Fail(os.str());
        }
        ++i;  // past '{'

        // Read block body (accept any indent; terminator is '}')
        BuildRawSubtree(lines, i, /*parentIndent*/ 0, block.properties, /*insideBlock*/ true);

        // Skip Empty lines until BlockClose
        while (i < lines.size() && lines[i].kind == AocLine::Kind::Empty) ++i;
        if (i >= lines.size() || lines[i].kind != AocLine::Kind::BlockClose) {
            std::ostringstream os;
            os << "Expected '}' to close block '" << block.name << "' opened at line " << openLine;
            return Result<AocFile>::Fail(os.str());
        }
        ++i;  // past '}'

        // Typed post-processing for 3 known block types
        if (block.name == "ClientAnimationController") {
            block.clientAnim = BuildClientAnim(block.properties, out.warnings);
        } else if (block.name == "SkinMesh") {
            block.skinMesh = BuildSkinMesh(block.properties, out.warnings);
        } else if (block.name == "AnimationController") {
            block.animController = BuildAnimCtrl(block.properties, out.warnings);
        }

        out.blocks.push_back(std::move(block));
    }

    return Result<AocFile>::Ok(std::move(out));
}

namespace {

// -----------------------------------------------------------------------
// ClientAnimationController: extract skeleton, sockets, bone_groups
// -----------------------------------------------------------------------
std::unique_ptr<ClientAnimCtrl> BuildClientAnim(const std::vector<AocRawNode>& properties,
                                                 std::vector<std::string>& warnings) {
    auto out = std::make_unique<ClientAnimCtrl>();
    std::unordered_set<std::string> seenSocketNames;
    std::unordered_set<std::string> seenGroupNames;

    for (const AocRawNode& node : properties) {
        if (node.key == "skeleton") {
            out->skeletonPath = node.value;
        } else if (node.key == "socket") {
            Socket sock;
            sock.name = node.value;
            sock.sourceLine = node.sourceLine;
            for (const AocRawNode& child : node.children) {
                if (child.key == "parent")                      sock.parentBoneName = child.value;
                else if (child.key == "translation") {
                    if (!ParseVec3(child.value, sock.translation)) {
                        std::ostringstream os;
                        os << "Malformed translation vec3 for socket '" << sock.name
                           << "' at line " << child.sourceLine;
                        warnings.push_back(os.str());
                    }
                }
                else if (child.key == "rotation") {
                    if (!ParseVec3(child.value, sock.rotation)) {
                        std::ostringstream os;
                        os << "Malformed rotation vec3 for socket '" << sock.name
                           << "' at line " << child.sourceLine;
                        warnings.push_back(os.str());
                    }
                }
                else if (child.key == "scale") {
                    if (!ParseVec3(child.value, sock.scale)) {
                        std::ostringstream os;
                        os << "Malformed scale vec3 for socket '" << sock.name
                           << "' at line " << child.sourceLine;
                        warnings.push_back(os.str());
                    }
                }
                else if (child.key == "ignore_parent_axis")     sock.ignoreParentAxis = child.value;
                else if (child.key == "ignore_parent_rotation") sock.ignoreParentRot  = child.value;
                else {
                    std::ostringstream os;
                    os << "Unknown socket sub-key '" << child.key
                       << "' on socket '" << sock.name << "' at line " << child.sourceLine;
                    warnings.push_back(os.str());
                }
            }
            if (!seenSocketNames.insert(sock.name).second) {
                std::ostringstream os;
                os << "Duplicate socket name '" << sock.name << "' at line " << sock.sourceLine;
                warnings.push_back(os.str());
            }
            out->sockets.push_back(std::move(sock));
        } else if (node.key == "bone_group") {
            auto toks = SplitWhitespace(node.value);
            if (toks.size() < 2) {
                std::ostringstream os;
                os << "bone_group needs at least name + mask at line " << node.sourceLine;
                warnings.push_back(os.str());
                continue;
            }
            BoneGroup g;
            g.name = toks[0];
            g.maskFlag = (toks[1] == "true" || toks[1] == "1");
            g.boneNames.assign(toks.begin() + 2, toks.end());
            g.sourceLine = node.sourceLine;
            if (!seenGroupNames.insert(g.name).second) {
                std::ostringstream os;
                os << "Duplicate bone_group name '" << g.name << "' at line " << g.sourceLine;
                warnings.push_back(os.str());
            }
            out->boneGroups.push_back(std::move(g));
        } else {
            KeyValue kv;
            kv.key = node.key;
            kv.value = node.value;
            kv.sourceLine = node.sourceLine;
            out->otherProperties.push_back(std::move(kv));
        }
    }
    return out;
}

// -----------------------------------------------------------------------
// SkinMesh: extract skin + *Shape overrides
// -----------------------------------------------------------------------
std::unique_ptr<SkinMeshBlock> BuildSkinMesh(const std::vector<AocRawNode>& properties,
                                              std::vector<std::string>& warnings) {
    (void)warnings;
    auto out = std::make_unique<SkinMeshBlock>();
    for (const AocRawNode& node : properties) {
        if (node.key == "skin") {
            out->skinPath = node.value;
            // Children of skin = shape overrides
            for (const AocRawNode& child : node.children) {
                if (child.key.size() >= 5 && child.key.rfind("Shape") == child.key.size() - 5) {
                    KeyValue kv;
                    kv.key = child.key;
                    kv.value = child.value;
                    kv.sourceLine = child.sourceLine;
                    out->shapeOverrides.push_back(std::move(kv));
                } else {
                    KeyValue kv;
                    kv.key = child.key;
                    kv.value = child.value;
                    kv.sourceLine = child.sourceLine;
                    out->otherProperties.push_back(std::move(kv));
                }
            }
        } else {
            KeyValue kv;
            kv.key = node.key;
            kv.value = node.value;
            kv.sourceLine = node.sourceLine;
            out->otherProperties.push_back(std::move(kv));
        }
    }
    return out;
}

// -----------------------------------------------------------------------
// AnimationController: extract on_event_* handlers
// -----------------------------------------------------------------------
std::unique_ptr<AnimCtrlBlock> BuildAnimCtrl(const std::vector<AocRawNode>& properties,
                                              std::vector<std::string>& warnings) {
    (void)warnings;
    auto out = std::make_unique<AnimCtrlBlock>();
    for (const AocRawNode& node : properties) {
        KeyValue kv;
        kv.key = node.key;
        kv.value = node.value;
        kv.sourceLine = node.sourceLine;
        if (node.key.rfind("on_event_", 0) == 0) {
            out->eventHandlers.push_back(std::move(kv));
        } else {
            out->otherProperties.push_back(std::move(kv));
        }
    }
    return out;
}

} // anonymous namespace

}} // namespace GGPKExp::Parsers
