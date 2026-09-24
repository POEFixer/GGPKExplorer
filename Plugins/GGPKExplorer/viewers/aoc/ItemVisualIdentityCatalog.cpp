#include "ItemVisualIdentityCatalog.h"

#include "../../core/DatParser.h"
#include "../../core/DatSchema.h"
#include "../../core/GGPKManager.h"

#include <algorithm>
#include <cctype>

namespace GGPKExp {
namespace Aoc {

namespace {

constexpr const char* kBaseItemTypesPath    = "data/balance/baseitemtypes.datc64";
constexpr const char* kItemVisualIdentityPath = "data/balance/itemvisualidentity.datc64";
constexpr const char* kItemClassesPath      = "data/balance/itemclasses.datc64";

constexpr const char* kBaseItemTypesTable    = "BaseItemTypes";
constexpr const char* kItemVisualIdentityTbl = "ItemVisualIdentity";
constexpr const char* kItemClassesTable      = "ItemClasses";

constexpr const char* kColIdBase             = "Id";
constexpr const char* kColNameBase           = "Name";
constexpr const char* kColItemClassesKey     = "ItemClassesKey";
constexpr const char* kColItemVisualIdentity = "ItemVisualIdentity";

constexpr const char* kColNameClasses        = "Name";

constexpr const char* kColAOFile             = "AOFile";
constexpr const char* kColAOFile2            = "AOFile2";
constexpr const char* kColSMFiles            = "SMFiles";
constexpr const char* kColMarauderSMFiles    = "MarauderSMFiles";
constexpr const char* kColRangerSMFiles      = "RangerSMFiles";
constexpr const char* kColWitchSMFiles       = "WitchSMFiles";
constexpr const char* kColDuelistDexSMFiles  = "DuelistDexSMFiles";
constexpr const char* kColTemplarSMFiles     = "TemplarSMFiles";
constexpr const char* kColShadowSMFiles      = "ShadowSMFiles";
constexpr const char* kColScionSMFiles       = "ScionSMFiles";
constexpr const char* kColDDSFile            = "DDSFile";

bool IsAbsentString(const std::string& s) { return s.empty() || s == "null"; }

int FindColumnOffset(const SchemaTable& tab,
                     const char* name,
                     int* outColumnIndex) {
    int offset = 0;
    for (const auto& col : tab.columns) {
        if (col.name == name) {
            if (outColumnIndex) *outColumnIndex = col.index;
            return offset;
        }
        offset += GetColumnByteSize(col);
    }
    return -1;
}

std::string DecodeScalarString(const DatFile& dat,
                               std::uint32_t rowIdx,
                               int columnOffset,
                               const ColumnDef& col) {
    if (columnOffset < 0) return {};
    CellValue v = DecodeCell(dat, rowIdx, static_cast<std::uint32_t>(columnOffset), col);
    if (std::holds_alternative<std::string>(v)) {
        std::string s = std::get<std::string>(v);
        if (IsAbsentString(s)) return {};
        return s;
    }
    return {};
}

std::int64_t DecodeScalarForeignRow(const DatFile& dat,
                                    std::uint32_t rowIdx,
                                    int columnOffset,
                                    const ColumnDef& col) {
    if (columnOffset < 0) return -1;
    CellValue v = DecodeCell(dat, rowIdx, static_cast<std::uint32_t>(columnOffset), col);
    if (std::holds_alternative<std::int64_t>(v)) return std::get<std::int64_t>(v);
    return -1;
}

std::vector<std::string> DecodeStringArray(const DatFile& dat,
                                           std::uint32_t rowIdx,
                                           int columnOffset,
                                           const ColumnDef& col) {
    std::vector<std::string> out;
    if (columnOffset < 0) return out;
    CellValue v = DecodeCell(dat, rowIdx, static_cast<std::uint32_t>(columnOffset), col);
    if (!std::holds_alternative<CellArray>(v)) return out;
    const auto& arr = std::get<CellArray>(v).items;
    out.reserve(arr.size());
    for (const auto& item : arr) {
        if (!std::holds_alternative<std::string>(item)) continue;
        const std::string& s = std::get<std::string>(item);
        if (IsAbsentString(s)) continue;
        out.push_back(s);
    }
    return out;
}

// Lowercase ASCII, backslash->forward-slash, ensure ".dds" suffix.
// Used only for DDS icon paths (Phase 5.1.5c). Keeps FileKind-based
// NormalizePath limited to attach-target paths.
std::string NormalizeDDSPath(std::string s) {
    for (char& c : s) {
        if (c == '\\') c = '/';
        else           c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    static const std::string kDotDds = ".dds";
    if (s.size() < kDotDds.size() ||
        s.compare(s.size() - kDotDds.size(), kDotDds.size(), kDotDds) != 0) {
        s += kDotDds;
    }
    return s;
}

} // anonymous namespace

void ItemVisualIdentityCatalog::Reset() {
    m_items.clear();
    m_loadError.clear();
    m_loaded = false;
}

bool ItemVisualIdentityCatalog::Load(GGPKManager* mgr, DatSchema* schema, LogCb logCb) {
    Reset();

    if (!mgr || !schema) {
        m_loadError = "Load precondition failed: mgr/schema null";
        return false;
    }

    const SchemaTable* baseTab    = schema->FindTable(kBaseItemTypesTable);
    const SchemaTable* iviTab     = schema->FindTable(kItemVisualIdentityTbl);
    const SchemaTable* classesTab = schema->FindTable(kItemClassesTable);
    if (!baseTab) { m_loadError = "Schema missing table: BaseItemTypes"; return false; }
    if (!iviTab)  { m_loadError = "Schema missing table: ItemVisualIdentity"; return false; }
    if (!classesTab) { m_loadError = "Schema missing table: ItemClasses"; return false; }

    DatFile baseDat, iviDat, classesDat;
    std::string err;
    if (!LoadDat(mgr, kBaseItemTypesPath, baseDat, err)) {
        m_loadError = "BaseItemTypes: " + err;
        return false;
    }
    if (!LoadDat(mgr, kItemVisualIdentityPath, iviDat, err)) {
        m_loadError = "ItemVisualIdentity: " + err;
        return false;
    }
    if (!LoadDat(mgr, kItemClassesPath, classesDat, err)) {
        m_loadError = "ItemClasses: " + err;
        return false;
    }

    auto classNameByRow = BuildClassNameMap(classesDat, *classesTab, logCb);

    const int ofsBaseId       = FindColumnOffset(*baseTab, kColIdBase, nullptr);
    const int ofsBaseName     = FindColumnOffset(*baseTab, kColNameBase, nullptr);
    const int ofsBaseClass    = FindColumnOffset(*baseTab, kColItemClassesKey, nullptr);
    const int ofsBaseIvi      = FindColumnOffset(*baseTab, kColItemVisualIdentity, nullptr);
    if (ofsBaseName < 0 || ofsBaseIvi < 0) {
        m_loadError = "BaseItemTypes schema missing required columns";
        return false;
    }

    auto FindCol = [](const SchemaTable& t, const char* name) -> const ColumnDef* {
        for (const auto& c : t.columns) if (c.name == name) return &c;
        return nullptr;
    };
    const ColumnDef* colBaseId    = FindCol(*baseTab, kColIdBase);
    const ColumnDef* colBaseName  = FindCol(*baseTab, kColNameBase);
    const ColumnDef* colBaseClass = FindCol(*baseTab, kColItemClassesKey);
    const ColumnDef* colBaseIvi   = FindCol(*baseTab, kColItemVisualIdentity);
    if (!colBaseName || !colBaseIvi) {
        m_loadError = "BaseItemTypes schema column lookup failed";
        return false;
    }

    m_items.reserve(baseDat.rowCount);
    for (std::uint32_t row = 0; row < baseDat.rowCount; ++row) {
        const std::int64_t iviRef = DecodeScalarForeignRow(baseDat, row, ofsBaseIvi, *colBaseIvi);
        if (iviRef < 0 || static_cast<std::uint32_t>(iviRef) >= iviDat.rowCount) continue;

        std::vector<FileCandidate> files;
        CollectFileCandidates(static_cast<std::uint32_t>(iviRef), iviDat, *iviTab, files, logCb);
        if (files.empty()) continue;

        Item item;
        item.baseItemRowIdx = row;
        item.iviRowIdx      = static_cast<std::uint32_t>(iviRef);
        if (colBaseName) item.name = DecodeScalarString(baseDat, row, ofsBaseName, *colBaseName);
        if (colBaseId)   item.id   = DecodeScalarString(baseDat, row, ofsBaseId,   *colBaseId);

        if (colBaseClass && ofsBaseClass >= 0) {
            const std::int64_t classRef =
                DecodeScalarForeignRow(baseDat, row, ofsBaseClass, *colBaseClass);
            if (classRef >= 0) {
                auto it = classNameByRow.find(static_cast<std::uint32_t>(classRef));
                if (it != classNameByRow.end()) item.className = it->second;
            }
        }

        if (item.name.empty()) item.name = item.id;

        item.searchBlob = ToLowerAscii(item.name + " " + item.className);

        // ---- Phase 5.1.5c - DDSFile collection for thumbnails ----
        const int ofsIviDDS = FindColumnOffset(*iviTab, kColDDSFile, nullptr);
        const ColumnDef* colIviDDS = FindCol(*iviTab, kColDDSFile);
        if (ofsIviDDS >= 0 && colIviDDS) {
            std::string raw = DecodeScalarString(
                iviDat, static_cast<std::uint32_t>(iviRef), ofsIviDDS, *colIviDDS);
            if (!raw.empty()) {
                item.ddsPath = NormalizeDDSPath(std::move(raw));
            }
        }
        // ---- end Phase 5.1.5c ----

        item.files = std::move(files);
        m_items.push_back(std::move(item));
    }

    m_loaded = true;
    return true;
}

bool ItemVisualIdentityCatalog::LoadDat(GGPKManager* mgr,
                                        const std::string& virtualPath,
                                        DatFile& outDat,
                                        std::string& outError) {
    void* node = mgr->FindNodeByPath(virtualPath);
    if (!node) { outError = "file not in GGPK: " + virtualPath; return false; }
    auto res = mgr->ReadFile(node);
    if (!res.success) { outError = "read failed: " + res.error; return false; }
    outDat = Parse(res.value);
    if (!outDat.valid) { outError = "parse failed: " + outDat.error; return false; }
    return true;
}

std::unordered_map<std::uint32_t, std::string>
ItemVisualIdentityCatalog::BuildClassNameMap(const DatFile& classesDat,
                                             const SchemaTable& classesTab,
                                             LogCb /*logCb*/) {
    std::unordered_map<std::uint32_t, std::string> out;
    const int ofsClassName = FindColumnOffset(classesTab, kColNameClasses, nullptr);
    if (ofsClassName < 0) return out;

    const ColumnDef* colName = nullptr;
    for (const auto& c : classesTab.columns) if (c.name == kColNameClasses) { colName = &c; break; }
    if (!colName) return out;

    out.reserve(classesDat.rowCount);
    for (std::uint32_t row = 0; row < classesDat.rowCount; ++row) {
        std::string n = DecodeScalarString(classesDat, row, ofsClassName, *colName);
        if (n.empty()) continue;
        out.emplace(row, std::move(n));
    }
    return out;
}

std::string ItemVisualIdentityCatalog::ToLowerAscii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

void ItemVisualIdentityCatalog::CollectFileCandidates(std::uint32_t iviRow,
                                                       const DatFile& iviDat,
                                                       const SchemaTable& iviTab,
                                                       std::vector<FileCandidate>& out,
                                                       LogCb /*logCb*/) {
    struct ColSpec {
        const char* name;
        FileKind    kind;
        bool        isArray;
    };
    static const ColSpec kSpecs[] = {
        { kColAOFile,            FileKind::AOFile,            false },
        { kColAOFile2,           FileKind::AOFile2,           false },
        { kColSMFiles,           FileKind::SMFiles,           true  },
        { kColMarauderSMFiles,   FileKind::MarauderSMFiles,   true  },
        { kColRangerSMFiles,     FileKind::RangerSMFiles,     true  },
        { kColWitchSMFiles,      FileKind::WitchSMFiles,      true  },
        { kColDuelistDexSMFiles, FileKind::DuelistDexSMFiles, true  },
        { kColTemplarSMFiles,    FileKind::TemplarSMFiles,    true  },
        { kColShadowSMFiles,     FileKind::ShadowSMFiles,     true  },
        { kColScionSMFiles,      FileKind::ScionSMFiles,      true  },
    };

    auto basename = [](const std::string& p) -> std::string {
        const std::size_t sl = p.find_last_of('/');
        return (sl == std::string::npos) ? p : p.substr(sl + 1);
    };

    auto kindLabel = [](FileKind k) -> const char* {
        switch (k) {
        case FileKind::AOFile:            return "AOFile";
        case FileKind::AOFile2:           return "AOFile2";
        case FileKind::SMFiles:           return "SMFiles";
        case FileKind::MarauderSMFiles:   return "MarauderSMFiles";
        case FileKind::RangerSMFiles:     return "RangerSMFiles";
        case FileKind::WitchSMFiles:      return "WitchSMFiles";
        case FileKind::DuelistDexSMFiles: return "DuelistDexSMFiles";
        case FileKind::TemplarSMFiles:    return "TemplarSMFiles";
        case FileKind::ShadowSMFiles:     return "ShadowSMFiles";
        case FileKind::ScionSMFiles:      return "ScionSMFiles";
        }
        return "?";
    };

    auto emit = [&](FileKind kind, int subIndex, std::string rawPath) {
        std::string path = NormalizePath(std::move(rawPath), kind);
        if (path.empty()) return;
        FileCandidate c;
        c.kind     = kind;
        c.subIndex = subIndex;
        c.path     = ToLowerAscii(path);
        if (kind == FileKind::AOFile || kind == FileKind::AOFile2) {
            c.label = std::string(kindLabel(kind)) + ": " + basename(c.path);
        } else {
            c.label = std::string(kindLabel(kind)) + "[" + std::to_string(subIndex) + "]: " + basename(c.path);
        }
        out.push_back(std::move(c));
    };

    for (const auto& spec : kSpecs) {
        const int ofs = FindColumnOffset(iviTab, spec.name, nullptr);
        if (ofs < 0) continue;

        const ColumnDef* col = nullptr;
        for (const auto& c : iviTab.columns) if (c.name == spec.name) { col = &c; break; }
        if (!col) continue;

        if (spec.isArray) {
            auto values = DecodeStringArray(iviDat, iviRow, ofs, *col);
            for (std::size_t i = 0; i < values.size(); ++i) {
                emit(spec.kind, static_cast<int>(i), std::move(values[i]));
            }
        } else {
            std::string s = DecodeScalarString(iviDat, iviRow, ofs, *col);
            if (!s.empty()) emit(spec.kind, 0, std::move(s));
        }
    }
}

std::string ItemVisualIdentityCatalog::NormalizePath(std::string s, FileKind kind) {
    if (s.empty() || s == "null") return {};

    auto endsWithCi = [](const std::string& str, const char* suffix) -> bool {
        const std::size_t n = std::char_traits<char>::length(suffix);
        if (str.size() < n) return false;
        for (std::size_t i = 0; i < n; ++i) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(str[str.size() - n + i])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
            if (a != b) return false;
        }
        return true;
    };

