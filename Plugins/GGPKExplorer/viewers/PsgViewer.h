#pragma once
#include "IViewer.h"
#include "../core/parsers/PsgParser.h"
#include "../core/AsyncTaskRunner.h"
#include "../core/Result.h"
#include "../core/DatSchema.h"
#include "../core/GGPKExplorerSettings.h"
#include "../core/parsers/CsdParser.h"
#include "PsgIconCache.h"
#include <d3d11.h>
#include <imgui.h>
#include <array>
#include <deque>
#include <future>
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace GGPKExp {

class GGPKManager;
class DatSchema;
class AsyncTaskRunner;

// Passive skill info loaded from PassiveSkills.datc64. Everything needed for
// both the game-style tooltip and the Ctrl-held detailed view lives here.
struct PassiveSkillInfo {
    std::string name;                         // "Armour and Evasion"
    std::string id;                           // "armour_and_evasion2"
    std::string iconPath;                     // "Art/2DArt/SkillIcons/passives/...dds"
    std::string flavourText;                  // Flavor text shown at the bottom of the tooltip

    // Stat references (row indices into Stats.datc64) and their numeric values.
    std::vector<int64_t> statRows;
    std::array<int32_t, 5> statValues = {0, 0, 0, 0, 0};

    // Pre-formatted stat lines for the main tooltip ("+10 to Armour", ...).
    std::vector<std::string> formattedStats;

    // Type flags drive tooltip header color and node outline choice.
    bool isKeystone = false;
    bool isNotable = false;
    bool isJewelSocket = false;
    bool isAttribute = false;
    bool isAscendancyStartingNode = false;
    bool isJustIcon = false;
    bool isMultipleChoice = false;
    bool isMultipleChoiceOption = false;
    bool isExpansion = false;
    bool isProxyPassive = false;
    bool isAnointmentOnly = false;

    // Extended metadata shown only while Ctrl is held.
    int32_t skillType = 0;
    int32_t skillPointsGranted = 0;
    int32_t weaponPointsGranted = 0;
    int64_t ascendancy = -1;                  // foreignrow in Ascendancy
    int64_t masteryGroup = -1;                // foreignrow in PassiveSkillMasteryGroups
    int64_t groupRow = -1;                    // foreignrow ("Group" column)
    int64_t keystoneId = -1;
    int64_t atlasSubTree = -1;
    std::vector<int64_t> characters;          // foreignrow[] in Characters
    std::vector<int64_t> buffs;               // foreignrow[] in BuffTemplates
    std::vector<int64_t> reminderStrings;     // foreignrow[] in ReminderText
};

struct PsgAsyncInput {
    std::vector<uint8_t> psgBytes;
    std::vector<uint8_t> statsBytes;
    std::vector<uint8_t> passiveBytes;
    // Raw CSD files (bytes) — pre-read on main thread so the worker never
    // touches the GGPK.
    std::vector<std::vector<uint8_t>> csdBytes;
    // Value copies of the two schema tables we need. SchemaTable contains
    // only std::string + std::vector<ColumnDef> so copying is safe and the
    // worker thread reads its own instance.
    SchemaTable statsTable;
    SchemaTable passiveTable;
};

struct PsgAsyncResult {
    Parsers::PsgData psg;
    std::unordered_map<uint32_t, PassiveSkillInfo> skillInfo;
    std::unordered_map<uint32_t, uint32_t> ascNodeToClass;
    std::vector<std::string> iconPaths;

    // NEW (Task 3.4): per-language CSD format maps and stat-row-to-id table
    // moved out of worker scope so the main thread can do on-demand
    // formatting by language (Task 3.5).
    Parsers::CsdMultiLangFormatMap statFormatsByLang;
    std::unordered_map<int64_t, std::string> statRowToId;
};

class PsgViewer : public IViewer {
public:
    void SetContext(GGPKManager* mgr, DatSchema* schema, ID3D11Device* device,
                    AsyncTaskRunner* runner) {
        m_manager = mgr;
        m_schema = schema;
        m_device = device;
        m_taskRunner = runner;
    }
    // Bind a pointer to the plugin's persistent UI settings. Reads
    // settings->statDescLanguage to initialize the active language so
    // a PSG reload within the same session doesn't show stale language.
    // Safe if nullptr — combo stays hidden until CSD data arrives.
    void SetUiSettings(GGPKExplorerUiSettings* s) {
        m_settings = s;
        if (s && !s->statDescLanguage.empty()) {
            m_currentLang = s->statDescLanguage;
        }
    }
    // Consume and clear the "settings changed" flag. Mirrors MeshViewer /
    // AocViewer mechanism — GGPKExplorer polls this each frame and triggers
    // a debounced save when true.
    bool ConsumeUiSettingsDirty() {
        const bool was = m_settingsDirty;
        m_settingsDirty = false;
        return was;
    }
    void LoadData(std::vector<uint8_t> data, const std::string& path) override;
    void Clear() override;
    void Render() override;
    void RenderVisual();
    void RenderMetadata();

private:
    GGPKManager* m_manager = nullptr;
    DatSchema* m_schema = nullptr;
    ID3D11Device* m_device = nullptr;
    AsyncTaskRunner* m_taskRunner = nullptr;

    Parsers::PsgData m_psg;
    int m_selectedGroup = -1;

    // Passive skill data loaded from datc64 files + .csd stat descriptions.
    std::unordered_map<uint32_t, PassiveSkillInfo> m_skillInfo;
    bool m_skillInfoLoaded = false;

    // Per-language stat-format maps populated by the CSD loader worker.
    // Key = language name as in .csd ("English", "French", "Russian", ...).
    // Populated by Task 3.4; empty until then.
    GGPKExp::Parsers::CsdMultiLangFormatMap m_statFormatsByLang;

    // Map from stat-row index (from Stats.datc64) to stat-id string used as
    // key in m_statFormatsByLang. Populated by the worker; consumed by the
    // on-demand format path at tooltip time (Task 3.5).
    std::unordered_map<int64_t, std::string> m_statRowToId;

    // Sorted list of language keys that actually appeared in the loaded
    // CSD files. "English" first, remainder alphabetical. Populated by
    // Task 3.4. Used by the UI combobox (Task 3.5).
    std::vector<std::string> m_availableLangs;

    // Active language for stat-description formatting. Mirrors
    // m_settings->statDescLanguage; initialized when settings are bound.
    std::string m_currentLang = "English";

    // Pointer to plugin UI settings (may be nullptr before ContentPanel
    // binds it; guard all reads/writes with a null-check).
    GGPKExplorerUiSettings* m_settings = nullptr;

    // Dirty flag set when the user changes the stat-description language
    // via the combo. Consumed by ConsumeUiSettingsDirty() each frame so the
    // host can schedule a debounced settings save.
    bool m_settingsDirty = false;

    // Icon textures — eagerly loaded at PSG open time.
    PsgIconCache m_icons;
    bool m_iconsLoaded = false;

    // Rate-limited icon loading state. DDS decode triggers an NVIDIA driver
    // crash on bulk loads (nvwgf2umx.dll AV), so icons are pumped a few per
    // frame with each CreateDDSTextureFromMemory wrapped in SEH.
    enum class IconLoadState { Idle, Loading, Done };
    IconLoadState m_iconLoadState = IconLoadState::Idle;
    std::deque<std::string> m_iconQueue;
    int m_iconsLoadedThisLoad = 0;
    int m_iconsTotalThisLoad = 0;
    static constexpr int kIconsPerFrame = 24;

    void PumpIconQueue();   // Called from PollLoadState each frame

    // Class filter state: -1 = no class selected (all ascendancy nodes hidden),
    // otherwise an index into m_psg.rootNodes.
    int m_selectedClassIdx = -1;

    // For each ascendancy node (info.ascendancy >= 0), which class (root node
    // hash) reaches it via ascendancy-only BFS. Built once when PSG is loaded.
    std::unordered_map<uint32_t, uint32_t> m_ascNodeToClass;

    // Async load state
    enum class LoadState { Idle, Preparing, Loading, Ready, Error };
    LoadState m_loadState = LoadState::Idle;
    std::future<Result<PsgAsyncResult>> m_loadFuture;
    std::string m_loadingPath;

    // Deferred GGPK read input: set by LoadData, consumed by DoMainThreadPrep on
    // the second frame of Preparing state (so the spinner draws first).
    struct PsgPendingInput {
        std::vector<uint8_t> data;
        std::string path;
    };
    PsgPendingInput m_pending;
    bool m_prepareFrameDrawn = false;

    // Main-thread GGPK reads + worker dispatch. Called from PollLoadState on the
    // second Preparing frame after the spinner has rendered once.
    void DoMainThreadPrep();

    // Visual tab state
    float m_zoom = 1.0f;
    ImVec2 m_pan{0, 0};
    bool m_dragging = false;
    int m_dragButton = 0;
    ImVec2 m_dragStart{0, 0};
    ImVec2 m_panStart{0, 0};
    uint32_t m_selectedNodeHash = 0;
    uint32_t m_hoveredNodeHash = 0;
    bool m_needsFit = true;

    // Precomputed bounds for fit-to-view
    float m_minX = 0, m_maxX = 0, m_minY = 0, m_maxY = 0;

    // Node search state
    char m_searchBuf[128] = {};
    std::vector<uint32_t> m_searchMatches;           // ordered matches (node hashes)
    std::unordered_set<uint32_t> m_searchMatchSet;   // O(1) lookup for dim check
    int m_searchIndex = -1;
    std::unordered_map<uint32_t, std::string> m_searchableTextCache;
    ImVec2 m_canvasLastSize{0, 0};

    void BuildSearchableCache();
    void RebuildSearchMatches();
    void NavigateNext();
    void NavigatePrev();
    void PanToNode(uint32_t nodeHash);

    void ComputeBounds();
    void FitToView(ImVec2 availSize);
    ImVec2 WorldToScreen(float wx, float wy, ImVec2 origin, ImVec2 size) const;
    bool IsOnScreen(float wx, float wy, ImVec2 origin, ImVec2 size, float margin) const;

    const PassiveSkillInfo* GetSkillInfo(uint32_t hash) const {
        auto it = m_skillInfo.find(hash);
        return (it != m_skillInfo.end()) ? &it->second : nullptr;
    }

    // Main-thread side: runs after the async parse returns. Creates D3D
    // textures for the gathered icon paths. Blocks the main thread for
    // ~1–2 s — acceptable because the spinner has already been visible
    // during the (longer) parse phase.
    void LoadIconsSync(const std::vector<std::string>& iconPaths);

    // Advances m_loadState when the worker completes. Call at the top of
    // both RenderVisual and RenderMetadata. Returns true when the caller
    // should draw the spinner (still loading or failed-to-start) rather
    // than the real content.
    bool PollLoadState();

    // Centered spinner + dim overlay + status text, matching ContentPanel's
    // GGPK-loading spinner style. Called when PollLoadState returns true.
    void DrawLoadingSpinner(const char* statusText);

    // Pure worker function — no GGPK, no D3D, no ImGui access. Runs on an
    // AsyncTaskRunner worker thread.
    static Result<PsgAsyncResult> DoParseWork(const PsgAsyncInput& input);
};

} // namespace GGPKExp
