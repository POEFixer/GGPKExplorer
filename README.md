# GGPKExplorer — POE2Fixer Plugin

Current version: 1.0.3

Requires POE2Fixer 364 or newer.

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

## Install

Install it from POE2Fixer → Plugins → Plugin Manager (one click, updates included), or download `GGPKExplorer.zip` from [Releases](https://github.com/POEFixer/GGPKExplorer/releases/latest) and extract it into the POE2Fixer folder (it creates `Plugins\GGPKExplorer\`). Then enable the plugin in the Plugins tab.

## Build from source

This repository is the plugin folder from the POE2Fixer source tree (`Plugins/GGPKExplorer/`) together with the host files it compiles against, kept at the same relative paths:

- `POEFixer/plugin_sdk/` - the POE2Fixer Plugin SDK v6 headers (`PluginAbi.h`, `PluginSDK.h`)
- `POEFixer/imgui/` - the Dear ImGui sources the DLL compiles against
- `tools/plugin_i18n/gen.py` - the translation tool: after editing `Plugins/GGPKExplorer/lang/en.json` run `python tools/plugin_i18n/gen.py Plugins/GGPKExplorer` (it rewrites the embedded English header); `--gate` checks every language file and the keys the sources use

1. Install Visual Studio 2022 with the "Desktop development with C++" workload (MSVC v143, Windows 10/11 SDK).
2. Open `GGPKExplorer.sln`, select **Release | x64** and build the solution.
3. The plugin is written to `x64\Release\Plugins\GGPKExplorer\GGPKExplorer.dll` (Release builds carry no debug information, see `Directory.Build.targets`).
4. Copy `GGPKExplorer.dll` into `Plugins\GGPKExplorer\` next to the POE2Fixer executable, together with `Plugins\GGPKExplorer\lang` from this repository (same folder names).
