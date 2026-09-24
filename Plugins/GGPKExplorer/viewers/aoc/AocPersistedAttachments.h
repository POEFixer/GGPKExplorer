#pragma once

#include <string>
#include <vector>

namespace GGPKExp {

class GGPKManager;

namespace Parsers {
struct AocDocument;
}

namespace Aoc {

class AocSceneGraph;
class AocTransitionState;

// Phase 5.1 — Persistent attachments + Transitionable state via JSON sidecar.
// JSON location: {pluginDir}/user_data/attachments/<aoc_virtual_path>.json
// Auto-loaded on AocViewer::DoParsing; auto-saved on every user-visible change.
class AocPersistedAttachments {
public:
    enum class EntryStatus { Applied, MissingSource, MissingSocket, ParseFailed };

    struct Entry {
        std::string socket;
        std::string sourcePath;
        EntryStatus status = EntryStatus::Applied;
        std::string failureReason;  // empty iff status == Applied
    };

    struct LoadResult {
        int  restoredAttachments     = 0;
        int  skippedAttachments      = 0;
        bool transitionStateRestored = false;
        // T7 (Bug B5) — true iff JSON had a non-empty overrideOtPath that
        // could not be parsed. The override string is preserved on disk and
        // in memory; caller surfaces a toast so the user isn't left in the
        // dark. overrideOtWarnings captures user-visible context (one entry
        // per failure). Distinct from class-level m_warnings (which is the
        // general persistence-parser warning channel).
        bool                     overrideOtFailed = false;
        std::vector<std::string> overrideOtWarnings;
    };

    bool Populate(const std::string&          pluginDir,
                  const std::string&          aocPath,
                  GGPKManager*                mgr,
                  AocSceneGraph&              sceneGraph,
                  const Parsers::AocDocument& parsedDoc,
                  AocTransitionState&         transitions,
                  LoadResult&                 outResult);

    void Save(const std::string&        pluginDir,
              const std::string&        aocPath,
              const AocSceneGraph&      sceneGraph,
              const AocTransitionState& transitions);

    void ClearPersistence(const std::string& pluginDir,
                          const std::string& aocPath);

    void Clear();

    bool IsLoaded()           const { return m_loaded; }
    bool JsonExistsOnDisk()   const { return m_jsonExists; }
    int  AttachmentCount()    const { return static_cast<int>(m_entries.size()); }
    bool HasTransitionState() const { return m_hasState; }

    const std::string&              JsonPath() const { return m_jsonPath; }
    const std::vector<Entry>&       Entries()  const { return m_entries; }
    const std::vector<std::string>& Warnings() const { return m_warnings; }

private:
    bool                     m_loaded     = false;
    bool                     m_jsonExists = false;
    bool                     m_hasState   = false;
    std::string              m_jsonPath;
    std::vector<Entry>       m_entries;
    std::vector<std::string> m_warnings;

    static std::string DeriveJsonPath(const std::string& pluginDir, const std::string& aocPath);
    static bool        IsStateDefault(const AocTransitionState& t);
};

} // namespace Aoc
} // namespace GGPKExp