    const bool isAoKind = (kind == FileKind::AOFile || kind == FileKind::AOFile2);
    const char* wanted  = isAoKind ? ".aoc" : ".sm";

    if (endsWithCi(s, wanted)) return s;

    // PoE2 asset convention: IVI stores the source-file suffix (".ao" for actor
    // objects, ".smd" for skinned meshes), but the compiled/loadable form at
    // the same basename uses ".aoc" / ".sm". Map the source suffix to the
    // compiled one rather than naively appending, which would yield
    // "<path>.ao.aoc" and miss the real asset.
    auto stripAndAppend = [&](const char* srcSuffix) {
        const std::size_t n = std::char_traits<char>::length(srcSuffix);
        s.resize(s.size() - n);
        s += wanted;
    };

    if (isAoKind && endsWithCi(s, ".ao"))       { stripAndAppend(".ao");  return s; }
    if (!isAoKind && endsWithCi(s, ".smd"))     { stripAndAppend(".smd"); return s; }

    s += wanted;
    return s;
}

int ItemVisualIdentityCatalog::PickDefaultFileIndex(const Item& item,
                                                    const std::string& parentAocPath) {
    if (item.files.empty()) return -1;

    // Heuristic maps PoE2 character-archetype folder names (metadata/characters/{token}/)
    // to the corresponding {Class}SMFiles column in ItemVisualIdentity. The column names
    // themselves are PoE1 heritage (Marauder/Ranger/Witch/DuelistDex/Templar/Shadow/Scion)
    // but PoE2 stores character meshes under archetype folders: str / dex / int / strdex /
    // strint / dexint / strdexint. Order matters: longer/more-specific tokens first so
    // "dexint" wins over "dex".
    struct Token { const char* tok; FileKind kind; };
    static const Token kTokens[] = {
        { "/strdexint/", FileKind::ScionSMFiles       },
        { "/dexint/",    FileKind::ShadowSMFiles      },
        { "/strdex/",    FileKind::DuelistDexSMFiles  },
        { "/strint/",    FileKind::TemplarSMFiles     },
        { "/str/",       FileKind::MarauderSMFiles    },
        { "/dex/",       FileKind::RangerSMFiles      },
        { "/int/",       FileKind::WitchSMFiles       },
    };

    const std::string lowerPath = ToLowerAscii(parentAocPath);
    for (const auto& t : kTokens) {
        if (lowerPath.find(t.tok) == std::string::npos) continue;
        for (int i = 0; i < static_cast<int>(item.files.size()); ++i) {
            if (item.files[i].kind == t.kind) return i;
        }
    }

    for (int i = 0; i < static_cast<int>(item.files.size()); ++i) {
        if (item.files[i].kind == FileKind::AOFile) return i;
    }
    for (int i = 0; i < static_cast<int>(item.files.size()); ++i) {
        if (item.files[i].kind == FileKind::SMFiles) return i;
    }
    return 0;
}

} // namespace Aoc
} // namespace GGPKExp
