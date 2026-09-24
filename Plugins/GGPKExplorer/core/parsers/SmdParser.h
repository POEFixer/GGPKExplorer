#pragma once

#include "../Result.h"
#include <cstdint>
#include <string>
#include <vector>

namespace GGPKExp { namespace Parsers {

struct SmdVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    uint8_t nx = 0, ny = 0, nz = 0, nw = 0;
    uint8_t tx = 0, ty = 0, tz = 0, tw = 0;
    uint8_t colorRaw[4] = {};  // first available COLOR channel (bit 0x001/0x002/0x040)
    uint16_t uvRaw[2] = {};    // TEXCOORD0 half-floats (bit 0x008); for format 0x23C
                               // this lands at bytes 20-23 of the vertex block
    uint8_t boneData[8] = {};  // BLENDINDICES [0..3] + BLENDWEIGHT [4..7]
};

// Per-field byte offsets within a vertex, derived from the format bitmask.
// Port of the game's sub_7FF7A4DD87C0 (vertex format decoder). Field names
// reflect the actual D3D11 shader semantics POE2 binds each bit to — verified
// via the (b1, b2, b3) semantic triples in sub_7FF7A4DD8A40 and the semantic
// string table at 0x7FF7A6BE3200 (POSITION, TEXCOORD, COLOR, ...). A value of
// -1 means the corresponding field is absent for this format.
struct SmdVertexLayout {
    int posOff    = -1;  // 0x200 → POSITION0      (12 bytes, 3×float)
    int normOff   = -1;  // 0x010 → NORMAL0        (4 bytes, UBYTE4N)
    int tanOff    = -1;  // 0x020 → TANGENT0       (4 bytes, UBYTE4N)
    int uv0Off    = -1;  // 0x008 → TEXCOORD0      (4 bytes, 2×half — primary UV)
    int uv1Off    = -1;  // 0x080 → TEXCOORD1      (8 bytes — secondary UV / lightmap)
    int uv2Off    = -1;  // 0x100 → TEXCOORD2      (8 bytes)
    int boneIOff  = -1;  // 0x004 → BLENDINDICES0  (4 bytes, first half of 0x004 block)
    int boneWOff  = -1;  // 0x004 → BLENDWEIGHT0   (4 bytes, second half of 0x004 block)
    int color0Off = -1;  // 0x001 → COLOR3         (4 bytes — gates post-vert bone skip)
    int color1Off = -1;  // 0x002 → COLOR0         (4 bytes)
    int color2Off = -1;  // 0x040 → COLOR4         (4 bytes)
    uint32_t stride = 0;
    uint32_t format = 0;
};

// Computes per-field byte offsets from the format bitmask. The field order
// must match the game's decoder exactly (position, normal, tangent, color,
// uv0, uv1, bones, extra0, extra1, extra2).
SmdVertexLayout DecodeVertexLayout(uint32_t format);

struct SmdSubmeshRange {
    uint32_t indexOffset = 0;   // starting index in indices[]
    uint32_t indexCount = 0;    // number of indices
    uint32_t vertexOffset = 0;  // starting vertex in vertices[]
    uint32_t vertexCount = 0;
};

struct SmdMesh {
    uint8_t version = 0;
    uint32_t triangleCount = 0;
    uint32_t vertexCount = 0;
    uint16_t blendCount = 0;
    uint32_t boneNameDataSize = 0;
    float bboxMin[3] = {};
    float bboxMax[3] = {};
    uint8_t boneCount = 0;
    uint32_t detectedStride = 0;

    // DOLm diagnostic fields
    uint32_t dolmOffset = 0;        // offset where DOLm magic was found
    uint16_t dolmVersion = 0;
    uint16_t boneDataEntryCount = 0;
    uint8_t submeshCount = 0;

    std::vector<uint32_t> indices;
    std::vector<SmdVertex> vertices;
    std::vector<SmdSubmeshRange> submeshRanges; // per-submesh draw ranges
    std::string submeshName;                    // legacy — mirrors submeshNames[0] when present

    // Reserved for future V01/V02 bone-name remap work. Currently NOT populated —
    // initial reinterpretation of boneNameData as null-terminated strings failed
    // (real files concatenate names without separators), and the true semantics
    // of V01/V02 `boneCount` + BLENDINDICES still need IDA RE of sub_7FF7A6458A60.
    // MeshViewer's m_boneRemap consumes this only when non-empty.
    std::vector<std::string> boneNames;

    // V01/V02 per-submesh display names. Empty for DOLm/V3+ meshes.
    // Ordered to match `submeshRanges[]` — one entry per submesh.
    std::vector<std::string> submeshNames;

    // Fully decoded vertex layout for the format used by this mesh. Populated
    // for both v1/v2 and DOLm paths, so consumers (renderer, debug UI, plugin
    // tools) can tell which fields were actually present in the source stream.
    SmdVertexLayout vertexLayout;
};

Result<SmdMesh> ParseSmd(const uint8_t* data, size_t size);

}} // namespace GGPKExp::Parsers
