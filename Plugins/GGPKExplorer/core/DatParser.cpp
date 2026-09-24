#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include "DatParser.h"
#include <cstring>
#include <stdexcept>

namespace GGPKExp {

uint32_t ParseU8Value(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    const size_t last = text.find_last_not_of(" \t\r\n");
    if (first == std::string::npos || text[first] == '-')
        throw std::invalid_argument("u8 requires an integer in 0..255");
    const std::string value = text.substr(first, last - first + 1);
    size_t used = 0;
    const auto number = std::stoull(value, &used, 0);
    if (used != value.size() || number > 255)
        throw std::out_of_range("u8 requires an integer in 0..255");
    return static_cast<uint32_t>(number);
}

uint8_t EncodeU8Value(const CellValue& value) {
    if (std::holds_alternative<std::monostate>(value)) return 0;
    if (!std::holds_alternative<uint32_t>(value) || std::get<uint32_t>(value) > 255)
        throw std::out_of_range("u8 requires an unsigned integer in 0..255");
    return static_cast<uint8_t>(std::get<uint32_t>(value));
}

int GetTypeSize(ColumnType type) {
    switch (type) {
    case ColumnType::Bool:       return 1;
    case ColumnType::U8:         return 1;
    case ColumnType::I16:        return 2;
    case ColumnType::U16:        return 2;
    case ColumnType::I32:        return 4;
    case ColumnType::U32:        return 4;
    case ColumnType::F32:        return 4;
    case ColumnType::EnumRow:    return 4;
    case ColumnType::String:     return 8;
    case ColumnType::Row:        return 8;
    case ColumnType::ForeignRow: return 16;
    case ColumnType::Array:      return 16;
    default:                     return 4;
    }
}

int GetColumnByteSize(const ColumnDef& col) {
    if (col.isArray) return 16;
    int base = GetTypeSize(col.type);
    return col.isInterval ? base * 2 : base;
}

DatFile Parse(const std::vector<uint8_t>& data) {
    DatFile result;
    if (data.size() < 4) {
        result.error = "File too small";
        return result;
    }

    result.rawData = data;
    std::memcpy(&result.rowCount, data.data(), 4);
    result.fixedOffset = 4;

    if (result.rowCount == 0) {
        // Empty table: boundary may be right after row count
        const uint64_t boundary = 0xBBBBBBBBBBBBBBBB;
        if (data.size() >= 12) {
            uint64_t val = 0;
            std::memcpy(&val, data.data() + 4, 8);
            if (val == boundary) {
                result.varOffset = 4;
                result.rowLength = 0;
                result.valid = true;
                return result;
            }
        }
        result.varOffset = 4;
        result.rowLength = 0;
        result.valid = true;
        return result;
    }

    // Find all occurrences of the 8-byte boundary marker
    const uint64_t boundary = 0xBBBBBBBBBBBBBBBB;
    size_t alignedPos = 0;
    size_t firstPos = 0;

    for (size_t pos = 4; pos + 8 <= data.size(); pos++) {
        uint64_t val = 0;
        std::memcpy(&val, data.data() + pos, 8);
        if (val == boundary) {
            if (firstPos == 0) firstPos = pos;
            // Check alignment
            if ((pos - 4) % result.rowCount == 0) {
                alignedPos = pos;
                break;
            }
        }
    }

    // Prefer aligned position, fall back to first occurrence
    size_t boundaryPos = alignedPos > 0 ? alignedPos : firstPos;
    if (boundaryPos == 0) {
        result.error = "Boundary marker 0xBBBB...BB not found";
        return result;
    }

    uint32_t fixedSize = static_cast<uint32_t>(boundaryPos - 4);
    result.rowLength = fixedSize / result.rowCount;
    result.varOffset = static_cast<uint32_t>(boundaryPos);

    if (result.rowLength == 0) {
        result.error = "Computed row length is 0";
        return result;
    }

    result.valid = true;
    return result;
}

std::string DecodeString(const DatFile& dat, uint64_t varSectionOffset) {
    size_t absOffset = dat.varOffset + static_cast<size_t>(varSectionOffset);
    if (absOffset >= dat.rawData.size()) return "";

    std::wstring ws;
    while (absOffset + 2 <= dat.rawData.size()) {
        uint16_t ch = 0;
        std::memcpy(&ch, dat.rawData.data() + absOffset, 2);
        if (ch == 0) break;
        ws.push_back(static_cast<wchar_t>(ch));
        absOffset += 2;
    }

    if (ws.empty()) return "";
    int needed = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return "";
    std::string utf8(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

CellValue DecodeCell(const DatFile& dat, uint32_t rowIndex, uint32_t columnOffset, const ColumnDef& col) {
    size_t rowStart = dat.fixedOffset + static_cast<size_t>(rowIndex) * dat.rowLength;
    size_t offset = rowStart + columnOffset;

    if (col.isArray) {
        if (offset + 16 > dat.rawData.size()) return std::monostate{};
        uint64_t count = 0, arrOffset = 0;
        std::memcpy(&count, dat.rawData.data() + offset, 8);
        std::memcpy(&arrOffset, dat.rawData.data() + offset + 8, 8);

        if (count == 0 || count > 100000 || offset + count * 8 > dat.rawData.size()) {
            return CellArray{};
        }

        int elemSize = GetTypeSize(col.type);
        size_t absStart = dat.varOffset + static_cast<size_t>(arrOffset);
        CellArray result;
        auto& arr = result.items;
        arr.reserve(static_cast<size_t>(count));

        for (uint64_t i = 0; i < count; i++) {
            size_t elemOff = absStart + static_cast<size_t>(i) * elemSize;
            if (elemOff + elemSize > dat.rawData.size()) break;

            switch (col.type) {
            case ColumnType::U8:
                arr.emplace_back(static_cast<uint32_t>(dat.rawData[elemOff]));
                break;
            case ColumnType::Bool: {
                uint8_t v = dat.rawData[elemOff];
                arr.emplace_back(static_cast<bool>(v != 0));
                break;
            }
            case ColumnType::I16: {
                int16_t v = 0; std::memcpy(&v, dat.rawData.data() + elemOff, 2);
                arr.emplace_back(v);
                break;
            }
            case ColumnType::U16: {
                uint16_t v = 0; std::memcpy(&v, dat.rawData.data() + elemOff, 2);
                arr.emplace_back(v);
                break;
            }
            case ColumnType::I32: case ColumnType::EnumRow: {
                int32_t v = 0; std::memcpy(&v, dat.rawData.data() + elemOff, 4);
                arr.emplace_back(v);
                break;
            }
            case ColumnType::U32: {
                uint32_t v = 0; std::memcpy(&v, dat.rawData.data() + elemOff, 4);
                arr.emplace_back(v);
                break;
            }
            case ColumnType::F32: {
                float v = 0; std::memcpy(&v, dat.rawData.data() + elemOff, 4);
                arr.emplace_back(v);
                break;
            }
            case ColumnType::String: {
                uint64_t strOff = 0; std::memcpy(&strOff, dat.rawData.data() + elemOff, 8);
                arr.emplace_back(DecodeString(dat, strOff));
                break;
            }
            case ColumnType::Row: {
                int64_t v = 0; std::memcpy(&v, dat.rawData.data() + elemOff, 8);
                arr.emplace_back(v);
                break;
            }
            case ColumnType::ForeignRow: {
                int64_t v = 0; std::memcpy(&v, dat.rawData.data() + elemOff, 8);
                arr.emplace_back(v);
                break;
            }
            default:
                arr.emplace_back(std::monostate{});
                break;
            }
        }
        return result;
    }

    switch (col.type) {
    case ColumnType::U8:
        if (offset + 1 > dat.rawData.size()) return std::monostate{};
        return static_cast<uint32_t>(dat.rawData[offset]);
    case ColumnType::Bool: {
        if (offset + 1 > dat.rawData.size()) return std::monostate{};
        uint8_t v = dat.rawData[offset];
        return static_cast<bool>(v != 0);
    }
    case ColumnType::I16: {
        if (offset + 2 > dat.rawData.size()) return std::monostate{};
        int16_t v = 0; std::memcpy(&v, dat.rawData.data() + offset, 2);
        return v;
    }
    case ColumnType::U16: {
        if (offset + 2 > dat.rawData.size()) return std::monostate{};
        uint16_t v = 0; std::memcpy(&v, dat.rawData.data() + offset, 2);
        return v;
    }
    case ColumnType::I32: case ColumnType::EnumRow: {
        if (offset + 4 > dat.rawData.size()) return std::monostate{};
        int32_t v = 0; std::memcpy(&v, dat.rawData.data() + offset, 4);
        return v;
    }
    case ColumnType::U32: {
        if (offset + 4 > dat.rawData.size()) return std::monostate{};
        uint32_t v = 0; std::memcpy(&v, dat.rawData.data() + offset, 4);
        return v;
    }
    case ColumnType::F32: {
        if (offset + 4 > dat.rawData.size()) return std::monostate{};
        float v = 0; std::memcpy(&v, dat.rawData.data() + offset, 4);
        return v;
    }
    case ColumnType::String: {
        if (offset + 8 > dat.rawData.size()) return std::monostate{};
        uint64_t strOff = 0; std::memcpy(&strOff, dat.rawData.data() + offset, 8);
        if (strOff == NULL_MARKER_64) return std::string("null");
        return DecodeString(dat, strOff);
    }
    case ColumnType::Row: {
        if (offset + 8 > dat.rawData.size()) return std::monostate{};
        uint64_t raw = 0; std::memcpy(&raw, dat.rawData.data() + offset, 8);
        if (raw == NULL_MARKER_64) return std::monostate{};
        int64_t v = static_cast<int64_t>(raw);
        return v;
    }
    case ColumnType::ForeignRow: {
        if (offset + 16 > dat.rawData.size()) return std::monostate{};
        uint64_t raw = 0; std::memcpy(&raw, dat.rawData.data() + offset, 8);
        if (raw == NULL_MARKER_64) return std::monostate{};
        int64_t v = static_cast<int64_t>(raw);
        return v;
    }
    default:
        return std::monostate{};
    }
}

// Helper: encode UTF-8 string to UTF-16LE bytes with 4-byte null terminator
static void WriteUtf16String(std::vector<uint8_t>& varData, const std::string& str) {
    for (size_t i = 0; i < str.size(); ) {
        uint32_t cp = 0;
        uint8_t ch = static_cast<uint8_t>(str[i]);
        if (ch < 0x80) { cp = ch; i += 1; }
        else if ((ch & 0xE0) == 0xC0) { cp = ch & 0x1F; if (i + 1 < str.size()) cp = (cp << 6) | (str[i + 1] & 0x3F); i += 2; }
        else if ((ch & 0xF0) == 0xE0) { cp = ch & 0x0F; if (i + 2 < str.size()) { cp = (cp << 6) | (str[i + 1] & 0x3F); cp = (cp << 6) | (str[i + 2] & 0x3F); } i += 3; }
        else { cp = ch & 0x07; if (i + 3 < str.size()) { cp = (cp << 6) | (str[i + 1] & 0x3F); cp = (cp << 6) | (str[i + 2] & 0x3F); cp = (cp << 6) | (str[i + 3] & 0x3F); } i += 4; }

        if (cp <= 0xFFFF) {
            uint16_t w = static_cast<uint16_t>(cp);
            varData.push_back(static_cast<uint8_t>(w & 0xFF));
            varData.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));
        } else {
            // Surrogate pair
            cp -= 0x10000;
            uint16_t hi = static_cast<uint16_t>(0xD800 + (cp >> 10));
            uint16_t lo = static_cast<uint16_t>(0xDC00 + (cp & 0x3FF));
            varData.push_back(static_cast<uint8_t>(hi & 0xFF)); varData.push_back(static_cast<uint8_t>((hi >> 8) & 0xFF));
            varData.push_back(static_cast<uint8_t>(lo & 0xFF)); varData.push_back(static_cast<uint8_t>((lo >> 8) & 0xFF));
        }
    }
    // 4-byte null terminator
    varData.push_back(0); varData.push_back(0); varData.push_back(0); varData.push_back(0);
}

static void WriteFixed(std::vector<uint8_t>& fixed, const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    fixed.insert(fixed.end(), p, p + size);
}

std::vector<uint8_t> Serialize(const std::vector<std::vector<CellValue>>& rows,
                               const std::vector<ColumnDef>& columns) {
    uint32_t rowCount = static_cast<uint32_t>(rows.size());

    // Calculate row length from schema
    uint32_t rowLength = 0;
    for (const auto& col : columns)
        rowLength += static_cast<uint32_t>(GetColumnByteSize(col));

    // Build fixed and variable sections
    std::vector<uint8_t> fixedData;
    std::vector<uint8_t> varData;
    fixedData.reserve(static_cast<size_t>(rowCount) * rowLength);

    // Var-section offsets are relative to boundaryPos (start of the 0xBB...BB
    // 8-byte marker); the marker itself occupies the first 8 bytes, so real
    // .datc64 files never emit string/array offsets below 8. We therefore
    // bias all emitted offsets by +8 so that e.g. the first string lives at
    // disk offset 8 (= varData index 0, which is the first byte past the
    // marker in the assembled output).
    constexpr uint64_t kVarOffsetBias = 8;

    // Empty-string ("") and null-sentinel ("null") disambiguation:
    //   - Parse decodes NULL_MARKER_64 (0xFE…) → "null".
    //   - Parse decodes a pointer to a 4-byte null terminator → "".
    // To round-trip correctly we must NOT collapse both back into
    // NULL_MARKER_64. We materialise a single 4-byte null terminator inside
    // varData on first demand and share its biased offset for every "".
    bool emptyStringEmitted = false;
    uint64_t emptyStringOffset = 0;
    auto emitEmptyString = [&]() -> uint64_t {
        if (!emptyStringEmitted) {
            emptyStringOffset = varData.size() + kVarOffsetBias;
            varData.push_back(0); varData.push_back(0); varData.push_back(0); varData.push_back(0);
            emptyStringEmitted = true;
        }
        return emptyStringOffset;
    };

    // Track string offsets: collect all strings/arrays first, then write
    for (uint32_t r = 0; r < rowCount; r++) {
        const auto& row = rows[r];
        for (size_t c = 0; c < columns.size(); c++) {
            const auto& col = columns[c];
            const auto& val = (c < row.size()) ? row[c] : CellValue{};

            if (col.isArray) {
                // Array: write count + offset into var section
                if (std::holds_alternative<CellArray>(val)) {
                    const auto& arr = std::get<CellArray>(val);
                    uint64_t count = arr.items.size();
                    uint64_t offset = varData.size() + kVarOffsetBias;
                    WriteFixed(fixedData, &count, 8);
                    WriteFixed(fixedData, &offset, 8);
                    // Write array elements into var section.
                    // Element stride is driven by col.type (matching DecodeCell's
                    // GetTypeSize(col.type) stride), NOT by the runtime variant —
                    // this is critical for ForeignRow (16-byte stride: 8 primary
                    // + 8 zero pad) where a naive "holds_alternative<int64_t>"
                    // branch would emit only 8 bytes and desynchronise all
                    // subsequent elements.
                    // For string arrays: two-pass — first collect string offsets, then patch
                    std::vector<size_t> strArrayFixups; // positions in varData to patch with string offsets
                    for (const auto& elem : arr.items) {
                        switch (col.type) {
                        case ColumnType::U8: {
                            const uint8_t v = EncodeU8Value(elem);
                            WriteFixed(varData, &v, 1);
                            break;
                        }
                        case ColumnType::Bool: {
                            uint8_t v = std::holds_alternative<bool>(elem) ? (std::get<bool>(elem) ? 1 : 0) : 0;
                            WriteFixed(varData, &v, 1);
                            break;
                        }
                        case ColumnType::I16: {
                            int16_t v = std::holds_alternative<int16_t>(elem) ? std::get<int16_t>(elem) : 0;
                            WriteFixed(varData, &v, 2);
                            break;
                        }
                        case ColumnType::U16: {
                            uint16_t v = std::holds_alternative<uint16_t>(elem) ? std::get<uint16_t>(elem) : 0;
                            WriteFixed(varData, &v, 2);
                            break;
                        }
                        case ColumnType::I32:
                        case ColumnType::EnumRow: {
                            int32_t v = std::holds_alternative<int32_t>(elem) ? std::get<int32_t>(elem) : 0;
                            WriteFixed(varData, &v, 4);
                            break;
                        }
                        case ColumnType::U32: {
                            uint32_t v = std::holds_alternative<uint32_t>(elem) ? std::get<uint32_t>(elem) : 0;
                            WriteFixed(varData, &v, 4);
                            break;
                        }
                        case ColumnType::F32: {
                            float v = std::holds_alternative<float>(elem) ? std::get<float>(elem) : 0.0f;
                            WriteFixed(varData, &v, 4);
                            break;
                        }
                        case ColumnType::Row: {
                            // Stride = 8. Null signaled via monostate only
                            // (DecodeCell array path always emits int64_t, so
                            // monostate here comes from a non-matching variant
                            // supplied by the caller).
                            uint64_t raw;
                            if (std::holds_alternative<int64_t>(elem)) {
                                raw = static_cast<uint64_t>(std::get<int64_t>(elem));
                            } else {
                                raw = NULL_MARKER_64;
                            }
                            WriteFixed(varData, &raw, 8);
                            break;
                        }
                        case ColumnType::ForeignRow: {
                            // Stride = 16 (8 primary + 8 pad). Without this branch
                            // the array round-trips as [first, garbage, garbage...]
                            // because each missing 8-byte pad shifts the next read.
                            uint64_t raw;
                            if (std::holds_alternative<int64_t>(elem)) {
                                raw = static_cast<uint64_t>(std::get<int64_t>(elem));
                            } else {
                                raw = NULL_MARKER_64;
                            }
                            WriteFixed(varData, &raw, 8);
                            uint64_t pad = 0;
                            WriteFixed(varData, &pad, 8);
                            break;
                        }
                        case ColumnType::String: {
                            // Store fixup position, write 8-byte placeholder.
                            strArrayFixups.push_back(varData.size());
                            uint64_t placeholder = 0;
                            WriteFixed(varData, &placeholder, 8);
                            break;
                        }
                        default: {
                            // Unknown (e.g. nested array-of-array). Write zeroes
                            // with the documented stride to preserve layout.
                            int sz = GetTypeSize(col.type);
                            for (int z = 0; z < sz; ++z) varData.push_back(0);
                            break;
                        }
                        }
                    }
                    // Patch string array fixups
                    size_t fixIdx = 0;
                    for (const auto& elem : arr.items) {
                        if (std::holds_alternative<std::string>(elem) && fixIdx < strArrayFixups.size()) {
                            const std::string& s = std::get<std::string>(elem);
                            uint64_t strOff;
                            if (s == "null") {
                                strOff = NULL_MARKER_64;
                            } else if (s.empty()) {
                                strOff = emitEmptyString();
                            } else {
                                strOff = varData.size() + kVarOffsetBias;
                            }
                            std::memcpy(varData.data() + strArrayFixups[fixIdx], &strOff, 8);
                            if (!s.empty() && s != "null") {
                                WriteUtf16String(varData, s);
                            }
                            fixIdx++;
                        }
                    }
                } else {
                    // Empty array
                    uint64_t zero = 0;
                    WriteFixed(fixedData, &zero, 8);
                    WriteFixed(fixedData, &zero, 8);
                }
                continue;
            }

            switch (col.type) {
            case ColumnType::U8: {
                const uint8_t v = EncodeU8Value(val);
                WriteFixed(fixedData, &v, 1);
                if (col.isInterval) { const uint8_t zero = 0; WriteFixed(fixedData, &zero, 1); }
                break;
            }
            case ColumnType::Bool: {
                uint8_t v = std::holds_alternative<bool>(val) ? (std::get<bool>(val) ? 1 : 0) : 0;
                WriteFixed(fixedData, &v, 1);
                break;
            }
            case ColumnType::I16: {
                int16_t v = std::holds_alternative<int16_t>(val) ? std::get<int16_t>(val) : 0;
                WriteFixed(fixedData, &v, 2);
                if (col.isInterval) { int16_t z = 0; WriteFixed(fixedData, &z, 2); }
                break;
            }
            case ColumnType::U16: {
                uint16_t v = std::holds_alternative<uint16_t>(val) ? std::get<uint16_t>(val) : 0;
                WriteFixed(fixedData, &v, 2);
                if (col.isInterval) { uint16_t z = 0; WriteFixed(fixedData, &z, 2); }
                break;
            }
            case ColumnType::I32: case ColumnType::EnumRow: {
                int32_t v = std::holds_alternative<int32_t>(val) ? std::get<int32_t>(val) : 0;
                WriteFixed(fixedData, &v, 4);
                if (col.isInterval) { int32_t z = 0; WriteFixed(fixedData, &z, 4); }
                break;
            }
            case ColumnType::U32: {
                uint32_t v = std::holds_alternative<uint32_t>(val) ? std::get<uint32_t>(val) : 0;
                WriteFixed(fixedData, &v, 4);
                if (col.isInterval) { uint32_t z = 0; WriteFixed(fixedData, &z, 4); }
                break;
            }
            case ColumnType::F32: {
                float v = std::holds_alternative<float>(val) ? std::get<float>(val) : 0.0f;
                WriteFixed(fixedData, &v, 4);
                if (col.isInterval) { float z = 0.0f; WriteFixed(fixedData, &z, 4); }
                break;
            }
            case ColumnType::String: {
                if (std::holds_alternative<std::string>(val)) {
                    const std::string& s = std::get<std::string>(val);
                    if (s == "null") {
                        // Explicit null sentinel from DecodeCell.
                        uint64_t n = NULL_MARKER_64;
                        WriteFixed(fixedData, &n, 8);
                    } else if (s.empty()) {
                        // Empty string — must point at a bare null terminator,
                        // NOT the NULL_MARKER (which would round-trip to "null").
                        uint64_t offset = emitEmptyString();
                        WriteFixed(fixedData, &offset, 8);
                    } else {
                        uint64_t offset = varData.size() + kVarOffsetBias;
                        WriteFixed(fixedData, &offset, 8);
                        WriteUtf16String(varData, s);
                    }
                } else {
                    // monostate / other → NULL_MARKER (matches Parse behaviour).
                    uint64_t n = NULL_MARKER_64;
                    WriteFixed(fixedData, &n, 8);
                }
                break;
            }
            case ColumnType::Row: {
                // Null is signaled by monostate (DecodeCell emits monostate iff
                // raw == NULL_MARKER_64). A negative int64 is NOT implicitly
                // null: real files contain primaries such as 0xFEFEFEFEFEFEFE00
                // which would sign-extend negative and must round-trip verbatim.
                if (std::holds_alternative<int64_t>(val)) {
                    uint64_t u = static_cast<uint64_t>(std::get<int64_t>(val));
                    WriteFixed(fixedData, &u, 8);
                } else {
                    uint64_t n = NULL_MARKER_64;
                    WriteFixed(fixedData, &n, 8);
                }
                break;
            }
            case ColumnType::ForeignRow: {
                if (std::holds_alternative<int64_t>(val)) {
                    uint64_t u = static_cast<uint64_t>(std::get<int64_t>(val));
                    WriteFixed(fixedData, &u, 8);
                    uint64_t pad = 0;
                    WriteFixed(fixedData, &pad, 8);
                } else {
                    uint64_t n = NULL_MARKER_64;
                    WriteFixed(fixedData, &n, 8);
                    WriteFixed(fixedData, &n, 8);
                }
                break;
            }
            default: {
                int sz = GetColumnByteSize(col);
                for (int i = 0; i < sz; i++) fixedData.push_back(0);
                break;
            }
            }
        }
    }

    // Assemble: [rowCount][fixedData][boundary][varData]
    std::vector<uint8_t> result;
    result.reserve(4 + fixedData.size() + 8 + varData.size());

    // Row count
    WriteFixed(result, &rowCount, 4);
    // Fixed data
    result.insert(result.end(), fixedData.begin(), fixedData.end());
    // Boundary marker
    uint64_t boundary = 0xBBBBBBBBBBBBBBBB;
    WriteFixed(result, &boundary, 8);
    // Variable data
    result.insert(result.end(), varData.begin(), varData.end());

    return result;
}

// -----------------------------------------------------------------------------
// ApplyEdits — patch-based save (see header doc).
// -----------------------------------------------------------------------------

namespace {

// Sum of GetColumnByteSize() for columns[0..endIdx). Used to compute the byte
// offset of a column within a row.
uint32_t PrefixColumnSize(const std::vector<ColumnDef>& cols, uint32_t endIdx) {
    uint32_t total = 0;
    const uint32_t n = (endIdx < cols.size()) ? endIdx : static_cast<uint32_t>(cols.size());
    for (uint32_t i = 0; i < n; ++i) {
        total += static_cast<uint32_t>(GetColumnByteSize(cols[i]));
    }
    return total;
}

// Encode a UTF-8 string as UTF-16LE with a 4-byte null terminator (two u16 zeros).
// Matches the byte layout of strings in existing .datc64 var sections.
std::vector<uint8_t> EncodeUtf16LEWithTerminator(const std::string& utf8) {
    std::vector<uint8_t> out;
    if (!utf8.empty()) {
        int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                          static_cast<int>(utf8.size()),
                                          nullptr, 0);
        if (wideLen > 0) {
            std::wstring ws(static_cast<size_t>(wideLen), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                static_cast<int>(utf8.size()),
                                ws.data(), wideLen);
            out.reserve(static_cast<size_t>(wideLen) * 2 + 4);
            for (wchar_t w : ws) {
                uint16_t u = static_cast<uint16_t>(w);
                out.push_back(static_cast<uint8_t>(u & 0xFF));
                out.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
            }
        }
    }
    // 4-byte null terminator (two u16 zeros).
    out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(0);
    return out;
}

// Scan the original var section of `out` for a byte-identical copy of `needle`.
// Returns the offset relative to boundaryPos on success, or SIZE_MAX on miss.
//
// Guards: needle must start either immediately after the 8-byte boundary-marker
// duplicate (offset 8) OR directly after a 4-byte null terminator (previous
// string's end). This avoids false positives inside longer strings.
size_t FindDedupOffset(const std::vector<uint8_t>& out,
                       size_t boundaryPos,
                       size_t origEnd,
                       const std::vector<uint8_t>& needle) {
    if (needle.size() < 4) return SIZE_MAX;  // 4-byte terminator alone is not a string
    const size_t varStart = boundaryPos + 8;
    if (varStart + needle.size() > origEnd) return SIZE_MAX;

    const size_t scanEnd = origEnd - needle.size();
    for (size_t p = varStart; p <= scanEnd; p += 2) {
        bool atStart = (p == varStart);
        bool afterTerm = false;
        if (!atStart && p >= varStart + 4) {
            afterTerm = out[p - 4] == 0 && out[p - 3] == 0 &&
                        out[p - 2] == 0 && out[p - 1] == 0;
        }
        if (!atStart && !afterTerm) continue;

        if (std::memcmp(out.data() + p, needle.data(), needle.size()) == 0) {
            return p - boundaryPos;
        }
    }
    return SIZE_MAX;
}

void PatchString(std::vector<uint8_t>& out, size_t boundaryPos, size_t origEnd,
                 const CellValue& val, size_t fixedPos) {
    const bool isNullLike =
        !std::holds_alternative<std::string>(val) ||
        std::get<std::string>(val).empty() ||
        std::get<std::string>(val) == "null";

    if (isNullLike) {
        uint64_t marker = NULL_MARKER_64;
        std::memcpy(out.data() + fixedPos, &marker, 8);
        return;
    }

    const std::string& s = std::get<std::string>(val);
    std::vector<uint8_t> bytes = EncodeUtf16LEWithTerminator(s);

    size_t dedup = FindDedupOffset(out, boundaryPos, origEnd, bytes);
    uint64_t offset;
    if (dedup != SIZE_MAX) {
        offset = static_cast<uint64_t>(dedup);
    } else {
        offset = static_cast<uint64_t>(out.size() - boundaryPos);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    std::memcpy(out.data() + fixedPos, &offset, 8);
}

void PatchArray(std::vector<uint8_t>& out, size_t boundaryPos, size_t origEnd,
                const ColumnDef& col, const CellValue& val, size_t fixedPos) {
    auto writeEmpty = [&]() {
        uint64_t zero = 0;
        std::memcpy(out.data() + fixedPos, &zero, 8);
        std::memcpy(out.data() + fixedPos + 8, &zero, 8);
    };

    if (!std::holds_alternative<CellArray>(val)) { writeEmpty(); return; }
    const auto& arr = std::get<CellArray>(val).items;
    if (arr.empty()) { writeEmpty(); return; }

    // Pre-pass for array<string>: ensure each string exists in var section (dedup
    // or append), collect their offsets. These go into the array block as u64.
    std::vector<uint64_t> stringOffsets;
    if (col.type == ColumnType::String) {
        stringOffsets.reserve(arr.size());
        for (const auto& elem : arr) {
            const bool nullLike =
                !std::holds_alternative<std::string>(elem) ||
                std::get<std::string>(elem).empty() ||
                std::get<std::string>(elem) == "null";
            if (nullLike) {
                stringOffsets.push_back(NULL_MARKER_64);
                continue;
            }
            auto bytes = EncodeUtf16LEWithTerminator(std::get<std::string>(elem));
            size_t dedup = FindDedupOffset(out, boundaryPos, origEnd, bytes);
            if (dedup != SIZE_MAX) {
                stringOffsets.push_back(static_cast<uint64_t>(dedup));
            } else {
                uint64_t o = static_cast<uint64_t>(out.size() - boundaryPos);
                out.insert(out.end(), bytes.begin(), bytes.end());
                stringOffsets.push_back(o);
            }
        }
    }

    // Build the array block.
    std::vector<uint8_t> elemBytes;
    for (size_t i = 0; i < arr.size(); ++i) {
        const auto& elem = arr[i];
        switch (col.type) {
        case ColumnType::U8:
            elemBytes.push_back(EncodeU8Value(elem));
            break;
        case ColumnType::Bool: {
            uint8_t v = std::holds_alternative<bool>(elem) ? (std::get<bool>(elem) ? 1 : 0) : 0;
            elemBytes.push_back(v);
            break;
        }
        case ColumnType::I16: {
            int16_t v = std::holds_alternative<int16_t>(elem) ? std::get<int16_t>(elem) : 0;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
            elemBytes.insert(elemBytes.end(), p, p + 2);
            break;
        }
        case ColumnType::U16: {
            uint16_t v = std::holds_alternative<uint16_t>(elem) ? std::get<uint16_t>(elem) : 0;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
            elemBytes.insert(elemBytes.end(), p, p + 2);
            break;
        }
        case ColumnType::I32:
        case ColumnType::EnumRow: {
            int32_t v = std::holds_alternative<int32_t>(elem) ? std::get<int32_t>(elem) : 0;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
            elemBytes.insert(elemBytes.end(), p, p + 4);
            break;
        }
        case ColumnType::U32: {
            uint32_t v = std::holds_alternative<uint32_t>(elem) ? std::get<uint32_t>(elem) : 0;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
            elemBytes.insert(elemBytes.end(), p, p + 4);
            break;
        }
        case ColumnType::F32: {
            float v = std::holds_alternative<float>(elem) ? std::get<float>(elem) : 0.0f;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
            elemBytes.insert(elemBytes.end(), p, p + 4);
            break;
        }
        case ColumnType::Row: {
            // Row element stride = 8 bytes.
            int64_t v = std::holds_alternative<int64_t>(elem) ? std::get<int64_t>(elem) : -1;
            uint64_t raw = (v < 0) ? NULL_MARKER_64 : static_cast<uint64_t>(v);
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&raw);
            elemBytes.insert(elemBytes.end(), p, p + 8);
            break;
        }
        case ColumnType::ForeignRow: {
            // ForeignRow element stride = 16 bytes (8 primary + 8 secondary pad).
            int64_t v = std::holds_alternative<int64_t>(elem) ? std::get<int64_t>(elem) : -1;
            uint64_t raw = (v < 0) ? NULL_MARKER_64 : static_cast<uint64_t>(v);
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&raw);
            elemBytes.insert(elemBytes.end(), p, p + 8);
            uint64_t pad = 0;
            const uint8_t* q = reinterpret_cast<const uint8_t*>(&pad);
            elemBytes.insert(elemBytes.end(), q, q + 8);
            break;
        }
        case ColumnType::String: {
            uint64_t off = stringOffsets[i];
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&off);
            elemBytes.insert(elemBytes.end(), p, p + 8);
            break;
        }
        default: {
            int sz = GetTypeSize(col.type);
            for (int z = 0; z < sz; ++z) elemBytes.push_back(0);
            break;
        }
        }
    }

    uint64_t count = static_cast<uint64_t>(arr.size());
    uint64_t arrOffset = static_cast<uint64_t>(out.size() - boundaryPos);
    out.insert(out.end(), elemBytes.begin(), elemBytes.end());
    std::memcpy(out.data() + fixedPos, &count, 8);
    std::memcpy(out.data() + fixedPos + 8, &arrOffset, 8);
}

