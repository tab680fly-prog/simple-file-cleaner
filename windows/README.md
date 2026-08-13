# Simple File Cleaner — Windows Edition

A command-line port of Simple File Cleaner for Windows. The original app
(`../src`) is a GTK4/Libadwaita GUI built for GNOME and doesn't run on
Windows — Libadwaita is Linux/GNOME-only. This is a from-scratch Windows
implementation of the same scanning/cleaning idea: locate and clear out
cache, temp, and orphaned files, with a dry-run-first workflow.

It ships as a small console executable rather than a GUI.

## What it scans for

* **Recycle Bin** — queried and emptied via the Shell API.
* **Temp folders** — `%TEMP%` and `%LOCALAPPDATA%\Temp`.
* **Thumbnail & icon cache** — `%LOCALAPPDATA%\Microsoft\Windows\Explorer\thumbcache_*.db` / `iconcache_*.db`.
* **Browser caches** — Chrome, Edge, and Firefox (all profiles).
* **Package manager caches** — pip, npm.
* **Crash dumps & error reports** — `%LOCALAPPDATA%\CrashDumps`, Windows Error Reporting.
* **Large old downloads** — files in `Downloads` over 50 MB untouched for 30+ days.
* **Deep scan** (`--deep`) — walks your whole user profile for `node_modules`,
  `__pycache__`, build output folders (`dist`, `build`, `target`, `.gradle`, `.m2`),
  Dart/Flutter caches, and stale bytecode files (`.pyc`, `.obj`, `.class`).

Settings (enabled categories, excluded paths, custom scan paths) persist to
`%APPDATA%\FileCleaner\settings.json`.

## Building on Windows

Requires a C++20 compiler and CMake 3.16+. Either MSVC (Visual Studio 2022,
"Desktop development with C++" workload) or MinGW-w64 works.

```powershell
cmake -S windows -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
.\build\Release\file-cleaner-cli.exe scan
```

## Cross-compiling from Linux/macOS

For development/CI without a Windows machine, a MinGW-w64 toolchain file is
provided:

```bash
sudo apt install g++-mingw-w64-x86-64   # Debian/Ubuntu
cmake -S windows -B build-win -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=windows/toolchain-mingw.cmake
cmake --build build-win
```

This produces a statically-linked `file-cleaner-cli.exe` that needs no
extra DLLs. It can be smoke-tested under Wine.

## Usage

```
file-cleaner-cli.exe scan [--deep] [--json]
file-cleaner-cli.exe clean [--deep] [--yes]
file-cleaner-cli.exe folder <path> [--yes]
file-cleaner-cli.exe categories
```

* `scan` — dry run: lists what would be cleaned, deletes nothing.
* `clean` — scans, prints the results, then asks for confirmation before
  deleting everything found (`--yes` skips the prompt, e.g. for scheduled
  tasks).
* `folder <path>` — scans a single directory for junk subfolders (same
  categories as `--deep`) and files ≥50 MB at its top level, then offers to
  clean it.
* `categories` — lists every scan category and whether it's enabled.
* `--json` — machine-readable output for `scan`/`folder`, for scripting.

Items that fail to delete due to permissions are reported separately, with
a suggestion to re-run from an elevated ("Run as administrator") prompt.

## Disclaimer

Use at your own risk — this may delete important files. Exclude anything
important via `custom_scan_paths`/`excluded_paths` in the settings file
before running `clean`.
