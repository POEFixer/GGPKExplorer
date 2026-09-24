#include "SmdParser.h"
#include "../BinaryReader.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace GGPKExp { namespace Parsers {

// Port of game's sub_7FF7A4DD87C0 — computes per-field byte offset inside a
// vertex from the format bitmask. Field order is fixed by the game decoder
// (verified from decompiled asm at 0x7FF7A4DD87C0). Names reflect the actual
// D3D11 shader semantics — see semantic triples in sub_7FF7A4DD8A40.
//
// IMPORTANT: bit 0x008 in POE2 is TEXCOORD0 (the primary UV channel), NOT a
// color field. The original reverse-engineering notes (based on another game)
// mislabeled it as "Color" — verified by the sub_7FF7A4DD8A40 table entry
// (b1=16, b2=5, b3=0) where b2=5 maps to "TEXCOORD" in the string pool at
// 0x7FF7A6BE3200. This is why the parser already produced correct UVs for
// the 99.7% of files in format 0x23C.
SmdVertexLayout DecodeVertexLayout(uint32_t format) {
    SmdVertexLayout L{};
    L.format = format;
    uint32_t off = 0;
    if (format & 0x200) { L.posOff    = static_cast<int>(off); off += 12; }
    if (format & 0x010) { L.normOff   = static_cast<int>(off); off += 4;  }
    if (format & 0x020) { L.tanOff    = static_cast<int>(off); off += 4;  }
    if (format & 0x008) { L.uv0Off    = static_cast<int>(off); off += 4;  }
    if (format & 0x080) { L.uv1Off    = static_cast<int>(off); off += 8;  }
    if (format & 0x100) { L.uv2Off    = static_cast<int>(off); off += 8;  }
    if (format & 0x004) { L.boneIOff  = static_cast<int>(off);
                          L.boneWOff  = static_cast<int>(off + 4); off += 8; }
    if (format & 0x001) { L.color0Off = static_cast<int>(off); off += 4;  }
    if (format & 0x002) { L.color1Off = static_cast<int>(off); off += 4;  }
    if (format & 0x040) { L.color2Off = static_cast<int>(off); off += 4;  }
    L.stride = off;
    return L;
}

// Helper: read a single vertex honoring the per-field offsets supplied by the
// layout. Fully format-aware port of the game decoder — no hardcoded offsets.
// v.uvRaw gets TEXCOORD0 (bit 0x008), which for the dominant 0x23C format
// lands at bytes 20-23; earlier "synthetic UV" was actually the real UV all
// along. v.colorRaw takes the first available COLOR channel (0x001/0x002/
// 0x040) — most POE2 meshes have none, but the field is there when needed.
static SmdVertex ReadVertexAt(const uint8_t* data, size_t off,
                              const SmdVertexLayout& L, size_t size) {
    SmdVertex v;
    auto inRange = [&](int fieldOff, size_t n) {
        return fieldOff >= 0 &&
               off + static_cast<size_t>(fieldOff) + n <= size;
    };

    if (inRange(L.posOff, 12)) {
        std::memcpy(&v.x, data + off + L.posOff,     4);
        std::memcpy(&v.y, data + off + L.posOff + 4, 4);
        std::memcpy(&v.z, data + off + L.posOff + 8, 4);
    }
    if (inRange(L.normOff, 4)) {
        v.nx = data[off + L.normOff + 0];
        v.ny = data[off + L.normOff + 1];
        v.nz = data[off + L.normOff + 2];
        v.nw = data[off + L.normOff + 3];
    }
    if (inRange(L.tanOff, 4)) {
        v.tx = data[off + L.tanOff + 0];
        v.ty = data[off + L.tanOff + 1];
        v.tz = data[off + L.tanOff + 2];
        v.tw = data[off + L.tanOff + 3];
    }
    if (inRange(L.uv0Off, 4)) {
        // TEXCOORD0 — 4 bytes = two half-floats (matches renderer's
        // HalfToFloat(v.uvRaw[i]) consumption in MeshRenderer.cpp).
        std::memcpy(&v.uvRaw[0], data + off + L.uv0Off,     2);
        std::memcpy(&v.uvRaw[1], data + off + L.uv0Off + 2, 2);
    }

    // First available vertex-color channel, in game decoder order.
    int colorPick = (L.color0Off >= 0) ? L.color0Off
                  : (L.color1Off >= 0) ? L.color1Off
                  : L.color2Off;
    if (inRange(colorPick, 4)) {
        std::memcpy(v.colorRaw, data + off + colorPick, 4);
    }

    // Bone block order in POE2: BLENDINDICES first (boneIOff), BLENDWEIGHT
    // second (boneWOff). v.boneData preserves that packing (indices @0-3,
    // weights @4-7) — what MeshRenderer.cpp's vertex-upload path consumes.
    if (inRange(L.boneIOff, 4) && inRange(L.boneWOff, 4)) {
        std::memcpy(v.boneData,     data + off + L.boneIOff, 4);
        std::memcpy(v.boneData + 4, data + off + L.boneWOff, 4);
    }
    return v;
}