// Rewrites the bytes at `out[pos .. pos+sizeof(type))` in place for a
// fixed-width column type. String and Array cells are handled by separate
// helpers in later tasks; they are no-ops here.
void PatchFixed(std::vector<uint8_t>& out, size_t pos,
                const ColumnDef& col, const CellValue& val) {
    switch (col.type) {
    case ColumnType::U8:
        out[pos] = EncodeU8Value(val);
        break;
    case ColumnType::Bool: {
        uint8_t v = std::holds_alternative<bool>(val) ? (std::get<bool>(val) ? 1 : 0) : 0;
        out[pos] = v;
        break;
    }
    case ColumnType::I16: {
        int16_t v = std::holds_alternative<int16_t>(val) ? std::get<int16_t>(val) : 0;
        std::memcpy(out.data() + pos, &v, 2);
        break;
    }
    case ColumnType::U16: {
        uint16_t v = std::holds_alternative<uint16_t>(val) ? std::get<uint16_t>(val) : 0;
        std::memcpy(out.data() + pos, &v, 2);
        break;
    }
    case ColumnType::I32:
    case ColumnType::EnumRow: {
        int32_t v = std::holds_alternative<int32_t>(val) ? std::get<int32_t>(val) : 0;
        std::memcpy(out.data() + pos, &v, 4);
        break;
    }
    case ColumnType::U32: {
        uint32_t v = std::holds_alternative<uint32_t>(val) ? std::get<uint32_t>(val) : 0;
        std::memcpy(out.data() + pos, &v, 4);
        break;
    }
    case ColumnType::F32: {
        float v = std::holds_alternative<float>(val) ? std::get<float>(val) : 0.0f;
        std::memcpy(out.data() + pos, &v, 4);
        break;
    }
    case ColumnType::Row: {
        int64_t v = std::holds_alternative<int64_t>(val) ? std::get<int64_t>(val) : -1;
        uint64_t raw = (v < 0) ? NULL_MARKER_64 : static_cast<uint64_t>(v);
        std::memcpy(out.data() + pos, &raw, 8);
        break;
    }
    case ColumnType::ForeignRow: {
        int64_t v = std::holds_alternative<int64_t>(val) ? std::get<int64_t>(val) : -1;
        uint64_t raw = (v < 0) ? NULL_MARKER_64 : static_cast<uint64_t>(v);
        std::memcpy(out.data() + pos, &raw, 8);
        // Secondary 8 bytes intentionally untouched.
        break;
    }
    default:
        // String/Array are handled by separate helpers; unknown types are no-op.
        break;
    }
}

} // anonymous namespace

