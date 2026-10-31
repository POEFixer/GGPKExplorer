# Changelog

## 1.0.4

- Extracting folders and selections is several times faster (it reads whole bundles at once and writes with up to four threads), and a Cancel button in the toolbar stops a running extraction.
- Save As converts textures to PNG and tables to JSON/CSV on several CPU cores, with its own Cancel button.
- Find in Files, Replace in Files and Find references in the table viewer run much faster; replacing no longer freezes the window.
- Expand All on a large folder no longer freezes the window.
- Fixed crashes when loading, unloading or reloading (F5) during an extraction, export or search, and when a search used an invalid "\x" escape.
- Folders with non-English names work for opening and extracting. Empty files are extracted, long output paths work, and failed writes are reported.
- A file replaced with an empty one reads back correctly straight away, not only after the next save.
- The replace preview no longer skips files inside collapsed folders.
- The game path is pre-filled from the program's general settings when Smoother has none.
- .aoc files open in the same Model view as meshes: the main model with its skeleton and animations. The Text tab is unchanged.
- The 3D view is simpler: diffuse textures, one light, Solid/Wire, Grid, Bones, Follow and the animation panel. Lights, sounds, particles, states, sockets and attachments are removed.
- The model view says what is missing (the mesh data, a parent of an .aoc, the skeleton) instead of showing an empty view, and saving an .aoc in the Text tab keeps the camera and the selected clip.
- The Info tab shows a short summary with a Copy button.
- Lighter: no shadow pass every frame, and the plugin DLL is about 15% smaller.

## 1.0.3

- Follows the program language (ten languages).
- In a narrow tab the load status moves to its own line and the empty file tree's note wraps, so neither is cut off.
- Works in every game language: the table definitions come from the game data the program installs and updates, and load again when it updates them; until they arrive the table views show "Waiting for game data". The text viewer keeps its monospace font after the program changes language.
- The Skill Tree and its Metadata tab wait for the table definitions too ("Waiting for game data") and open as soon as they arrive, instead of staying on an error when a skill tree file was opened first.
- Table definitions that arrived but could not be read show why on every view that needs them, instead of "Waiting for game data" for ever; they are read again when the program updates them.
- Long lines in the search results are cut between characters, never inside one.

## 1.0.2

- Works with the separate menu window (POEFixer 366): the host hands the plugin the right drawing context for its settings tab and for its overlay.

## 1.0.1

- Rebuilt with the updated plugin SDK headers (POEFixer 365 adds optional calls for UI control names, stash objects and tabs, wider entity scans and the host automation flag). No change in what the plugin does.

## 1.0.0

- First versioned release.
