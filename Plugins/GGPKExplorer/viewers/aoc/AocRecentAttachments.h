#pragma once

#include <ctime>
#include <string>
#include <vector>

namespace GGPKExp {
namespace Aoc {

// Phase 5.1.5a — Global LRU list of recently attached child mesh sources.
// JSON location: {pluginDir}/user_data/recent_attachments.json
// Not per-AOC-file — shared across the whole plugin session and across runs.
class AocRecentAttachments {
public:
    struct Entry {
        std::string sourcePath;   // Lowercased GGPK path (stable for dedup)
        std::time_t lastUsedUnix = 0;
    };

    static constexpr int kMaxEntries = 15;

    // Load from disk. Safe to call repeatedly — clears previous state first.
    // Empty pluginDir is a no-op (returns true silently). Returns false only
    // on parse failure (corrupted JSON) — caller may still use empty list.
    bool Load(const std::string& pluginDir);

    // Write current list to disk (creates parent dirs). Empty pluginDir no-op.
    // Silent on I/O failure — caller is expected to log via Logger if desired.
    void Save(const std::string& pluginDir) const;

    // Bump sourcePath to the front (LRU). If already present, moved; else inserted.
    // Trims to kMaxEntries. Does NOT save — caller must invoke Save() after.
    void Record(const std::string& sourcePath);

    // Remove a single entry by path. No-op if not present. Does NOT save.
    void Remove(const std::string& sourcePath);

    // Wipe the in-memory list. Does NOT save or delete the JSON file.
    void Clear();

    const std::vector<Entry>& Entries() const { return m_entries; }
    bool IsEmpty() const { return m_entries.empty(); }

private:
    std::vector<Entry> m_entries;

    static std::string DeriveJsonPath(const std::string& pluginDir);
    static std::string ToLowerPath(const std::string& s);
};

} // namespace Aoc
} // namespace GGPKExp