// -----------------------------------------------------------------------------
// Interval second-half helpers — extract + patch.
// -----------------------------------------------------------------------------
//
// Rationale: DecodeCell returns only the first of two adjacent values for an
// interval column; the second half is currently dropped. Serialize therefore
// emits zeros for those bytes, which is data-destructive. Rather than widen
// CellValue (which would ripple through many sites), we expose:
//   1. ExtractIntervalSeconds  — pulls the seconds out of a parsed DatFile.
//   2. PatchIntervalSecondHalves — writes them back into the Serialize output.
// Callers (round-trip test, importer) are responsible for threading the map
// through. Non-interval columns, array columns, and unsupported types (String,
// Row, ForeignRow) are skipped — they shouldn't be marked isInterval anyway.
// -----------------------------------------------------------------------------

IntervalSecondMap ExtractIntervalSeconds(const DatFile& dat,
                                         const std::vector<ColumnDef>& columns) {
    IntervalSecondMap out;
    if (!dat.valid || dat.rowCount == 0) return out;

    // Pre-compute column byte offsets within a row.
    std::vector<uint32_t> colOffsets; colOffsets.reserve(columns.size());
    uint32_t running = 0;
    for (const auto& c : columns) {
        colOffsets.push_back(running);
        running += static_cast<uint32_t>(GetColumnByteSize(c));
    }

    for (uint32_t r = 0; r < dat.rowCount; ++r) {
        const size_t rowStart = dat.fixedOffset + static_cast<size_t>(r) * dat.rowLength;
        for (size_t c = 0; c < columns.size(); ++c) {
            const auto& col = columns[c];
            if (!col.isInterval || col.isArray) continue;
            const int baseSz = GetTypeSize(col.type);
            const size_t secondOff = rowStart + colOffsets[c] + static_cast<size_t>(baseSz);
            if (secondOff + static_cast<size_t>(baseSz) > dat.rawData.size()) continue;
            const uint8_t* p = dat.rawData.data() + secondOff;

            CellValue v;
            switch (col.type) {
            case ColumnType::U8:   { v = static_cast<uint32_t>(*p); break; }
            case ColumnType::Bool: { uint8_t x = *p; v = static_cast<bool>(x != 0); break; }
            case ColumnType::I16:  { int16_t x = 0;  std::memcpy(&x, p, 2); v = x; break; }
            case ColumnType::U16:  { uint16_t x = 0; std::memcpy(&x, p, 2); v = x; break; }
            case ColumnType::I32:
            case ColumnType::EnumRow: { int32_t x = 0; std::memcpy(&x, p, 4); v = x; break; }
            case ColumnType::U32:  { uint32_t x = 0; std::memcpy(&x, p, 4); v = x; break; }
            case ColumnType::F32:  { float x = 0.0f; std::memcpy(&x, p, 4); v = x; break; }
            default: continue; // String/Row/ForeignRow: no defined interval layout.
            }
            out.emplace(MakeIntervalKey(r, static_cast<uint32_t>(c)), std::move(v));
        }
    }
    return out;
}