// =====================================================================
// Version 1-2: Real parser (sub_7FF7A6458A60)
// =====================================================================
static Result<SmdMesh> ParseSmdV1V2(const uint8_t* data, size_t size, SmdMesh& result) {
    size_t headerSize = (result.version == 1) ? 40 : 44;
    if (size < headerSize)
        return Result<SmdMesh>::Fail("File too small for v" + std::to_string(result.version) + " header");

    BinaryReader reader(data, size);
    reader.Skip(1); // version already read

    result.triangleCount = reader.Read<uint32_t>();
    result.vertexCount = reader.Read<uint32_t>();
    result.boneCount = reader.Read<uint8_t>();
    result.blendCount = reader.Read<uint16_t>();
    result.boneNameDataSize = reader.Read<uint32_t>();

    // Bounding box: 4 floats at offset 16 (minX, maxX, minY, maxY)
    result.bboxMin[0] = reader.Read<float>();
    result.bboxMax[0] = reader.Read<float>();
    result.bboxMin[1] = reader.Read<float>();
    result.bboxMax[1] = reader.Read<float>();

    // Z range from extra data
    float z1 = reader.Read<float>();
    float z2 = reader.Read<float>();
    if (std::isfinite(z1) && std::isfinite(z2) && std::fabs(z1) < 10000 && std::fabs(z2) < 10000) {
        result.bboxMin[2] = (std::min)(z1, z2);
        result.bboxMax[2] = (std::max)(z1, z2);
    }

    if (!reader.IsValid())
        return Result<SmdMesh>::Fail("Failed to read v" + std::to_string(result.version) + " header");

    // Sanity checks
    if (result.vertexCount > 10000000 || result.triangleCount > 10000000)
        return Result<SmdMesh>::Fail("Unreasonable counts");

    // Vertex stride: format 0x23C for v1 (= 32 bytes)
    // or from header byte 40-43 | 0x238 for v2
    uint32_t format = 0x23C; // v1 default
    if (result.version == 2 && headerSize >= 44) {
        uint32_t hdrFormat = 0;
        std::memcpy(&hdrFormat, data + 40, 4);
        format = hdrFormat | 0x238;
    }

    SmdVertexLayout layout = DecodeVertexLayout(format | 0x200);
    uint32_t stride = layout.stride;
    result.detectedStride = stride;
    result.vertexLayout = layout;

    // Compute data offsets
    size_t blendBytes = result.blendCount * 8;
    size_t boneNameBytes = result.boneNameDataSize;
    size_t idxStride = (result.vertexCount > 0xFFFF) ? 12 : 6;
    size_t idxBytes = result.triangleCount * idxStride;
    size_t vtxBytes = result.vertexCount * stride;

    size_t blendStart = headerSize;
    size_t boneNameStart = blendStart + blendBytes;
    size_t idxStart = boneNameStart + boneNameBytes;
    size_t vtxStart = idxStart + idxBytes;

    // Parse the blend block as (nameLengthInBytes, firstTriangleIndex) pairs.
    // Raw inspection of bittersoncorpse01.smd (v1, 4 submeshes) confirms:
    //   blend[0] = (36, 0)   — "bandages_meshShape" = 18 chars * 2 bytes
    //   blend[1] = (28, 211) — "body_meshShape"     = 14 chars * 2 bytes
    //   blend[2] = (28, 3238)
    //   blend[3] = (28, 4118)
    //   sum of lengths = 120 = nameSize, names stored contiguously WITHOUT null separators.
    //
    // NOTE (2026-04-15): an earlier attempt to reinterpret field0 as a byte OFFSET
    // and walk the name block as null-terminated strings failed — the real file
    // has no null terminators. The length-based slicing below is correct; the
    // `boneNames` field in SmdMesh stays empty until a proper IDA RE session
    // reveals how V01/V02 local bone indices are actually resolved.
    struct BlendEntry {
        uint32_t nameLenBytes;
        uint32_t firstTriIdx;
    };
    std::vector<BlendEntry> blendEntries;
    if (result.blendCount > 0 && blendStart + blendBytes <= size) {
        blendEntries.reserve(result.blendCount);
        for (uint32_t i = 0; i < result.blendCount; ++i) {
            BlendEntry e{};
            std::memcpy(&e.nameLenBytes, data + blendStart + i * 8,     4);
            std::memcpy(&e.firstTriIdx,  data + blendStart + i * 8 + 4, 4);
            blendEntries.push_back(e);
        }
    }

    // Read a fixed-byte-length UTF-16LE slice starting at `off`. Printable ASCII
    // only, matching the parser's conservative decoding policy.
    auto readUtf16Ascii = [&](size_t off, size_t bytes) -> std::string {
        std::string s;
        s.reserve(bytes / 2);
        size_t end = (std::min)(off + bytes, size);
        for (size_t i = off; i + 1 < end; i += 2) {
            uint16_t ch = 0;
            std::memcpy(&ch, data + i, 2);
            if (ch >= 0x20 && ch < 0x7f)
                s.push_back(static_cast<char>(ch));
        }
        return s;
    };

    // Slice name block by each submesh's declared length.
    {
        size_t cursor = boneNameStart;
        const size_t nameEnd = boneNameStart + boneNameBytes;
        result.submeshNames.reserve(blendEntries.size());
        for (const auto& e : blendEntries) {
            if (cursor >= nameEnd) break;
            size_t take = (std::min<size_t>)(e.nameLenBytes, nameEnd - cursor);
            result.submeshNames.push_back(readUtf16Ascii(cursor, take));
            cursor += take;
        }
    }

    // Legacy aggregate — first submesh name, or whole-block concat when no blend
    // entries were present.
    if (!result.submeshNames.empty()) {
        result.submeshName = result.submeshNames.front();
    } else {
        result.submeshName = readUtf16Ascii(boneNameStart, boneNameBytes);
    }

    // Read index data (16-bit or 32-bit depending on vertex count)
    if (idxStart + idxBytes <= size) {
        result.indices.reserve(result.triangleCount * 3);
        if (idxStride == 6) {
            // 16-bit indices
            for (uint32_t i = 0; i < result.triangleCount * 3; i++) {
                uint16_t idx;
                std::memcpy(&idx, data + idxStart + i * 2, 2);
                result.indices.push_back(static_cast<uint32_t>(idx));
            }
        } else {
            // 32-bit indices (idxStride == 12)
            for (uint32_t i = 0; i < result.triangleCount * 3; i++) {
                uint32_t idx;
                std::memcpy(&idx, data + idxStart + i * 4, 4);
                result.indices.push_back(idx);
            }
        }
    }

    // Read vertex data
    if (vtxStart + vtxBytes <= size) {
        result.vertices.reserve(result.vertexCount);
        for (uint32_t i = 0; i < result.vertexCount; i++) {
            result.vertices.push_back(ReadVertexAt(data, vtxStart + i * stride, layout, size));
        }
    } else if (vtxStart < size) {
        // Partial read
        uint32_t availVerts = static_cast<uint32_t>((size - vtxStart) / stride);
        result.vertices.reserve(availVerts);
        for (uint32_t i = 0; i < availVerts; i++) {
            size_t off = vtxStart + i * stride;
            if (off + 12 > size) break;
            result.vertices.push_back(ReadVertexAt(data, off, layout, size));
        }
    }

    if (result.vertices.empty())
        return Result<SmdMesh>::Fail("No vertex data found at computed offset");

    // Build per-submesh index ranges from the blend block. All submeshes share
    // the single vertex buffer in v01/v02 — only the index slice differs, which
    // is enough to bind different materials per submesh at draw time.
    const uint32_t totalIndices = static_cast<uint32_t>(result.indices.size());
    const uint32_t totalVerts   = static_cast<uint32_t>(result.vertices.size());
    if (!blendEntries.empty()) {
        result.submeshRanges.reserve(blendEntries.size());
        for (size_t i = 0; i < blendEntries.size(); ++i) {
            SmdSubmeshRange r;
            r.indexOffset  = blendEntries[i].firstTriIdx * 3;
            uint32_t nextStart = (i + 1 < blendEntries.size())
                ? blendEntries[i + 1].firstTriIdx * 3
                : totalIndices;
            r.indexCount   = (nextStart > r.indexOffset) ? nextStart - r.indexOffset : 0;
            r.vertexOffset = 0;
            r.vertexCount  = totalVerts;
            result.submeshRanges.push_back(r);
        }
    } else {
        // No blend block — fall back to the old single-submesh contract.
        SmdSubmeshRange r;
        r.indexOffset = 0;
        r.indexCount  = totalIndices;
        r.vertexOffset = 0;
        r.vertexCount  = totalVerts;
        result.submeshRanges.push_back(r);
    }

    return Result<SmdMesh>::Ok(std::move(result));
}

