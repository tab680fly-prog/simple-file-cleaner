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

## Disclaimer

* Developed with the assistance of Claude and Gemini.
* This is a personal project; please do not spam or harass me for bug fixes or feature updates.
* as of right now this app is linux only

## use at own risk may delete important files please exculde important folders in the settings menu!

## Requirements to Build

To build and run Simple File Cleaner from source, your system needs the following development tools and libraries.

### System Dependencies
* **Python 3** (version 3.10 or higher)
* **GTK 4** and **Libadwaita** (for rendering the modern GNOME user interface elements)
* **PyGObject** (the Python bindings required to interface with GTK4 libraries)

On Fedora-based atomic systems (like Bazzite), these development libraries can be installed inside a development container (`toolbox` or `distrobox`) to keep your base system clean:
```bash
sudo dnf install python3-devel python3-gobject gtk4-devel libadwaita-devel
