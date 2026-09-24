#pragma once
#include <cstdint>
#include <string>
#include <algorithm>
#include <vector>

namespace GGPKExp {

enum class FileType : uint8_t {
    Directory, Text, Image, Dat, Audio, Video, Shader,
    Mesh, Terrain, TerrainMeta, MatTable, SkillTree,
    Aoc,
    Binary, Unknown
};

inline FileType DetectFileType(const std::string& name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos) return FileType::Unknown;
    std::string ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Images
    if (ext == ".dds" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
        ext == ".bmp" || ext == ".tga" || ext == ".hdr")
        return FileType::Image;

    // DAT
    if (ext == ".dat" || ext == ".datc64" || ext == ".datl64")
        return FileType::Dat;

    // Audio
    if (ext == ".ogg" || ext == ".wav" || ext == ".mp3" || ext == ".bank")
        return FileType::Audio;

    // Video
    if (ext == ".bk2" || ext == ".mp4" || ext == ".usm")
        return FileType::Video;

    // Shaders
    if (ext == ".fx" || ext == ".hlsl" || ext == ".glsl" || ext == ".cso" || ext == ".sho" ||
        ext == ".ffx")
        return FileType::Shader;

    // 3D Mesh (skinned mesh descriptor + data + skeleton)
    if (ext == ".sm") return FileType::Mesh;
    if (ext == ".smd") return FileType::Mesh;
    if (ext == ".ast") return FileType::Mesh;

    // Terrain metadata
    if (ext == ".mtp") return FileType::TerrainMeta;

    // Terrain grid data
    if (ext == ".tdt") return FileType::Terrain;

    if (ext == ".psg") return FileType::SkillTree;

    // AOC (animation/object controller) — custom viewer with typed sections
    if (ext == ".aoc") return FileType::Aoc;

    // Text (long list from spec)
    if (ext == ".txt" || ext == ".json" || ext == ".xml" || ext == ".html" ||
        ext == ".filter" || ext == ".lua" || ext == ".csv" || ext == ".atlas" ||
        ext == ".ot" || ext == ".pet" || ext == ".sm" || ext == ".trl" ||
        ext == ".act" || ext == ".ai" || ext == ".amd" ||
        ext == ".arm" || ext == ".cfg" || ext == ".env" || ext == ".epk" ||
        ext == ".it" || ext == ".mat" || ext == ".mtd" || ext == ".properties" ||
        ext == ".red" || ext == ".rs" || ext == ".tgr" ||
        ext == ".tgt" || ext == ".ui" || ext == ".clt" || ext == ".tsi" ||
        ext == ".tsx" || ext == ".idl" || ext == ".csd" || ext == ".ao" || ext == ".otc" ||
        ext == ".fxgraph" ||
        ext == ".itc" || ext == ".dgr" || ext == ".atl" || ext == ".tmd" ||
        ext == ".cht" || ext == ".ecf" || ext == ".dct" || ext == ".dlp" ||
        ext == ".ddt" || ext == ".fgp" || ext == ".gft" || ext == ".toy" ||
        ext == ".tst" || ext == ".gt" || ext == ".tmo" || ext == ".et" ||
        ext == ".hideout")
        return FileType::Text;

    return FileType::Binary;
}

inline const char* FileTypeToString(FileType type) {
    switch (type) {
    case FileType::Directory: return "Directory";
    case FileType::Text:      return "Text";
    case FileType::Image:     return "Image";
    case FileType::Dat:       return "DAT Table";
    case FileType::Audio:     return "Audio";
    case FileType::Video:     return "Video";
    case FileType::Shader:    return "Shader";
    case FileType::Mesh:        return "3D Mesh";
    case FileType::Terrain:     return "Terrain Data";
    case FileType::TerrainMeta: return "Terrain Metadata";
    case FileType::MatTable:    return "Material Table";
    case FileType::SkillTree:   return "Skill Tree";
    case FileType::Aoc:         return "AOC";
    case FileType::Binary:    return "Binary";
    default:                  return "Unknown";
    }
}

/// Heuristic: scan first 8KB for null bytes. If <=5% nulls, likely text.
inline bool IsLikelyText(const std::vector<uint8_t>& data) {
    if (data.empty()) return false;
    size_t scanLen = std::min<size_t>(data.size(), 8192);
    size_t nullCount = 0;
    for (size_t i = 0; i < scanLen; ++i) {
        if (data[i] == 0) ++nullCount;
    }
    return (nullCount * 100 / scanLen) <= 5;
}

} // namespace GGPKExp
