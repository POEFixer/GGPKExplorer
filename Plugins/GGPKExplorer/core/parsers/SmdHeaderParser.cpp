#include "SmdHeaderParser.h"
#include "../BinaryReader.h"

namespace GGPKExp { namespace Parsers {

Result<SmdHeaderInfo> ParseSmdHeader(const uint8_t* data, size_t size) {
    if (!data || size < 36)
        return Result<SmdHeaderInfo>::Fail("Data too small for SMD header file (need at least 36 bytes)");

    BinaryReader reader(data, size);
    SmdHeaderInfo result;

    // Read structured fields from first 36 bytes
    result.vertexStride = reader.Read<uint32_t>();
    result.indexStride = reader.Read<uint32_t>();

    // Sanity check strides -- allow 0 (placeholder meshes have no geometry)
    if (result.vertexStride > 256)
        return Result<SmdHeaderInfo>::Fail("Invalid vertex stride: " + std::to_string(result.vertexStride));
    if (result.indexStride > 64)
        return Result<SmdHeaderInfo>::Fail("Invalid index stride: " + std::to_string(result.indexStride));

    // Skip 4 bytes (offset 8-11)
    reader.Skip(4);

    // Bounding box: 6 floats at offset 12
    result.bboxMin[0] = reader.Read<float>();
    result.bboxMin[1] = reader.Read<float>();
    result.bboxMin[2] = reader.Read<float>();
    result.bboxMax[0] = reader.Read<float>();
    result.bboxMax[1] = reader.Read<float>();
    result.bboxMax[2] = reader.Read<float>();
    if (!reader.IsValid())
        return Result<SmdHeaderInfo>::Fail("Failed to read SMD header bounding box");

    // Scan remaining bytes for ASCII strings > 2 chars (submesh names)
    std::string current;
    for (size_t i = 36; i < size; i++) {
        uint8_t ch = data[i];
        if (ch >= 0x20 && ch < 0x7F) {
            current.push_back(static_cast<char>(ch));
        } else {
            if (current.size() > 2) {
                result.submeshNames.push_back(current);
            }
            current.clear();
        }
    }
    // Don't forget the last string if file doesn't end with a non-printable
    if (current.size() > 2) {
        result.submeshNames.push_back(current);
    }

    return Result<SmdHeaderInfo>::Ok(std::move(result));
}

}} // namespace GGPKExp::Parsers