// =====================================================================
// Version 3+ / 9+: DOLm format (parsed by sub_7FF7A645D780)
// =====================================================================
static Result<SmdMesh> ParseSmdDOLm(const uint8_t* data, size_t size, SmdMesh& result) {
    // Scan for "DOLm" magic -- don't hardcode offset (varies by version/pre-header)
    size_t dolmOffset = 0;
    bool dolmFound = false;
    size_t maxScan = (std::min)(size, static_cast<size_t>(512));
    for (size_t i = 0; i + 3 < maxScan; ++i) {
        if (data[i] == 'D' && data[i+1] == 'O' && data[i+2] == 'L' && data[i+3] == 'm') {
            dolmOffset = i;
            dolmFound = true;
            break;
        }
    }
    if (!dolmFound)
        return Result<SmdMesh>::Fail("DOLm magic not found in first 512 bytes");

    if (size < dolmOffset + 13)
        return Result<SmdMesh>::Fail("File too small for DOLm header");

    // DOLm header (13 bytes):
    // [0-3]  "DOLm" magic
    // [4-5]  uint16 dolm_version
    // [6]    uint8  submesh_count
    // [7-8]  uint16 bone_data_entry_count
    // [9-12] uint32 vertex_format_bitmask
    uint16_t dolmVersion;
    std::memcpy(&dolmVersion, data + dolmOffset + 4, 2);
    uint8_t submeshCount = data[dolmOffset + 6];
    uint16_t boneDataEntryCount;
    std::memcpy(&boneDataEntryCount, data + dolmOffset + 7, 2);
    uint32_t vertexFormat;
    std::memcpy(&vertexFormat, data + dolmOffset + 9, 4);
    vertexFormat |= 0x200;

    // Store DOLm diagnostic info
    result.dolmOffset = static_cast<uint32_t>(dolmOffset);
    result.dolmVersion = dolmVersion;
    result.boneDataEntryCount = boneDataEntryCount;
    result.submeshCount = submeshCount;

    SmdVertexLayout layout = DecodeVertexLayout(vertexFormat);
    uint32_t stride = layout.stride;
    result.detectedStride = stride;
    result.vertexLayout = layout;

    size_t tableOff = dolmOffset + 13;
    if (tableOff + submeshCount * 8 > size)
        return Result<SmdMesh>::Fail("Submesh table past end");

    // Read all submeshes sequentially
    if (submeshCount > 0) {
        // First pass: read submesh table to compute totals
        uint32_t totalTris = 0, totalVerts = 0;
        struct SubInfo { uint32_t tris; uint32_t verts; };
        std::vector<SubInfo> subInfos(submeshCount);
        for (uint8_t s = 0; s < submeshCount; ++s) {
            std::memcpy(&subInfos[s].tris, data + tableOff + s * 8, 4);
            std::memcpy(&subInfos[s].verts, data + tableOff + s * 8 + 4, 4);
            totalTris += subInfos[s].tris;
            totalVerts += subInfos[s].verts;
        }
        result.triangleCount = totalTris;
        result.vertexCount = totalVerts;

        result.indices.reserve(totalTris * 3);
        result.vertices.reserve(totalVerts);

        // Second pass: read data for each submesh sequentially
        size_t cursor = tableOff + submeshCount * 8;
        uint32_t runningIdxOff = 0;
        uint32_t runningVtxOff = 0;

        for (uint8_t s = 0; s < submeshCount; ++s) {
            uint32_t sTris = subInfos[s].tris;
            uint32_t sVerts = subInfos[s].verts;
            uint32_t idxEntrySize = (sVerts > 0xFFFF) ? 12 : 6;
            size_t idxBytes = sTris * idxEntrySize;
            size_t vtxBytes = sVerts * stride;

            // Step 1: Skip bone entry table (8 bytes per bone entry)
            cursor += boneDataEntryCount * 8;

            SmdSubmeshRange range;
            range.indexOffset = runningIdxOff;
            range.indexCount = sTris * 3;
            range.vertexOffset = runningVtxOff;
            range.vertexCount = sVerts;

            // Step 2: Read indices (rebased to global vertex array)
            if (cursor + idxBytes <= size) {
                for (uint32_t t = 0; t < sTris; ++t) {
                    size_t tOff = cursor + t * idxEntrySize;
                    for (int i = 0; i < 3; ++i) {
                        if (idxEntrySize == 6) {
                            uint16_t idx;
                            std::memcpy(&idx, data + tOff + i * 2, 2);
                            result.indices.push_back(static_cast<uint32_t>(idx) + runningVtxOff);
                        } else {
                            uint32_t idx32;
                            std::memcpy(&idx32, data + tOff + i * 4, 4);
                            result.indices.push_back(idx32 + runningVtxOff);
                        }
                    }
                }
            }
            cursor += idxBytes;

            // Read vertices
            if (cursor + vtxBytes <= size) {
                for (uint32_t i = 0; i < sVerts; ++i) {
                    result.vertices.push_back(ReadVertexAt(data, cursor + i * stride, layout, size));
                }
            }
            cursor += vtxBytes;

            // Step 4: Skip full bone data (per-submesh, after vertex data).
            // IDA sub_7FF7A645D010 at 0x7FF7A645D295 performs this skip only
            // when bit 0x001 is set in the format bitmask — the `jnb
            // [rsp+0x64], ecx` compares that field's offset against stride and
            // jumps past the skip when it is absent. The bit represents the
            // COLOR3 channel in POE2's shader binding (verified against the
            // semantic table in sub_7FF7A4DD8A40) — earlier notes labeled it
            // "Extra0". An unconditional skip desynchronizes submesh N+1 by
            // bones*40 (v<3) or bones*36 (v>=3).
            const bool hasColor3 = (vertexFormat & 0x001) != 0;
            if (hasColor3 && boneDataEntryCount > 0) {
                size_t boneEntrySize = (dolmVersion >= 3) ? 36 : 40;
                cursor += static_cast<size_t>(boneDataEntryCount) * boneEntrySize;
            }

            // Step 5: Skip adjacency data (DOLm version >= 4)
            if (dolmVersion >= 4 && cursor + 4 <= size) {
                uint32_t adjSize;
                std::memcpy(&adjSize, data + cursor, 4);
                cursor += 4;
                if (adjSize > 0) {
                    size_t adjEntrySize = (sVerts > 0xFFFF) ? 4 : 2;
                    cursor += static_cast<size_t>(adjSize) * adjSize * adjEntrySize;
                }
            }

            result.submeshRanges.push_back(range);
            runningIdxOff += sTris * 3;
            runningVtxOff += sVerts;
        }

        // Compute bounding box -- skip degenerate vertices (FLT_MAX sentinels, NaN, Inf)
        if (!result.vertices.empty()) {
            constexpr float kMax = 1e6f;
            bool init = false;
            for (const auto& vt : result.vertices) {
                if (!std::isfinite(vt.x) || !std::isfinite(vt.y) || !std::isfinite(vt.z)) continue;
                if (std::fabs(vt.x) > kMax || std::fabs(vt.y) > kMax || std::fabs(vt.z) > kMax) continue;
                if (!init) {
                    result.bboxMin[0] = result.bboxMax[0] = vt.x;
                    result.bboxMin[1] = result.bboxMax[1] = vt.y;
                    result.bboxMin[2] = result.bboxMax[2] = vt.z;
                    init = true; continue;
                }
                if (vt.x < result.bboxMin[0]) result.bboxMin[0] = vt.x;
                if (vt.x > result.bboxMax[0]) result.bboxMax[0] = vt.x;
                if (vt.y < result.bboxMin[1]) result.bboxMin[1] = vt.y;
                if (vt.y > result.bboxMax[1]) result.bboxMax[1] = vt.y;
                if (vt.z < result.bboxMin[2]) result.bboxMin[2] = vt.z;
                if (vt.z > result.bboxMax[2]) result.bboxMax[2] = vt.z;
            }
        }
    }

    return Result<SmdMesh>::Ok(std::move(result));
}

Result<SmdMesh> ParseSmd(const uint8_t* data, size_t size) {
    if (!data || size < 40)
        return Result<SmdMesh>::Fail("File too small");

    SmdMesh result;
    result.version = data[0];

    if (result.version == 1 || result.version == 2)
        return ParseSmdV1V2(data, size, result);
    else if (result.version >= 3)
        return ParseSmdDOLm(data, size, result);
    else
        return Result<SmdMesh>::Fail("Unknown version: " + std::to_string(result.version));
}

}} // namespace GGPKExp::Parsers