void PatchIntervalSecondHalves(std::vector<uint8_t>& bytes,
                               uint32_t rowCount,
                               const std::vector<ColumnDef>& columns,
                               const IntervalSecondMap& seconds) {
    if (seconds.empty() || bytes.size() < 4 || rowCount == 0) return;

    // Re-derive row length from the schema (matches what Serialize produced).
    uint32_t rowLength = 0;
    for (const auto& c : columns)
        rowLength += static_cast<uint32_t>(GetColumnByteSize(c));
    if (rowLength == 0) return;

    // Pre-compute column byte offsets.
    std::vector<uint32_t> colOffsets; colOffsets.reserve(columns.size());
    uint32_t running = 0;
    for (const auto& c : columns) {
        colOffsets.push_back(running);
        running += static_cast<uint32_t>(GetColumnByteSize(c));
    }

    const size_t fixedOffset = 4; // Matches Serialize output layout.
    const size_t fixedEnd = fixedOffset + static_cast<size_t>(rowCount) * rowLength;
    if (bytes.size() < fixedEnd) return;

    for (const auto& [key, val] : seconds) {
        const uint32_t r = static_cast<uint32_t>(key >> 32);
        const uint32_t c = static_cast<uint32_t>(key & 0xFFFFFFFFu);
        if (r >= rowCount || c >= columns.size()) continue;
        const auto& col = columns[c];
        if (!col.isInterval || col.isArray) continue;

        const int baseSz = GetTypeSize(col.type);
        const size_t secondOff = fixedOffset + static_cast<size_t>(r) * rowLength
                                + colOffsets[c] + static_cast<size_t>(baseSz);
        if (secondOff + static_cast<size_t>(baseSz) > fixedEnd) continue;
        uint8_t* p = bytes.data() + secondOff;

        // Strict variant-match, fall back to zero-fill defaults on mismatch.
        switch (col.type) {
        case ColumnType::U8:
            *p = EncodeU8Value(val);
            break;
        case ColumnType::Bool: {
            uint8_t x = std::holds_alternative<bool>(val) ? (std::get<bool>(val) ? 1 : 0) : 0;
            *p = x; break;
        }
        case ColumnType::I16: {
            int16_t x = std::holds_alternative<int16_t>(val) ? std::get<int16_t>(val) : 0;
            std::memcpy(p, &x, 2); break;
        }
        case ColumnType::U16: {
            uint16_t x = std::holds_alternative<uint16_t>(val) ? std::get<uint16_t>(val) : 0;
            std::memcpy(p, &x, 2); break;
        }
        case ColumnType::I32:
        case ColumnType::EnumRow: {
            int32_t x = std::holds_alternative<int32_t>(val) ? std::get<int32_t>(val) : 0;
            std::memcpy(p, &x, 4); break;
        }
        case ColumnType::U32: {
            uint32_t x = std::holds_alternative<uint32_t>(val) ? std::get<uint32_t>(val) : 0;
            std::memcpy(p, &x, 4); break;
        }
        case ColumnType::F32: {
            float x = std::holds_alternative<float>(val) ? std::get<float>(val) : 0.0f;
            std::memcpy(p, &x, 4); break;
        }
        default: break;
        }
    }
}

