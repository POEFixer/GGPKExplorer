// Plugins/GGPKExplorer/ui/SchemaGate.h
// Which content tabs wait for the DAT schema (game data the host installs, and may still be
// downloading) and which reopen when a new one arrives. STL only: tests/UiLogicTests.cpp checks
// it without ImGui.
#pragma once
#include "../core/FileTypeDetector.h"

#include <unordered_map>

namespace GGPKExp::SchemaGate {

// The viewers that read the schema: the DAT and Schema tabs (FileType::Dat) and the Skill Tree
// with its Metadata tab (FileType::SkillTree: PsgViewer reads Stats and PassiveSkills through
// it). Until a schema is loaded their tabs show "Waiting for game data", or why the last load
// failed (SchemaNotReadyLine, ui/UiText.h).
inline bool NeedsSchema(FileType type) {
    return type == FileType::Dat || type == FileType::SkillTree;
}

// A new schema arrived: every loaded viewer that reads it loads again on its next frame, a DAT
// table only when it holds no edits or import. A Skill Tree opened before the schema (or one
// that failed with an older schema) recovers this way.
inline void OnSchemaChanged(std::unordered_map<FileType, bool>& loaded, bool datHasPendingEdits) {
    for (auto& [type, isLoaded] : loaded) {
        if (!isLoaded || !NeedsSchema(type)) continue;
        if (type == FileType::Dat && datHasPendingEdits) continue;
        isLoaded = false;
    }
}

} // namespace GGPKExp::SchemaGate
