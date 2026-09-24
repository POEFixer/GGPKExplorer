# GGPKExplorer — POE2Fixer Plugin

A browser for Path of Exile 2's game content (`Content.ggpk` and the bundle index) inside
POEFixer: a searchable file tree with viewers for the game's formats, extraction and
export.

## What It Does

- **File tree** of the game content with a navigation bar, quick jump, bookmarks and
  keyboard shortcuts.
- **Search** by file name, by file content and inside DAT tables.
- **Viewers**:
  - DAT tables (with a schema), with a cell editor and import/export;
  - images (DDS and others), text, hex;
  - PSG passive skill graphs and the skill tree;
  - meshes (SMD), animations (AOC), materials (MAT tables), terrain;
  - audio.
- **Extract** files or folders and **Save As** into other formats.

Browsing, searching and extracting never change the game files. Saving in the DAT editor,
importing, saving in the Text tab and saving an AOC file DO write into the game content,
and those writes are flushed to disk automatically: back up your game folder before you
save an edit, and expect the game's own file check to restore modified files.

## Disclaimer

Community third-party plugin for POE2Fixer. Use at your own risk. Not affiliated with
Grinding Gear Games.

## Build from source

This repository is the plugin folder from the POEFixer source tree (`Plugins/GGPKExplorer/`) together with the host files it compiles against, kept at the same relative paths:

- `POEFixer/plugin_sdk/` - the POEFixer Plugin SDK v6 headers (`PluginAbi.h`, `PluginSDK.h`)
- `POEFixer/imgui/` - the Dear ImGui sources the DLL compiles against

1. Install Visual Studio 2022 with the "Desktop development with C++" workload (MSVC v143, Windows 10/11 SDK).
2. Open `GGPKExplorer.sln`, select **Release | x64** and build the solution.
3. The plugin is written to `x64\Release\Plugins\GGPKExplorer\GGPKExplorer.dll`.

## Install

Copy `GGPKExplorer.dll` into `Plugins\GGPKExplorer\` next to the POEFixer executable (create the folder if needed), start POEFixer and enable the plugin in the Plugins tab.
