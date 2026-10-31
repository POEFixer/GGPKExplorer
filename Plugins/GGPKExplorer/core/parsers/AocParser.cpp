#include "AocParser.h"

#include <sstream>

namespace GGPKExp { namespace Parsers {

namespace {

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

        out.blocks.push_back(std::move(block));
    }

    return Result<AocFile>::Ok(std::move(out));
}

}} // namespace GGPKExp::Parsers
