# Simple File Cleaner

A modern, lightweight file scanner and cleaner built for the GNOME desktop environment. Simple File Cleaner helps keep your Linux system organized and free of clutter by locating and safely clearing out unnecessary cache, temporary data, and orphaned files.

---

## Features

* **GNOME-Native Design:** Tailored to integrate cleanly with the modern GNOME desktop interface.
* **Targeted Scanning:** Deeply analyzes user and application directories for non-essential junk files.
* **Dry Run Security:** Displays a complete list of target paths for review before any file deletion takes place.


## What It Scans For

Simple File Cleaner scans specific, non-critical directories to find the following types of clutter:

* **Application Caches:** Old data stored by web browsers, software centers, and desktop apps (`~/.cache`).
* **System and User Logs:** Accumulated log files that are no longer active or required (`~/.local/share/log` or system log paths).
* **Temporary Files:** Leftover installation files, runtime data, and crash dumps (`/tmp` and user temporary directories).
* **Package Manager Leftovers:** Orphaned data or cached installation packages no longer needed by the system.
* **Thumbnail Caches:** Cached image and video thumbnails that have piled up over time (`~/.cache/thumbnails`).

## Windows

The GTK4/Libadwaita GUI here is Linux/GNOME-only (Libadwaita doesn't run on
Windows). A separate command-line edition for Windows lives in
[`windows/`](windows/) — see [`windows/README.md`](windows/README.md) for
what it scans and how to build it.

## Disclaimer

* Developed with the assistance of Claude and Gemini.
* This is a personal project; please do not spam or harass me for bug fixes or feature updates.

## use at own risk may delete important files please exculde important folders in the settings menu!

## Requirements to Build

Simple File Cleaner is written in C++20 and built with CMake. To build and run it from source, your system needs:

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

Alternatively, `build.sh` builds and installs the app as a user Flatpak (see below).