std::vector<uint8_t> ApplyEdits(const DatFile& dat,
                                 const std::vector<ColumnDef>& columns,
                                 const std::vector<DatEdit>& edits) {
    if (!dat.valid) return {};

    // Schema compatibility check: refuse if column layout doesn't match file.
    uint32_t schemaRowLength = 0;
    for (const auto& c : columns) schemaRowLength += static_cast<uint32_t>(GetColumnByteSize(c));
    if (schemaRowLength != dat.rowLength) return {};

    // Fast path: no edits → return the original bytes unchanged.
    if (edits.empty()) {
        return dat.rawData;
    }

    std::vector<uint8_t> out = dat.rawData;

    for (const auto& edit : edits) {
        if (edit.rowIndex >= dat.rowCount) continue;
        if (edit.columnIndex >= columns.size()) continue;
        const auto& col = columns[edit.columnIndex];

        size_t rowStart = 4 + static_cast<size_t>(edit.rowIndex) * dat.rowLength;
        size_t colOffset = PrefixColumnSize(columns, edit.columnIndex);
        size_t patchPos = rowStart + colOffset;

        if (col.isArray) {
            size_t origEnd = dat.rawData.size();
            PatchArray(out, dat.varOffset, origEnd, col, edit.newValue, patchPos);
            continue;
        }
        if (col.type == ColumnType::String) {
            size_t origEnd = dat.rawData.size();
            PatchString(out, dat.varOffset, origEnd, edit.newValue, patchPos);
            continue;
        }

        size_t interval2Shift = (edit.editInterval2 && col.isInterval)
            ? static_cast<size_t>(GetTypeSize(col.type)) : 0;
        PatchFixed(out, patchPos + interval2Shift, col, edit.newValue);
    }

    return out;
}

} // namespace GGPKExp
