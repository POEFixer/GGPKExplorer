#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace GGPKExp {

class GGPKManager;
class DatSchema;
struct DatFile;
struct SchemaTable;

namespace Aoc {

// Phase 5.1.5b — DAT-backed catalog of items and their visual file paths.
// Loads three DAT files once per GGPK session:
//   - BaseItemTypes.datc64       → item identity (Name, Id, class, IVI ref)
//   - ItemVisualIdentity.datc64  → visual fields (AOFile, SMFiles[], per-class SMFiles[])
//   - ItemClasses.datc64         → human-readable class name
// Skips items whose ItemVisualIdentity foreignrow is null OR whose IVI row has
// zero non-empty file candidates (nothing attachable).
class ItemVisualIdentityCatalog {
public:
    enum class FileKind {
        AOFile,
        AOFile2,
        SMFiles,
        MarauderSMFiles,
        RangerSMFiles,
        WitchSMFiles,
        DuelistDexSMFiles,
        TemplarSMFiles,
        ShadowSMFiles,
        ScionSMFiles,
    };

    struct FileCandidate {
        FileKind    kind = FileKind::AOFile;
        int         subIndex = 0;      // 0 for AOFile/AOFile2; array index otherwise.
        std::string path;              // Lowercase GGPK virtual path with correct suffix.
        std::string label;             // Short UI label ("MarauderSMFiles[0]: {basename}").
    };

    struct Item {
        std::uint32_t              baseItemRowIdx = 0;
        std::uint32_t              iviRowIdx      = 0;
        std::string                name;          // BaseItemTypes.Name (localized)
        std::string                id;            // BaseItemTypes.Id (internal)
        std::string                className;     // ItemClasses.Name via join
        std::string                searchBlob;    // lowercase(name + " " + className) for filter
        std::string                ddsPath;       // Normalized GGPK path of IVI.DDSFile; empty if absent.
        std::vector<FileCandidate> files;         // non-empty by construction (empty items are dropped)
    };

    using LogCb = std::function<void(const std::string&)>;

    bool Load(GGPKManager* mgr, DatSchema* schema, LogCb logCb = nullptr);

    const std::vector<Item>& Items() const        { return m_items; }
    const std::string&       GetLoadError() const { return m_loadError; }
    bool                     IsLoaded() const     { return m_loaded; }

    void Reset();

    static int PickDefaultFileIndex(const Item& item, const std::string& parentAocPath);

private:
    static bool LoadDat(GGPKManager* mgr,
                        const std::string& virtualPath,
                        DatFile& outDat,
                        std::string& outError);

    static std::unordered_map<std::uint32_t, std::string>
        BuildClassNameMap(const DatFile& classesDat,
                          const SchemaTable& classesTab,
                          LogCb logCb);

    static void CollectFileCandidates(std::uint32_t iviRow,
                                      const DatFile& iviDat,
                                      const SchemaTable& iviTab,
                                      std::vector<FileCandidate>& out,
                                      LogCb logCb);

    static std::string NormalizePath(std::string s, FileKind kind);
    static std::string ToLowerAscii(const std::string& s);

    std::vector<Item> m_items;
    std::string       m_loadError;
    bool              m_loaded = false;
};

} // namespace Aoc
} // namespace GGPKExp
