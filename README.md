# Simple File Cleaner

A modern, lightweight file scanner and cleaner for Linux (GNOME) and Windows. Simple File Cleaner helps keep your system organized and free of clutter by locating and safely clearing out caches, temporary data and leftover development files.

---

## Features

* **GNOME-Native Design:** Built with GTK 4 and libadwaita, with a start page, a results summary and a clean preferences dialog.
* **Three Scan Types:** Quick Scan for common cache locations, Deep Scan for development files throughout your home folder, and Scan a Folder for any folder you choose.
* **Review Before Deleting:** Every item found is listed with its size. Nothing is deleted until you review the results and confirm.
* **Protected Locations:** Folders you add under Preferences → Exclusions are never scanned or deleted, and neither is any folder that contains one. On first launch, the app offers to protect common folders such as Documents, Desktop and Projects.
* **Built-in Safeguards:** System folders, drive roots and your home folder can never be deleted, whatever the settings say. On Windows, `C:\Windows`, Program Files and ProgramData are always protected.
* **Cancellable Scans** and a **Scan History** showing how much space each clean-up recovered.

## What It Scans For

### Linux

* **Trash:** Items in `~/.local/share/Trash`.
* **Thumbnail Cache:** `~/.cache/thumbnails`.
* **Browser Caches:** Firefox, Chrome and Chromium.
* **Flatpak Application Caches:** `~/.var/app/*/cache` (the Flatpak repository itself is never touched).
* **Package Manager Caches:** yay, paru and pip.
* **Logs:** Old Xorg logs and user journal files.
* **Large, Unused Downloads:** Items in `~/Downloads` over 50 MB that have not been opened in 30 days.

### Windows

* **Recycle Bin** on all local drives.
* **Temporary Files** in `%TEMP%` that have not been used for 7 days.
* **Thumbnail Cache:** Windows Explorer thumbnail databases.
* **Browser Caches:** Chrome, Edge, Firefox and Brave.
* **Package Manager Caches:** pip, npm, Yarn and NuGet.
* **Crash Dumps and Error Reports.**
* **Large, Unused Downloads.**

### Deep Scan and Folder Scan (both platforms)

* `node_modules`, `__pycache__`, `dist`, `build`, `target`, `.gradle`, Dart and Flutter caches, and Python/Java bytecode files (`.pyc`, `.pyo`, `.class`).
* Application data folders (for example `~/.config`, `~/.local`, `~/.vscode` and `AppData`) are skipped, because they contain installed software rather than disposable build output.

## ⚠️ Use at Your Own Risk

Deleted files cannot be recovered. Deep scans in particular may find folders named `build`, `dist` or `target` inside your own projects. **Please add your important folders under Preferences → Exclusions before cleaning**, and review the results carefully before deleting.

## Disclaimer

* Developed with the assistance of Claude.
* This is a personal project; please do not spam or harass me for bug fixes or feature updates.
* The Windows version is new and has had less testing than the Linux version.

## Installing

### Linux (Flatpak)

```bash
./build.sh
```

This builds File Cleaner and installs it as a user Flatpak. It then appears in your application menu. If `flatpak-builder` is not installed, the script uses the Flatpak Builder app from Flathub instead, so it also works on immutable systems such as Bazzite and Silverblue.

### Windows

Build the Windows version from Linux with:

```bash
./build-windows.sh
```

This produces `dist/FileCleaner-windows-x64.zip`. Unzip it on Windows 10 or 11 and double-click `FileCleaner.exe`; no installation is required. The script downloads the llvm-mingw toolchain and the MSYS2 builds of GTK 4 and libadwaita (about 1.3 GB) into `.win-build/` on the first run, and runs the build inside the GNOME SDK Flatpak.

Settings are stored in `%APPDATA%\FileCleaner` and scan history in `%LOCALAPPDATA%\FileCleaner`.

## Requirements to Build

Simple File Cleaner is written in C++20 and built with CMake. To build and run it from source on Linux, your system needs:

### System Dependencies
* **A C++20 compiler** (GCC or Clang)
* **CMake** (3.16+) and **Ninja**
* **GTK 4** and **Libadwaita** development headers

On Fedora-based atomic systems (like Bazzite), these development libraries can be installed inside a development container (`toolbox` or `distrobox`) to keep your base system clean:
```bash
sudo dnf install gcc-c++ cmake ninja-build gtk4-devel libadwaita-devel
```

On Debian/Ubuntu:
```bash
sudo apt install g++ cmake ninja-build libgtk-4-dev libadwaita-1-dev
```

### Building

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/file-cleaner
```
