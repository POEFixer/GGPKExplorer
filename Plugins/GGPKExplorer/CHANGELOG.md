# Changelog

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
