#!/usr/bin/env python3
"""
File Cleaner — GTK4 + libadwaita
Settings, custom scanning paths, animation toggle, folder scan, pkexec escalation.
Run: python3 file-cleaner.py
"""

import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")

from gi.repository import Gtk, Adw, GLib, Gio, Gdk, Pango
import cairo, os, math, shutil, subprocess, threading, time, json, datetime
from pathlib import Path
from dataclasses import dataclass, field
from typing import List, Callable, Optional


# ---------------------------------------------------------------------------
# Dark mode fix when running as root
# ---------------------------------------------------------------------------

def apply_color_scheme():
    mgr = Adw.StyleManager.get_default()
    if os.geteuid() == 0:
        mgr.set_color_scheme(Adw.ColorScheme.FORCE_DARK)
    else:
        mgr.set_color_scheme(Adw.ColorScheme.DEFAULT)


# ---------------------------------------------------------------------------
# CSS
# ---------------------------------------------------------------------------

CSS = b"""
.scan-path-label { font-family: monospace; font-size: 11px; }

.found-pill {
    border-radius: 999px;
    padding: 2px 12px;
    font-size: 11px;
    font-weight: bold;
}

.bottom-bar-box {
    padding: 12px 18px;
    border-top: 1px solid rgba(127,127,127,0.13);
}

.action-btn { min-width: 130px; }

.size-label-large { color: #e5534b; }

.cat-group-header {
    font-size: 12px;
    font-weight: 600;
    opacity: 0.75;
    padding: 2px 0;
}

.summary-bar {
    padding: 10px 16px;
    border-bottom: 1px solid rgba(127,127,127,0.13);
}

.summary-total {
    font-size: 13px;
    font-weight: 600;
}

.result-group-fading {
    transition: opacity 200ms ease;
}
"""


# ---------------------------------------------------------------------------
# Settings — persisted to ~/.config/filecleaner.json
# ---------------------------------------------------------------------------

SETTINGS_PATH = Path.home() / ".config" / "filecleaner.json"

SCAN_CATEGORIES = {
    "trash":          "Trash",
    "thumbs":         "Thumbnail cache",
    "browser_cache":  "Browser caches (Firefox, Chrome, Chromium)",
    "flatpak":        "Flatpak app caches",
    "pkg_cache":      "Package manager caches (yay, paru, pip)",
    "logs":           "Old logs & systemd journal records",
    "downloads":      "Large old downloads (>50 MB, >30 days)",
    "node_modules":   "node_modules folders",
    "pycache":        "Python __pycache__ / bytecode",
    "build_dirs":     "Build output folders (dist, build, target…)",
    "dart":           "Dart / Flutter caches",
    "stale_bytecode": "Stale bytecode files (.pyc, .o, .class)",
}

DEEP_JUNK_CATEGORY_MAP = {
    "node_modules":   {"node_modules"},
    "pycache":        {"__pycache__", ".mypy_cache", ".pytest_cache", ".ruff_cache"},
    "build_dirs":     {"dist", "build", ".gradle", ".m2", "target"},
    "dart":           {".dart_tool", ".pub-cache"},
}


# ---------------------------------------------------------------------------
# History Management
# ---------------------------------------------------------------------------

HISTORY_PATH = Path.home() / ".local" / "share" / "filecleaner" / "history.json"

@dataclass
class HistoryEntry:
    id: str
    date: str
    mode: str
    categories_found: int
    total_found: int
    total_deleted: int
    deleted_count: int

    def date_fmt(self) -> str:
        try:
            dt = datetime.datetime.fromisoformat(self.date)
            return dt.strftime("%d %b %Y, %H:%M")
        except Exception:
            return self.date

    def total_found_fmt(self) -> str:
        return fmt_size(self.total_found)

    def total_deleted_fmt(self) -> str:
        return fmt_size(self.total_deleted)


def load_history() -> List["HistoryEntry"]:
    try:
        if HISTORY_PATH.exists():
            data = json.loads(HISTORY_PATH.read_text())
            return [HistoryEntry(**e) for e in data]
    except Exception:
        pass
    return []


def save_history(entries: List["HistoryEntry"]):
    HISTORY_PATH.parent.mkdir(parents=True, exist_ok=True)
    HISTORY_PATH.write_text(json.dumps([e.__dict__ for e in entries], indent=2))


def add_history_entry(entry: "HistoryEntry"):
    entries = load_history()
    entries.insert(0, entry)
    save_history(entries[:100])


@dataclass
class Settings:
    enabled: dict = field(default_factory=lambda: {k: True for k in SCAN_CATEGORIES})
    excluded_paths: list = field(default_factory=list)
    custom_scan_paths: list = field(default_factory=list)  # User-defined paths to clean
    animation: str = "magnifier"
    auto_rescan: bool = True

    def save(self):
        SETTINGS_PATH.parent.mkdir(parents=True, exist_ok=True)
        with open(SETTINGS_PATH, "w") as f:
            json.dump({
                "enabled": self.enabled,
                "excluded_paths": self.excluded_paths,
                "custom_scan_paths": self.custom_scan_paths,
                "animation": self.animation,
                "auto_rescan": self.auto_rescan,
            }, f, indent=2)

    @classmethod
    def load(cls) -> "Settings":
        s = cls()
        try:
            if SETTINGS_PATH.exists():
                data = json.loads(SETTINGS_PATH.read_text())
                for k, v in data.get("enabled", {}).items():
                    if k in SCAN_CATEGORIES:
                        s.enabled[k] = bool(v)
                s.excluded_paths = [str(p) for p in data.get("excluded_paths", [])]
                s.custom_scan_paths = [str(p) for p in data.get("custom_scan_paths", [])]
                if data.get("animation") in ("magnifier", "spinner"):
                    s.animation = data["animation"]
                if "auto_rescan" in data:
                    s.auto_rescan = bool(data["auto_rescan"])
        except Exception:
            pass
        return s

    def cat_on(self, key: str) -> bool:
        return self.enabled.get(key, True)

    def path_excluded(self, p: Path) -> bool:
        sp = str(p)
        return any(sp.startswith(ex) for ex in self.excluded_paths)


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

@dataclass
class FileEntry:
    path: Path
    size: int
    selected: bool = True


@dataclass
class Category:
    key: str
    title: str
    subtitle: str
    icon: str
    entries: List[FileEntry] = field(default_factory=list)

    @property
    def total_size(self) -> int:
        return sum(e.size for e in self.entries)

    @property
    def selected_size(self) -> int:
        return sum(e.size for e in self.entries if e.selected)


def fmt_size(n: int) -> str:
    if n >= 1 << 30: return f"{n / (1 << 30):.1f} GB"
    if n >= 1 << 20: return f"{n / (1 << 20):.0f} MB"
    if n >= 1 << 10: return f"{n / (1 << 10):.0f} KB"
    return f"{n} B"


# ---------------------------------------------------------------------------
# Fast sizing via du
# ---------------------------------------------------------------------------

def dir_size(p: Path, progress_cb: Optional[Callable[[str], None]] = None) -> int:
    if progress_cb:
        progress_cb(str(p))
    try:
        result = subprocess.run(
            ["timeout", "8", "du", "-sb", "--", str(p)],
            capture_output=True, text=True, timeout=10)
        if result.returncode == 0:
            return int(result.stdout.split()[0])
    except (subprocess.TimeoutExpired, ValueError, IndexError, FileNotFoundError):
        pass

    total = 0
    count = 0
    try:
        for entry in os.scandir(p):
            if count > 50000:
                break
            count += 1
            try:
                if entry.is_file(follow_symlinks=False):
                    total += entry.stat().st_size
                elif entry.is_dir(follow_symlinks=False):
                    total += dir_size(Path(entry.path))
            except (PermissionError, OSError):
                pass
    except (PermissionError, OSError):
        pass
    return total


def dir_size_many(paths: List[Path], progress_cb: Optional[Callable[[str], None]] = None) -> dict:
    existing = [p for p in paths if p.exists()]
    if not existing:
        return {}
    if progress_cb:
        progress_cb(str(existing[0]))
    try:
        result = subprocess.run(
            ["timeout", "60", "du", "-sb", "--"] + [str(p) for p in existing],
            capture_output=True, text=True, timeout=65)
        sizes = {}
        for line in result.stdout.splitlines():
            parts = line.split("\t", 1)
            if len(parts) == 2:
                try:
                    sizes[Path(parts[1])] = int(parts[0])
                except ValueError:
                    pass
        return sizes
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return {p: dir_size(p, progress_cb) for p in existing}


# ---------------------------------------------------------------------------
# Scanners (Quick, Deep, Custom Paths)
# ---------------------------------------------------------------------------

def _is_writable(p: Path) -> bool:
    try:
        return os.access(p, os.W_OK)
    except OSError:
        return False


def scan_generator(progress_cb: Callable[[str], None], settings: Settings):
    home = Path.home()

    # Process Custom Target Scan Rules configured by the user
    if settings.custom_scan_paths:
        progress_cb("Querying custom rulesets...")
        custom_paths = [Path(p) for p in settings.custom_scan_paths if Path(p).exists()]
        if custom_paths:
            custom_sizes = dir_size_many(custom_paths, progress_cb)
            custom_entries = []
            for cp in custom_paths:
                if settings.path_excluded(cp):
                    continue
                sz = custom_sizes.get(cp, cp.stat().st_size if cp.is_file() else 0)
                if sz:
                    custom_entries.append(FileEntry(path=cp, size=sz))
            if custom_entries:
                yield Category("custom_paths", "Custom Targeted Paths", "User-specified scan configuration paths",
                               "folder-saved-search-symbolic", custom_entries)

    known_dirs = [
        home / ".local/share/Trash/files",
        home / ".local/share/Trash/info",
        home / ".cache/thumbnails",
        home / ".cache/mozilla/firefox",
        home / ".cache/chromium",
        home / ".config/google-chrome/Default/Cache",
        home / ".local/share/flatpak/repo",
        home / ".cache/yay",
        home / ".cache/paru",
        home / ".cache/pip",
    ]
    progress_cb("Sizing common junk paths…")
    batch_sizes = dir_size_many(known_dirs, progress_cb)

    def bsz(p: Path) -> int:
        return batch_sizes.get(p, 0)

    def excl(p: Path) -> bool:
        return settings.path_excluded(p)

    if settings.cat_on("trash"):
        progress_cb("~/.local/share/Trash")
        entries = []
        for d in [home / ".local/share/Trash/files", home / ".local/share/Trash/info"]:
            if d.exists() and not excl(d):
                try:
                    items = list(d.iterdir())
                    item_sizes = dir_size_many([i for i in items if i.is_dir()], progress_cb)
                    for item in items:
                        if excl(item):
                            continue
                        try:
                            sz = item_sizes.get(item, item.stat().st_size if item.is_file() else 0)
                            if sz:
                                entries.append(FileEntry(path=item, size=sz))
                        except (PermissionError, OSError):
                            pass
                except (PermissionError, OSError):
                    pass
        if entries:
            yield Category("trash", "Trash", "~/.local/share/Trash", "user-trash-symbolic", entries)

    if settings.cat_on("thumbs"):
        thumb = home / ".cache/thumbnails"
        if thumb.exists() and not excl(thumb):
            sz = bsz(thumb)
            if sz:
                yield Category("thumbs", "Thumbnail cache", "~/.cache/thumbnails",
                               "image-x-generic-symbolic", [FileEntry(path=thumb, size=sz)])

    if settings.cat_on("browser_cache"):
        for label, p in {
            "Firefox cache":  home / ".cache/mozilla/firefox",
            "Chromium cache": home / ".cache/chromium",
            "Chrome cache":   home / ".config/google-chrome/Default/Cache",
        }.items():
            if p.exists() and not excl(p):
                sz = bsz(p)
                if sz:
                    yield Category(f"browser_{label}", label, str(p).replace(str(home), "~"),
                                   "web-browser-symbolic", [FileEntry(path=p, size=sz)])

    if settings.cat_on("flatpak"):
        flatpak_entries = []
        user_flatpak = home / ".local/share/flatpak/repo"
        if user_flatpak.exists() and not excl(user_flatpak) and _is_writable(user_flatpak):
            sz = bsz(user_flatpak)
            if sz > 1 << 20:
                flatpak_entries.append(FileEntry(path=user_flatpak, size=sz))
        
        var_app = home / ".var/app"
        if var_app.exists() and not excl(var_app):
            try:
                app_caches = [d / "cache" for d in var_app.iterdir() if (d / "cache").exists()]
                if app_caches:
                    cache_sizes = dir_size_many(app_caches, progress_cb)
                    for path, sz in cache_sizes.items():
                        if sz > 5 << 20 and not excl(path):
                            flatpak_entries.append(FileEntry(path=path, size=sz))
            except Exception:
                pass
        if flatpak_entries:
            yield Category("flatpak_repo", "Flatpak application storage", "Flatpak cache layers & local repos",
                           "package-x-generic-symbolic", flatpak_entries)

    if settings.cat_on("pkg_cache"):
        for name in ["yay", "paru"]:
            p = home / f".cache/{name}"
            if p.exists() and not excl(p):
                sz = bsz(p)
                if sz:
                    yield Category(name, f"{name} package cache", str(p).replace(str(home), "~"),
                                   "package-x-generic-symbolic", [FileEntry(path=p, size=sz)])
        pip_cache = home / ".cache/pip"
        if pip_cache.exists() and not excl(pip_cache):
            sz = bsz(pip_cache)
            if sz:
                yield Category("pip", "pip cache", "~/.cache/pip",
                               "application-x-python-bytecode-symbolic", [FileEntry(path=pip_cache, size=sz)])

    if settings.cat_on("logs"):
        log_entries = []
        for pattern in ["Xorg.*.old", "Xorg.*.log.old"]:
            for f in home.glob(f".local/share/xorg/{pattern}"):
                if not excl(f) and _is_writable(f):
                    try:
                        log_entries.append(FileEntry(path=f, size=f.stat().st_size))
                    except OSError:
                        pass

        for jpath in [
            Path("/run/user") / str(os.getuid()) / "systemd/journal",
            home / ".local/share/systemd",
        ]:
            if jpath.exists() and not excl(jpath) and _is_writable(jpath):
                sz = dir_size(jpath, progress_cb)
                if sz:
                    log_entries.append(FileEntry(path=jpath, size=sz))
        if log_entries:
            yield Category("logs", "System logs", "Xorg diagnostic logs and local user journal instances",
                           "text-x-generic-symbolic", log_entries)

    if settings.cat_on("downloads"):
        downloads = home / "Downloads"
        if downloads.exists() and not excl(downloads):
            cutoff = time.time() - 30 * 86400
            dl_entries = []
            try:
                top_items = list(downloads.iterdir())
                progress_cb(f"Inspecting {len(top_items)} items inside Downloads…")
                dl_dir_sizes = dir_size_many([i for i in top_items if i.is_dir()], progress_cb)
                for item in top_items:
                    if excl(item):
                        continue
                    try:
                        progress_cb(str(item))
                        st = item.stat()
                        sz = st.st_size if item.is_file() else dl_dir_sizes.get(item, 0)
                        if sz >= 50 << 20 and st.st_atime < cutoff:
                            dl_entries.append(FileEntry(path=item, size=sz))
                    except (PermissionError, OSError):
                        pass
            except (PermissionError, OSError):
                pass
            if dl_entries:
                yield Category("downloads", "Large historical downloads",
                               "~/Downloads — files over 50 MB untouched for 30+ days",
                               "folder-download-symbolic", dl_entries)


# ---------------------------------------------------------------------------
# Deep scan logic
# ---------------------------------------------------------------------------

DEEP_JUNK_DIRS = {
    "node_modules", "__pycache__", ".mypy_cache", ".pytest_cache",
    ".ruff_cache", "dist", "build", ".gradle", ".m2", "target",
    ".dart_tool", ".pub-cache",
}
DEEP_JUNK_SUFFIXES = {".pyc", ".pyo", ".o", ".a", ".so.old", ".class"}
DEEP_SKIP_DIRS = {
    ".git", ".hg", ".svn", ".var", ".steam", "steam", "Steam",
    ".cargo", ".rustup", ".nvm", ".rbenv", ".rvm", ".sdkman",
    ".java", "go", "snap", "Games", "games", "Music", "Videos",
    "Pictures", "proc", "sys", "dev", "run",
}
DEEP_JUNK_META = {
    "node_modules":   ("node_modules directories",   "JavaScript code dependencies",      "code-symbolic"),
    "__pycache__":    ("Python bytecode",            "Compiled bytecode targets",         "application-x-python-bytecode-symbolic"),
    ".mypy_cache":    ("mypy cache",                 "Python type checker cache",         "text-x-generic-symbolic"),
    ".pytest_cache":  ("pytest cache",               "Test execution cache artifacts",    "text-x-generic-symbolic"),
    ".ruff_cache":    ("ruff cache",                 "Linter caching systems",            "text-x-generic-symbolic"),
    "dist":           ("Distribution targets",       "Compiled distribution outputs",     "package-x-generic-symbolic"),
    "build":          ("Build outputs",              "Build compilation folders",         "package-x-generic-symbolic"),
    ".gradle":        (".gradle environment",        "Gradle build caching blocks",       "package-x-generic-symbolic"),
    ".m2":            (".m2 local cache",            "Maven repository cache trees",      "package-x-generic-symbolic"),
    "target":         ("Rust target profiles",       "Rust/Cargo target cache artifacts", "package-x-generic-symbolic"),
    ".dart_tool":     (".dart_tool assets",          "Dart environment metadata",         "package-x-generic-symbolic"),
    ".pub-cache":     ("Dart package caches",        "Dart/Flutter external dependencies","package-x-generic-symbolic"),
    "stale_bytecode": ("Isolated binary artifacts",   "Loose .pyc, .class, and .o files",  "text-x-generic-symbolic"),
}


def _enabled_deep_junk_dirs(settings: Settings) -> set:
    enabled = set()
    for cat_key, dir_names in DEEP_JUNK_CATEGORY_MAP.items():
        if settings.cat_on(cat_key):
            enabled |= dir_names
    return enabled


def _deep_walk(root: Path, settings: Settings, progress_cb: Callable[[str], None]):
    enabled_dirs = _enabled_deep_junk_dirs(settings)
    buckets: dict[str, list[Path]] = {k: [] for k in DEEP_JUNK_DIRS}
    buckets["stale_bytecode"] = []

    def walk(p: Path, depth: int = 0):
        if depth > 10:
            return
        try:
            entries = list(os.scandir(p))
        except (PermissionError, OSError):
            return

        junk_found = []
        recurse_into = []

        for e in entries:
            ep = Path(e.path)
            if e.name in DEEP_SKIP_DIRS or settings.path_excluded(ep):
                continue
            try:
                is_dir = e.is_dir(follow_symlinks=False)
            except OSError:
                continue

            if is_dir:
                if e.name in enabled_dirs:
                    junk_found.append((e.name, ep))
                else:
                    recurse_into.append(ep)
            else:
                if (settings.cat_on("stale_bytecode")
                        and Path(e.name).suffix.lower() in DEEP_JUNK_SUFFIXES
                        and not settings.path_excluded(ep)):
                    buckets["stale_bytecode"].append(ep)

        if junk_found:
            progress_cb(f"Analyzing {len(junk_found)} artifacts in {p.name}/…")
            sizes = dir_size_many([pth for _, pth in junk_found], progress_cb)
            for key, pth in junk_found:
                if sizes.get(pth, 0):
                    buckets[key].append(pth)

        for sub in recurse_into:
            progress_cb(str(sub))
            walk(sub, depth + 1)

    walk(root)
    return buckets


def _emit_deep_buckets(buckets, settings, progress_cb, prefix="deep"):
    for key, paths in buckets.items():
        if not paths:
            continue
        title, subtitle, icon = DEEP_JUNK_META.get(key, (key, key, "folder-symbolic"))
        if key == "stale_bytecode":
            if not settings.cat_on("stale_bytecode"):
                continue
            entries = []
            for p in paths:
                try:
                    entries.append(FileEntry(path=p, size=p.stat().st_size))
                except OSError:
                    pass
        else:
            sizes = dir_size_many(paths, progress_cb)
            entries = [FileEntry(path=p, size=s) for p, s in sizes.items() if s]
        if entries:
            yield Category(f"{prefix}_{key}", title, subtitle, icon, entries)


def deep_scan_generator(progress_cb: Callable[[str], None], settings: Settings):
    home = Path.home()
    progress_cb("Querying standard file-system paths…")
    yield from scan_generator(progress_cb, settings)

    progress_cb("Performing full file-system structural search…")
    buckets = _deep_walk(home, settings, progress_cb)
    yield from _emit_deep_buckets(buckets, settings, progress_cb, prefix="deep")


def folder_scan_generator(root: Path, progress_cb: Callable[[str], None], settings: Settings):
    progress_cb(f"Inspecting directory target {root}…")
    buckets = _deep_walk(root, settings, progress_cb)

    large_entries = []
    try:
        for item in root.iterdir():
            if item.is_file(follow_symlinks=False) and not settings.path_excluded(item):
                try:
                    sz = item.stat().st_size
                    if sz >= 50 << 20:
                        large_entries.append(FileEntry(path=item, size=sz))
                except OSError:
                    pass
    except (PermissionError, OSError):
        pass
    if large_entries:
        yield Category("folder_large", "Large root objects", f"Objects ≥50 MB inside {root.name}",
                       "folder-download-symbolic", large_entries)

    yield from _emit_deep_buckets(buckets, settings, progress_cb, prefix="folder")


# ---------------------------------------------------------------------------
# Cairo Drawing Area Rendering (Fixed Checkmark Vectors)
# ---------------------------------------------------------------------------

class ScanRing(Gtk.DrawingArea):
    SIZE = 160

    def __init__(self, style: str = "magnifier"):
        super().__init__()
        self.set_size_request(self.SIZE, self.SIZE)
        self.set_draw_func(self._draw)
        self._angle = 0.0
        self._done_progress = 0.0
        self._timer_id = None
        self._done_timer = None
        self._running = False
        self._done_ok = False
        self._style = style
        self._r, self._g, self._b = 0.208, 0.518, 0.894

    def set_style(self, style: str):
        self._style = style

    def start(self):
        if hasattr(self, "_done_timer") and self._done_timer:
            GLib.source_remove(self._done_timer)
            self._done_timer = None
        self._running = True
        self._done_ok = False
        self._done_progress = 0.0
        if not self._timer_id:
            self._timer_id = GLib.timeout_add(16, self._tick)

    def stop(self, success: bool = True):
        self._running = False
        self._done_ok = success
        if self._timer_id:
            GLib.source_remove(self._timer_id)
            self._timer_id = None
        if success:
            self._done_timer = GLib.timeout_add(16, self._done_tick)
        else:
            self.queue_draw()

    def _tick(self):
        if not self._running:
            self._timer_id = None
            return GLib.SOURCE_REMOVE
        self._angle = (self._angle + 3.0) % 360.0
        self._refresh_accent()
        self.queue_draw()
        return GLib.SOURCE_CONTINUE

    def _done_tick(self):
        self._done_progress = min(1.0, self._done_progress + 0.05)
        self.queue_draw()
        return GLib.SOURCE_REMOVE if self._done_progress >= 1.0 else GLib.SOURCE_CONTINUE

    def _refresh_accent(self):
        try:
            ok, c = self.get_style_context().lookup_color("accent_color")
            if ok:
                self._r, self._g, self._b = c.red, c.green, c.blue
        except Exception:
            pass

    def _draw(self, area, cr, w, h):
        if self._style == "spinner":
            self._draw_spinner(cr, w, h)
        else:
            self._draw_magnifier_style(cr, w, h)

    def _draw_magnifier_style(self, cr, w, h):
        cx, cy = w / 2, h / 2
        r, g, b = self._r, self._g, self._b
        circle_r  = 58
        inner_orb = 24

        if self._done_ok:
            p = self._done_progress
            fr = r + (0.20 - r) * p
            fg = g + (0.78 - g) * p
            fb = b + (0.35 - b) * p
            cr.set_source_rgba(fr, fg, fb, 1.0)
        else:
            cr.set_source_rgba(r, g, b, 1.0)
        cr.arc(cx, cy, circle_r, 0, 2 * math.pi)
        cr.fill()

        if self._running:
            rad = math.radians(self._angle - 90)
            mx = cx + inner_orb * math.cos(rad)
            my = cy + inner_orb * math.sin(rad)
            self._draw_magnifier(cr, mx, my)
        elif self._done_ok:
            self._draw_checkmark_vector(cr, cx, cy, self._done_progress, is_inverted=True)

    def _draw_magnifier(self, cr, mx, my):
        lens_r, handle_l, lw = 17.0, 14.0, 3.0
        cr.save()
        cr.translate(mx, my)

        cr.arc(0, 0, lens_r, 0, 2 * math.pi)
        cr.set_source_rgba(1.0, 1.0, 1.0, 0.20)
        cr.fill()

        cr.arc(0, 0, lens_r, 0, 2 * math.pi)
        cr.set_source_rgba(1.0, 1.0, 1.0, 1.0)
        cr.set_line_width(lw)
        cr.stroke()

        cr.arc(0, 0, lens_r - 5, math.radians(210), math.radians(285))
        cr.set_source_rgba(1.0, 1.0, 1.0, 0.50)
        cr.set_line_width(lw - 1.0)
        cr.stroke()

        hx = lens_r * math.cos(math.radians(45))
        hy = lens_r * math.sin(math.radians(45))
        cr.move_to(hx, hy)
        cr.line_to(hx + handle_l, hy + handle_l)
        cr.set_source_rgba(1.0, 1.0, 1.0, 1.0)
        cr.set_line_width(lw + 1.5)
        cr.set_line_cap(cairo.LINE_CAP_ROUND)
        cr.stroke()
        cr.restore()

    def _draw_spinner(self, cr, w, h):
        cx, cy = w / 2, h / 2
        r, g, b = self._r, self._g, self._b
        track_r = 52
        lw = 6 

        if self._running:
            cr.set_line_cap(cairo.LINE_CAP_ROUND)
            cr.set_line_width(lw)
            
            a0 = math.radians(self._angle - 90)
            a1 = a0 + (math.pi / 2)
            
            cr.arc(cx, cy, track_r, a0, a1)
            cr.set_source_rgba(r, g, b, 1.0)
            cr.stroke()

        elif self._done_ok:
            prog = self._done_progress
            
            cr.set_line_width(lw)
            cr.set_line_cap(cairo.LINE_CAP_ROUND)
            cr.arc(cx, cy, track_r, -math.pi / 2, -math.pi / 2 + 2 * math.pi * prog)
            cr.set_source_rgba(0.20, 0.78, 0.35, 1.0)
            cr.stroke()

            if prog >= 1.0:
                self._draw_checkmark_vector(cr, cx, cy, 1.0, is_inverted=False)
        else:
            cr.set_source_rgba(r, g, b, 0.12)
            cr.set_line_width(lw)
            cr.arc(cx, cy, track_r, 0, 2 * math.pi)
            cr.stroke()

    def _draw_checkmark_vector(self, cr, cx, cy, progress, is_inverted=False):
        if progress < 0.01:
            return
            
        tick_p = max(0.0, (progress - 0.2) / 0.8) if is_inverted else progress
        if tick_p <= 0:
            return

        cr.save()
        cr.translate(cx, cy)
        cr.set_line_width(5.0)
        cr.set_line_cap(cairo.LINE_CAP_ROUND)
        cr.set_line_join(cairo.LINE_JOIN_ROUND)
        
        if is_inverted:
            cr.set_source_rgba(1.0, 1.0, 1.0, 1.0)
        else:
            cr.set_source_rgba(0.20, 0.78, 0.35, 1.0)

        p1x, p1y = -14.0,  -1.0
        p2x, p2y =  -4.0,   9.0
        p3x, p3y =  12.0,  -9.0

        seg1_p = min(1.0, tick_p * 2.5)
        seg2_p = max(0.0, (tick_p - 0.4) * 1.66)

        cr.begin_new_path()
        
        if seg1_p > 0:
            cr.move_to(p1x, p1y)
            cr.line_to(p1x + (p2x - p1x) * seg1_p, p1y + (p2y - p1y) * seg1_p)
            
        if seg2_p > 0:
            cr.move_to(p2x, p2y)
            cr.line_to(p2x + (p3x - p2x) * seg2_p, p2y + (p3y - p2y) * seg2_p)
            
        cr.stroke()
        cr.restore()


# ---------------------------------------------------------------------------
# Layout Containers & Views
# ---------------------------------------------------------------------------

class ScanPage(Gtk.Box):
    def __init__(self, settings: Settings):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self._settings = settings
        self.set_valign(Gtk.Align.CENTER)
        self.set_vexpand(True)

        center = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=16)
        center.set_halign(Gtk.Align.CENTER)
        center.set_valign(Gtk.Align.CENTER)
        center.set_vexpand(True)
        center.set_margin_top(48)
        center.set_margin_bottom(32)
        self.append(center)

        self._ring_box = Gtk.Box()
        self._ring_box.set_halign(Gtk.Align.CENTER)
        center.append(self._ring_box)

        self._ring = ScanRing(style=settings.animation)
        self._ring_box.append(self._ring)

        self._title = Gtk.Label(label="Scanning…")
        self._title.set_css_classes(["title-3"])
        center.append(self._title)

        self._path_label = Gtk.Label(label="")
        self._path_label.set_css_classes(["scan-path-label", "dim-label"])
        self._path_label.set_max_width_chars(52)
        self._path_label.set_ellipsize(Pango.EllipsizeMode.MIDDLE)
        self._path_label.set_halign(Gtk.Align.CENTER)
        center.append(self._path_label)

        self._found_label = Gtk.Label(label="")
        self._found_label.set_css_classes(["found-pill", "accent"])
        self._found_label.set_halign(Gtk.Align.CENTER)
        self._found_label.set_visible(False)
        center.append(self._found_label)

    def refresh_style(self):
        self._ring.set_style(self._settings.animation)

    def start(self, label: str = "Scanning…"):
        self._ring.set_style(self._settings.animation)
        self._ring.start()
        self._title.set_label(label)
        self._path_label.set_label("")
        self._found_label.set_visible(False)

    def stop(self):
        self._ring.stop(success=True)

    def set_path(self, path: str):
        home = str(Path.home())
        self._path_label.set_label(path.replace(home, "~") if path.startswith(home) else path)

    def set_found(self, n: int, total: int):
        if n == 0:
            self._found_label.set_visible(False)
        else:
            self._found_label.set_label(f"Found {n} categor{'y' if n == 1 else 'ies'} · {fmt_size(total)}")
            self._found_label.set_visible(True)

    def set_done(self, n: int, total: int):
        self.stop()
        if n == 0:
            self._title.set_label("Nothing found")
            self._path_label.set_label("Your system configuration is verified clean")
        else:
            self._title.set_label("Scan complete")
            self._path_label.set_label(f"{n} categor{'y' if n == 1 else 'ies'} · {fmt_size(total)} queue target size")


class SettingsWindow(Adw.PreferencesWindow):
    def __init__(self, parent, settings: Settings, on_close: Callable):
        super().__init__()
        self.set_transient_for(parent)
        self.set_modal(True)
        self.set_title("Preferences")
        self.set_default_size(520, 600)
        self._settings = settings
        self._on_close = on_close
        self._excl_model = Gtk.StringList()

        self._build()
        self.connect("close-request", self._on_close_request)

    def _build(self):
        # PAGE 1: Core Scanning Configuration
        scan_page = Adw.PreferencesPage()
        scan_page.set_title("Scanning")
        scan_page.set_icon_name("folder-saved-search-symbolic")
        self.add(scan_page)

        cat_group = Adw.PreferencesGroup()
        cat_group.set_title("Categories")
        cat_group.set_description("Uncheck specific runtime paths to omit them from analysis tasks")
        scan_page.add(cat_group)

        for key, label in SCAN_CATEGORIES.items():
            row = Adw.SwitchRow()
            row.set_title(label)
            row.set_active(self._settings.cat_on(key))
            row.connect("notify::active",
                        lambda r, _, k=key: self._settings.enabled.__setitem__(k, r.get_active()))
            cat_group.add(row)

        # PAGE 2: Custom Rules Config
        custom_page = Adw.PreferencesPage()
        custom_page.set_title("Custom Rules")
        custom_page.set_icon_name("edit-find-symbolic")
        self.add(custom_page)

        custom_group = Adw.PreferencesGroup()
        custom_group.set_title("Custom Scanning Locations")
        custom_group.set_description("Register files or folders to be matched dynamically during scan routines")
        custom_page.add(custom_group)

        self._custom_listbox = Gtk.ListBox()
        self._custom_listbox.set_css_classes(["boxed-list"])
        self._custom_listbox.set_selection_mode(Gtk.SelectionMode.NONE)
        custom_group.add(self._custom_listbox)
        self._refresh_custom_list()

        custom_row = Adw.ActionRow()
        custom_row.set_title("Target new path")

        self._custom_entry = Gtk.Entry()
        self._custom_entry.set_placeholder_text("/home/user/.cache/app_logs")
        self._custom_entry.set_hexpand(True)
        self._custom_entry.set_valign(Gtk.Align.CENTER)

        custom_add_btn = Gtk.Button(label="Add")
        custom_add_btn.set_css_classes(["suggested-action"])
        custom_add_btn.set_valign(Gtk.Align.CENTER)
        custom_add_btn.connect("clicked", self._on_add_custom)

        custom_browse_btn = Gtk.Button()
        custom_browse_btn.set_icon_name("folder-open-symbolic")
        custom_browse_btn.set_valign(Gtk.Align.CENTER)
        custom_browse_btn.set_css_classes(["flat"])
        custom_browse_btn.connect("clicked", self._on_browse_custom)

        custom_row.add_suffix(self._custom_entry)
        custom_row.add_suffix(custom_browse_btn)
        custom_row.add_suffix(custom_add_btn)
        custom_group.add(custom_row)

        # PAGE 3: Exclusions Layout
        excl_page = Adw.PreferencesPage()
        excl_page.set_title("Exclusions")
        excl_page.set_icon_name("changes-prevent-symbolic")
        self.add(excl_page)

        excl_group = Adw.PreferencesGroup()
        excl_group.set_title("Excluded paths")
        excl_group.set_description("Target rules defined here preserve matching directories from modification")
        excl_page.add(excl_group)

        self._excl_listbox = Gtk.ListBox()
        self._excl_listbox.set_css_classes(["boxed-list"])
        self._excl_listbox.set_selection_mode(Gtk.SelectionMode.NONE)
        excl_group.add(self._excl_listbox)
        self._refresh_excl_list()

        add_row = Adw.ActionRow()
        add_row.set_title("Add exclusion path")

        self._excl_entry = Gtk.Entry()
        self._excl_entry.set_placeholder_text("/home/user/development")
        self._excl_entry.set_hexpand(True)
        self._excl_entry.set_valign(Gtk.Align.CENTER)

        add_btn = Gtk.Button(label="Add")
        add_btn.set_css_classes(["suggested-action"])
        add_btn.set_valign(Gtk.Align.CENTER)
        add_btn.connect("clicked", self._on_add_excl)

        browse_btn = Gtk.Button()
        browse_btn.set_icon_name("folder-open-symbolic")
        browse_btn.set_tooltip_text("Browse…")
        browse_btn.set_valign(Gtk.Align.CENTER)
        browse_btn.set_css_classes(["flat"])
        browse_btn.connect("clicked", self._on_browse_excl)

        add_row.add_suffix(self._excl_entry)
        add_row.add_suffix(browse_btn)
        add_row.add_suffix(add_btn)
        excl_group.add(add_row)

        # PAGE 4: Appearance Configuration
        appear_page = Adw.PreferencesPage()
        appear_page.set_title("Appearance")
        appear_page.set_icon_name("applications-graphics-symbolic")
        self.add(appear_page)

        anim_group = Adw.PreferencesGroup()
        anim_group.set_title("Scan animation")
        anim_group.set_description("Choose interface rendering pattern utilized during scans")
        appear_page.add(anim_group)

        mag_row = Adw.ActionRow()
        mag_row.set_title("Magnifying glass")
        mag_row.set_subtitle("Filled circle with tracking loop")
        mag_radio = Gtk.CheckButton()
        mag_radio.set_valign(Gtk.Align.CENTER)
        mag_radio.set_active(self._settings.animation == "magnifier")
        mag_row.add_prefix(mag_radio)
        mag_row.set_activatable_widget(mag_radio)
        anim_group.add(mag_row)

        spin_row = Adw.ActionRow()
        spin_row.set_title("Spinner")
        spin_row.set_subtitle("Clean architectural loading vector")
        spin_radio = Gtk.CheckButton()
        spin_radio.set_valign(Gtk.Align.CENTER)
        spin_radio.set_group(mag_radio)
        spin_radio.set_active(self._settings.animation == "spinner")
        spin_row.add_prefix(spin_radio)
        spin_row.set_activatable_widget(spin_radio)
        anim_group.add(spin_row)

        mag_radio.connect("toggled", lambda b: self._set_anim("magnifier") if b.get_active() else None)
        spin_radio.connect("toggled", lambda b: self._set_anim("spinner") if b.get_active() else None)

        behaviour_group = Adw.PreferencesGroup()
        behaviour_group.set_title("Behaviour")
        appear_page.add(behaviour_group)

        rescan_row = Adw.SwitchRow()
        rescan_row.set_title("Rescan automatically after deleting")
        rescan_row.set_subtitle("Refreshes active mode targets instantly to present state updates")
        rescan_row.set_active(self._settings.auto_rescan)
        rescan_row.connect("notify::active", lambda r, _: setattr(self._settings, "auto_rescan", r.get_active()))
        behaviour_group.add(rescan_row)

        # PAGE 5: History Diagnostics
        history_page = Adw.PreferencesPage()
        history_page.set_title("History")
        history_page.set_icon_name("document-open-recent-symbolic")
        self.add(history_page)

        self._hist_group = Adw.PreferencesGroup()
        self._hist_group.set_title("Scan records")
        self._hist_group.set_description("Historical telemetry items indexed over time")
        history_page.add(self._hist_group)

        clear_btn = Gtk.Button(label="Clear records")
        clear_btn.set_css_classes(["destructive-action"])
        clear_btn.set_valign(Gtk.Align.CENTER)
        clear_btn.connect("clicked", self._on_clear_history)
        self._hist_group.set_header_suffix(clear_btn)

        self._populate_history()

    def _set_anim(self, style: str):
        self._settings.animation = style

    def _populate_history(self):
        child = self._hist_group.get_first_child()
        while child:
            nxt = child.get_next_sibling()
            try: self._hist_group.remove(child)
            except Exception: pass
            child = nxt

        entries = load_history()
        if not entries:
            row = Adw.ActionRow()
            row.set_title("No execution records indexed")
            row.set_sensitive(False)
            self._hist_group.add(row)
            return

        mode_labels = {"quick": "Quick scan", "deep": "Deep scan", "folder": "Folder scan"}
        for entry in entries[:50]:
            row = Adw.ActionRow()
            mode_str = mode_labels.get(entry.mode, entry.mode.capitalize())
            row.set_title(f"{mode_str} — {entry.total_found_fmt()} mapped")
            deleted_str = f" · Cleared {entry.deleted_count} paths ({entry.total_deleted_fmt()})" if entry.deleted_count > 0 else ""
            row.set_subtitle(f"{entry.date_fmt()}{deleted_str}")
            icon = Gtk.Image.new_from_icon_name("folder-saved-search-symbolic")
            icon.set_valign(Gtk.Align.CENTER)
            row.add_prefix(icon)
            self._hist_group.add(row)

    def _on_clear_history(self, *_):
        dialog = Adw.MessageDialog(
            transient_for=self,
            heading="Clear records?",
            body="Historical data logs will be dropped permanently."
        )
        dialog.add_response("cancel", "Cancel")
        dialog.add_response("clear", "Clear")
        dialog.set_response_appearance("clear", Adw.ResponseAppearance.DESTRUCTIVE)
        
        def _handle_response(_, response):
            if response == "clear":
                save_history([])
                self._populate_history()
            dialog.destroy()
                
        dialog.connect("response", _handle_response)
        dialog.present()

    def _refresh_excl_list(self):
        child = self._excl_listbox.get_first_child()
        while child:
            nxt = child.get_next_sibling()
            self._excl_listbox.remove(child)
            child = nxt

        for i, path_str in enumerate(self._settings.excluded_paths):
            row = Adw.ActionRow()
            row.set_title(path_str)
            del_btn = Gtk.Button()
            del_btn.set_icon_name("list-remove-symbolic")
            del_btn.set_css_classes(["flat", "destructive-action"])
            del_btn.set_valign(Gtk.Align.CENTER)
            del_btn.connect("clicked", self._on_remove_excl, i)
            row.add_suffix(del_btn)
            self._excl_listbox.append(row)

        if not self._settings.excluded_paths:
            empty_row = Adw.ActionRow()
            empty_row.set_title("No preservation paths configured")
            empty_row.set_sensitive(False)
            self._excl_listbox.append(empty_row)

    def _refresh_custom_list(self):
        child = self._custom_listbox.get_first_child()
        while child:
            nxt = child.get_next_sibling()
            self._custom_listbox.remove(child)
            child = nxt

        for i, path_str in enumerate(self._settings.custom_scan_paths):
            row = Adw.ActionRow()
            row.set_title(path_str)
            del_btn = Gtk.Button()
            del_btn.set_icon_name("list-remove-symbolic")
            del_btn.set_css_classes(["flat", "destructive-action"])
            del_btn.set_valign(Gtk.Align.CENTER)
            del_btn.connect("clicked", self._on_remove_custom, i)
            row.add_suffix(del_btn)
            self._custom_listbox.append(row)

        if not self._settings.custom_scan_paths:
            empty_row = Adw.ActionRow()
            empty_row.set_title("No custom targets defined")
            empty_row.set_sensitive(False)
            self._custom_listbox.append(empty_row)

    def _on_add_excl(self, *_):
        text = self._excl_entry.get_text().strip()
        if text and text not in self._settings.excluded_paths:
            self._settings.excluded_paths.append(text)
            self._excl_entry.set_text("")
            self._refresh_excl_list()

    def _on_browse_excl(self, *_):
        dialog = Gtk.FileDialog()
        dialog.select_folder(self, None, self._on_browse_excl_done)

    def _on_browse_excl_done(self, dialog, result):
        try: folder = dialog.select_folder_finish(result)
        except Exception: return
        if folder:
            p = folder.get_path()
            if p and p not in self._settings.excluded_paths:
                self._settings.excluded_paths.append(p)
                self._refresh_excl_list()

    def _on_remove_excl(self, btn, index):
        try: self._settings.excluded_paths.pop(index)
        except IndexError: pass
        self._refresh_excl_list()

    def _on_add_custom(self, *_):
        text = self._custom_entry.get_text().strip()
        if text and text not in self._settings.custom_scan_paths:
            self._settings.custom_scan_paths.append(text)
            self._custom_entry.set_text("")
            self._refresh_custom_list()

    def _on_browse_custom(self, *_):
        dialog = Gtk.FileDialog()
        dialog.select_folder(self, None, self._on_browse_custom_done)

    def _on_browse_custom_done(self, dialog, result):
        try: folder = dialog.select_folder_finish(result)
        except Exception: return
        if folder:
            p = folder.get_path()
            if p and p not in self._settings.custom_scan_paths:
                self._settings.custom_scan_paths.append(p)
                self._refresh_custom_list()

    def _on_remove_custom(self, btn, index):
        try: self._settings.custom_scan_paths.pop(index)
        except IndexError: pass
        self._refresh_custom_list()

    def _on_close_request(self, *_):
        self._settings.save()
        self._on_close()
        return False


# ---------------------------------------------------------------------------
# File Mapping Rows
# ---------------------------------------------------------------------------

def open_in_files(path: Path):
    parent = path.parent if path.is_file() else path
    try: Gio.AppInfo.launch_default_for_uri(parent.as_uri(), None)
    except Exception: subprocess.Popen(["xdg-open", str(parent)])


class FileRow(Adw.ActionRow):
    def __init__(self, entry: FileEntry, on_toggle: Callable):
        super().__init__()
        self._entry = entry
        short = entry.path.name or str(entry.path)
        self.set_title(GLib.markup_escape_text(short))
        self.set_subtitle(GLib.markup_escape_text(str(entry.path).replace(str(Path.home()), "~")))
        self.set_subtitle_lines(1)

        check = Gtk.CheckButton()
        check.set_active(entry.selected)
        check.set_valign(Gtk.Align.CENTER)
        check.connect("toggled", lambda b: on_toggle(entry, b.get_active()))
        self.add_prefix(check)

        icon_name = "folder-symbolic" if entry.path.is_dir() else "text-x-generic-symbolic"
        icon = Gtk.Image.new_from_icon_name(icon_name)
        icon.set_pixel_size(16)
        icon.set_css_classes(["dim-label"])
        icon.set_valign(Gtk.Align.CENTER)
        self.add_prefix(icon)

        size_label = Gtk.Label(label=fmt_size(entry.size))
        size_label.set_valign(Gtk.Align.CENTER)
        size_label.set_css_classes([("size-label-large" if entry.size >= 500 << 20 else "dim-label"), "caption", "numeric"])
        self.add_suffix(size_label)

        open_btn = Gtk.Button()
        open_btn.set_icon_name("folder-open-symbolic")
        open_btn.set_valign(Gtk.Align.CENTER)
        open_btn.set_css_classes(["flat"])
        open_btn.connect("clicked", lambda _: open_in_files(entry.path))
        self.add_suffix(open_btn)


# ---------------------------------------------------------------------------
# Elevation Engine
# ---------------------------------------------------------------------------

PKEXEC_HELPER = """\
#!/usr/bin/env python3
import sys, os, shutil
for p in sys.stdin.read().splitlines():
    p = p.strip()
    if not p: continue
    try:
        if os.path.isdir(p): shutil.rmtree(p)
        else: os.unlink(p)
        print("OK", p)
    except Exception as e:
        print("ERR", p, str(e))
"""

def delete_with_pkexec(paths: List[Path]):
    import tempfile, stat as st
    tmp = tempfile.NamedTemporaryFile(mode="w", suffix=".py", prefix="filecleaner_helper_", delete=False)
    tmp.write(PKEXEC_HELPER)
    tmp.close()
    os.chmod(tmp.name, st.S_IRWXU | st.S_IRGRP | st.S_IXGRP | st.S_IROTH | st.S_IXOTH)

    deleted_count, errors = 0, []
    try:
        result = subprocess.run(["pkexec", "python3", tmp.name], input="\n".join(str(p) for p in paths) + "\n", capture_output=True, text=True)
        if result.returncode == 126:
            return 0, []
        for line in result.stdout.splitlines():
            if line.startswith("OK "): deleted_count += 1
            elif line.startswith("ERR "): errors.append(line[4:])
        if result.returncode not in (0, 1):
            errors.append(f"pkexec terminal process runtime fault: {result.returncode}")
    except FileNotFoundError:
        errors.append("pkexec binary missing from terminal context path")
    finally:
        try: os.unlink(tmp.name)
        except OSError: pass
    return deleted_count, errors


def delete_direct(paths: List[Path]):
    deleted_count, errors = 0, []
    for p in paths:
        try:
            if p.is_dir(): shutil.rmtree(p)
            else: p.unlink()
            deleted_count += 1
        except PermissionError:
            return None, ["permission_error"]
        except Exception as exc:
            errors.append(f"{p.name}: {exc}")
    return deleted_count, errors


# ---------------------------------------------------------------------------
# Window Architecture & Workflow Lifecycle
# ---------------------------------------------------------------------------

APP_VERSION = "1.2.0"

class FileCleanerWindow(Adw.ApplicationWindow):
    def __init__(self, app, settings: Settings):
        super().__init__(application=app)
        self.set_title(f"File Cleaner")
        self.set_default_size(640, 720)
        self.set_size_request(480, 420)

        self._settings = settings
        self._categories: List[Category] = []
        self._scanning = False
        self._scan_mode = "quick"
        self._scan_root: Optional[Path] = None
        self._post_delete = False
        self._last_history_id = None
        self._scan_gen = 0

        self._build_ui()

    def _build_ui(self):
        toolbar_view = Adw.ToolbarView()
        self.set_content(toolbar_view)

        header = Adw.HeaderBar()
        toolbar_view.add_top_bar(header)

        title_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        title_box.set_halign(Gtk.Align.CENTER)
        title_label = Gtk.Label(label="File Cleaner")
        title_label.set_css_classes(["title-4"])
        version_label = Gtk.Label(label=f"v{APP_VERSION}")
        version_label.set_css_classes(["dim-label", "caption"])
        title_box.append(title_label)
        title_box.append(version_label)
        header.set_title_widget(title_box)

        settings_btn = Gtk.Button()
        settings_btn.set_icon_name("preferences-system-symbolic")
        settings_btn.connect("clicked", self._on_open_settings)
        header.pack_end(settings_btn)

        self._stack = Gtk.Stack()
        self._stack.set_transition_type(Gtk.StackTransitionType.CROSSFADE)
        self._stack.set_transition_duration(220)
        toolbar_view.set_content(self._stack)

        placeholder = Adw.StatusPage()
        placeholder.set_icon_name("user-trash-symbolic")
        placeholder.set_title("Ready")
        placeholder.set_description("Select a structural scanning pattern to review temporary file targets.")
        self._stack.add_named(placeholder, "empty")

        self._scan_page = ScanPage(self._settings)
        self._stack.add_named(self._scan_page, "scanning")

        scroll = Gtk.ScrolledWindow()
        scroll.set_vexpand(True)
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        self._stack.add_named(scroll, "results")
        self._results_scroll = scroll

        clamp = Adw.Clamp()
        clamp.set_maximum_size(800)
        scroll.set_child(clamp)

        self._list_box_outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        self._list_box_outer.set_margin_top(18)
        self._list_box_outer.set_margin_bottom(18)
        self._list_box_outer.set_margin_start(12)
        self._list_box_outer.set_margin_end(12)
        clamp.set_child(self._list_box_outer)

        adj = scroll.get_vadjustment()
        adj.connect("value-changed", self._on_scroll_changed)

        bottom_bar = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        bottom_bar.add_css_class("bottom-bar-box")

        btn_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        btn_row.set_halign(Gtk.Align.CENTER)
        bottom_bar.append(btn_row)

        scan_menu = Gio.Menu()
        scan_menu.append("Quick scan (standard targets)", "win.scan-quick")
        scan_menu.append("Deep scan (full search)", "win.scan-deep")
        scan_menu.append("Isolate folder mapping…", "win.scan-folder")

        self._scan_btn = Gtk.MenuButton(label="Scan")
        self._scan_btn.set_css_classes(["suggested-action", "action-btn"])
        self._scan_btn.set_menu_model(scan_menu)
        btn_row.append(self._scan_btn)

        self._delete_btn = Gtk.Button(label="Clear targets")
        self._delete_btn.set_css_classes(["destructive-action", "action-btn"])
        self._delete_btn.set_sensitive(False)
        self._delete_btn.connect("clicked", self._on_delete)
        btn_row.append(self._delete_btn)

        self._status_label = Gtk.Label(label="Execution runtime standing by")
        self._status_label.set_css_classes(["dim-label", "caption"])
        self._status_label.set_halign(Gtk.Align.CENTER)
        self._status_label.set_margin_top(6)
        bottom_bar.append(self._status_label)

        toolbar_view.add_bottom_bar(bottom_bar)

        for name, cb in [
            ("scan-quick",  self._on_scan_quick),
            ("scan-deep",   self._on_scan_deep),
            ("scan-folder", self._on_scan_folder),
        ]:
            action = Gio.SimpleAction.new(name, None)
            action.connect("activate", cb)
            self.add_action(action)

        self._stack.set_visible_child_name("empty")

    def _on_open_settings(self, *_):
        SettingsWindow(self, self._settings, on_close=self._on_settings_closed).present()

    def _on_settings_closed(self):
        self._scan_page.refresh_style()

    def _on_scan_quick(self, action, param): self._start_scan(mode="quick", root=None)
    def _on_scan_deep(self, action, param): self._start_scan(mode="deep", root=None)
    def _on_scan_folder(self, action, param):
        dialog = Gtk.FileDialog()
        dialog.select_folder(self, None, self._on_folder_chosen)

    def _on_folder_chosen(self, dialog, result):
        try: folder = dialog.select_folder_finish(result)
        except Exception: return
        if folder: self._start_scan(mode="folder", root=Path(folder.get_path()))

    def _start_scan(self, mode: str, root: Optional[Path]):
        if self._scanning: return
        self._scanning = True
        self._scan_mode = mode
        self._scan_root = root
        self._categories = []
        self._scan_btn.set_sensitive(False)
        self._delete_btn.set_sensitive(False)
        self._delete_btn.set_label("Clear targets")

        lbl = f"Scanning {root.name}…" if (mode == "folder" and root) else ("Rescanning targets…" if self._post_delete else "Scanning filesystem structures…")
        self._status_label.set_label(lbl)

        child = self._list_box_outer.get_first_child()
        while child:
            nxt = child.get_next_sibling()
            self._list_box_outer.remove(child)
            child = nxt

        self._scan_gen += 1
        my_gen = self._scan_gen

        self._scan_page.start(lbl)
        self._stack.set_visible_child_name("scanning")

        threading.Thread(target=self._scan_worker, args=(mode, root, my_gen), daemon=True).start()

    def _scan_worker(self, mode: str, root: Optional[Path], gen: int):
        last_update = 0.0
        found_bytes = 0

        def progress(path: str):
            nonlocal last_update
            now = time.monotonic()
            if now - last_update > 0.125:
                last_update = now
                GLib.idle_add(self._scan_page.set_path, path)

        if mode == "quick": gen_iter = scan_generator(progress, self._settings)
        elif mode == "deep": gen_iter = deep_scan_generator(progress, self._settings)
        else: gen_iter = folder_scan_generator(root, progress, self._settings)

        for cat in gen_iter:
            found_bytes += cat.total_size
            GLib.idle_add(self._on_category_found, cat, found_bytes, gen)

        GLib.idle_add(self._scan_done, gen)

    def _on_category_found(self, cat: Category, running_total: int, gen: int):
        if gen != self._scan_gen: return GLib.SOURCE_REMOVE
        self._categories.append(cat)
        self._scan_page.set_found(len(self._categories), running_total)
        return GLib.SOURCE_REMOVE

    def _scan_done(self, gen: int):
        if gen != self._scan_gen: return GLib.SOURCE_REMOVE
        self._scanning = False
        self._scan_btn.set_sensitive(True)

        total = sum(c.total_size for c in self._categories)
        self._scan_page.set_done(len(self._categories), total)

        entry = HistoryEntry(
            id=str(time.time()), date=datetime.datetime.now().isoformat(),
            mode=self._scan_mode, categories_found=len(self._categories),
            total_found=total, total_deleted=0, deleted_count=0,
        )
        add_history_entry(entry)
        self._last_history_id = entry.id

        GLib.timeout_add(900, self._show_results, gen)
        return GLib.SOURCE_REMOVE

    def _show_results(self, gen: int = -1):
        if gen != -1 and gen != self._scan_gen: return GLib.SOURCE_REMOVE
        self._post_delete = False

        if not self._categories:
            self._stack.set_visible_child_name("scanning")
            self._status_label.set_label("Filesystem uniform cleanly structure verified")
            self._scan_page._title.set_label("Verified clean")
            self._scan_page._path_label.set_label("No targeted elements observed inside ruleset criteria")
            return GLib.SOURCE_REMOVE

        total = sum(c.total_size for c in self._categories)
        total_items = sum(len(c.entries) for c in self._categories)
        self._status_label.set_label(f"Review matrix: {fmt_size(total)} across {total_items} points")

        child = self._list_box_outer.get_first_child()
        while child:
            nxt = child.get_next_sibling()
            self._list_box_outer.remove(child)
            child = nxt

        summary = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        summary.add_css_class("summary-bar")
        summary_label = Gtk.Label()
        summary_label.set_markup(f"<b>{fmt_size(total)}</b> indexed across <b>{len(self._categories)}</b> areas")
        summary_label.set_css_classes(["summary-total"])
        summary_label.set_halign(Gtk.Align.START)
        summary_label.set_hexpand(True)
        
        none_btn = Gtk.Button(label="Deselect all")
        none_btn.set_css_classes(["flat"])
        none_btn.connect("clicked", self._on_deselect_all)
        all_btn = Gtk.Button(label="Select all")
        all_btn.set_css_classes(["flat"])
        all_btn.connect("clicked", self._on_select_all)
        
        summary.append(summary_label)
        summary.append(none_btn)
        summary.append(all_btn)
        self._list_box_outer.append(summary)

        revealers = []
        for cat in self._categories:
            rev = self._add_category_group(cat, animate=True)
            revealers.append(rev)

        self._stack.set_visible_child_name("results")
        for i, rev in enumerate(revealers):
            GLib.timeout_add(i * 80, self._reveal_group, rev)

        self._refresh_delete_btn()
        return GLib.SOURCE_REMOVE

    def _on_select_all(self, *_):
        for cat in self._categories:
            for entry in cat.entries: entry.selected = True
        self._refresh_delete_btn()

    def _on_deselect_all(self, *_):
        for cat in self._categories:
            for entry in cat.entries: entry.selected = False
        self._refresh_delete_btn()

    def _on_scroll_changed(self, adj):
        scroll_y = adj.get_value()
        viewport_h = adj.get_page_size()
        child = self._list_box_outer.get_first_child()
        while child:
            alloc = child.get_allocation()
            child_top, child_bottom = alloc.y, alloc.y + alloc.height
            fade_zone = 48
            if child_bottom < scroll_y - fade_zone or child_top > scroll_y + viewport_h + fade_zone:
                child.set_opacity(0.0)
            elif child_bottom < scroll_y + fade_zone:
                child.set_opacity(max(0.12, (child_bottom - scroll_y) / max(1, fade_zone)))
            elif child_top > scroll_y + viewport_h - fade_zone:
                child.set_opacity(max(0.12, (scroll_y + viewport_h - child_top) / max(1, fade_zone)))
            else:
                child.set_opacity(1.0)
            child = child.get_next_sibling()

    def _reveal_group(self, revealer):
        revealer.set_reveal_child(True)
        return GLib.SOURCE_REMOVE

    def _add_category_group(self, cat: Category, animate: bool = True):
        group = Adw.PreferencesGroup()
        group.set_title(cat.title)
        group.set_description(f"{cat.subtitle}  ·  {fmt_size(cat.total_size)}")

        header_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        select_all_btn = Gtk.Button(label="All")
        select_all_btn.set_css_classes(["flat", "caption"])
        select_all_btn.connect("clicked", lambda _, c=cat: self._select_all_in_cat(c, True))
        select_none_btn = Gtk.Button(label="None")
        select_none_btn.set_css_classes(["flat", "caption"])
        select_none_btn.connect("clicked", lambda _, c=cat: self._select_all_in_cat(c, False))
        header_box.append(select_all_btn)
        header_box.append(select_none_btn)
        group.set_header_suffix(header_box)

        for entry in cat.entries:
            row = FileRow(entry, on_toggle=self._on_file_toggle)
            group.add(row)

        revealer = Gtk.Revealer()
        revealer.set_transition_type(Gtk.RevealerTransitionType.SLIDE_DOWN)
        revealer.set_transition_duration(280)
        revealer.set_reveal_child(not animate)
        revealer.set_child(group)
        self._list_box_outer.append(revealer)
        return revealer

    def _select_all_in_cat(self, cat: Category, selected: bool):
        for entry in cat.entries: entry.selected = selected
        self._refresh_delete_btn()

    def _on_file_toggle(self, entry: FileEntry, active: bool):
        entry.selected = active
        self._refresh_delete_btn()

    def _refresh_delete_btn(self):
        total = sum(e.size for c in self._categories for e in c.entries if e.selected)
        if total > 0:
            self._delete_btn.set_label(f"Clear {fmt_size(total)}")
            self._delete_btn.set_sensitive(True)
        else:
            self._delete_btn.set_label("Clear targets")
            self._delete_btn.set_sensitive(False)

    def _on_delete(self, *_):
        selected = [e for c in self._categories for e in c.entries if e.selected]
        if not selected: return
        total = sum(e.size for e in selected)

        dialog = Adw.MessageDialog(
            transient_for=self,
            heading="Execute permanent removal?",
            body=f"Selected structures tracking {fmt_size(total)} will be unlinked permanently."
        )
        dialog.add_response("cancel", "Cancel")
        dialog.add_response("delete", f"Drop {fmt_size(total)}")
        dialog.set_response_appearance("delete", Adw.ResponseAppearance.DESTRUCTIVE)
        
        def _handle_delete_response(_, response):
            if response == "delete":
                self._delete_btn.set_sensitive(False)
                self._scan_btn.set_sensitive(False)
                self._status_label.set_label("Modifying file allocations…")
                threading.Thread(target=self._delete_worker, args=(selected,), daemon=True).start()
            dialog.destroy()
                
        dialog.connect("response", _handle_delete_response)
        dialog.present()

    def _on_select_all(self, *_):
        for cat in self._categories:
            for entry in cat.entries: entry.selected = True
        self._refresh_delete_btn()

    def _on_deselect_all(self, *_):
        for cat in self._categories:
            for entry in cat.entries: entry.selected = False
        self._refresh_delete_btn()

    def _on_scroll_changed(self, adj):
        scroll_y = adj.get_value()
        viewport_h = adj.get_page_size()
        child = self._list_box_outer.get_first_child()
        while child:
            alloc = child.get_allocation()
            child_top, child_bottom = alloc.y, alloc.y + alloc.height
            fade_zone = 48
            if child_bottom < scroll_y - fade_zone or child_top > scroll_y + viewport_h + fade_zone:
                child.set_opacity(0.0)
            elif child_bottom < scroll_y + fade_zone:
                child.set_opacity(max(0.12, (child_bottom - scroll_y) / max(1, fade_zone)))
            elif child_top > scroll_y + viewport_h - fade_zone:
                child.set_opacity(max(0.12, (scroll_y + viewport_h - child_top) / max(1, fade_zone)))
            else:
                child.set_opacity(1.0)
            child = child.get_next_sibling()

    def _reveal_group(self, revealer):
        revealer.set_reveal_child(True)
        return GLib.SOURCE_REMOVE

    def _add_category_group(self, cat: Category, animate: bool = True):
        group = Adw.PreferencesGroup()
        group.set_title(cat.title)
        group.set_description(f"{cat.subtitle}  ·  {fmt_size(cat.total_size)}")

        header_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        select_all_btn = Gtk.Button(label="All")
        select_all_btn.set_css_classes(["flat", "caption"])
        select_all_btn.connect("clicked", lambda _, c=cat: self._select_all_in_cat(c, True))
        select_none_btn = Gtk.Button(label="None")
        select_none_btn.set_css_classes(["flat", "caption"])
        select_none_btn.connect("clicked", lambda _, c=cat: self._select_all_in_cat(c, False))
        header_box.append(select_all_btn)
        header_box.append(select_none_btn)
        group.set_header_suffix(header_box)

        for entry in cat.entries:
            row = FileRow(entry, on_toggle=self._on_file_toggle)
            group.add(row)

        revealer = Gtk.Revealer()
        revealer.set_transition_type(Gtk.RevealerTransitionType.SLIDE_DOWN)
        revealer.set_transition_duration(280)
        revealer.set_reveal_child(not animate)
        revealer.set_child(group)
        self._list_box_outer.append(revealer)
        return revealer

    def _select_all_in_cat(self, cat: Category, selected: bool):
        for entry in cat.entries: entry.selected = selected
        self._refresh_delete_btn()

    def _on_file_toggle(self, entry: FileEntry, active: bool):
        entry.selected = active
        self._refresh_delete_btn()

    def _refresh_delete_btn(self):
        total = sum(e.size for c in self._categories for e in c.entries if e.selected)
        if total > 0:
            self._delete_btn.set_label(f"Clear {fmt_size(total)}")
            self._delete_btn.set_sensitive(True)
        else:
            self._delete_btn.set_label("Clear targets")
            self._delete_btn.set_sensitive(False)

    def _on_delete(self, *_):
        selected = [e for c in self._categories for e in c.entries if e.selected]
        if not selected: return
        total = sum(e.size for e in selected)

        dialog = Adw.MessageDialog(
            transient_for=self,
            heading="Execute permanent removal?",
            body=f"Selected structures tracking {fmt_size(total)} will be unlinked permanently."
        )
        dialog.add_response("cancel", "Cancel")
        dialog.add_response("delete", f"Drop {fmt_size(total)}")
        dialog.set_response_appearance("delete", Adw.ResponseAppearance.DESTRUCTIVE)
        
        def _handle_delete_response(_, response):
            if response == "delete":
                self._delete_btn.set_sensitive(False)
                self._scan_btn.set_sensitive(False)
                self._status_label.set_label("Modifying file allocations…")
                threading.Thread(target=self._delete_worker, args=(selected,), daemon=True).start()
            dialog.destroy()
                
        dialog.connect("response", _handle_delete_response)
        dialog.present()

    def _delete_worker(self, entries: List[FileEntry]):
        paths = [e.path for e in entries]
        if os.geteuid() == 0:
            deleted_count, errors = delete_direct(paths)
            if deleted_count is None: deleted_count = 0
        else:
            deleted_count, errors = delete_direct(paths)
            if deleted_count is None:
                deleted_count, errors = delete_with_pkexec(paths)
        GLib.idle_add(self._delete_done, deleted_count or 0, errors)

    def _delete_done(self, deleted_count: int, errors: list):
        self._scan_btn.set_sensitive(True)
        self._delete_btn.set_sensitive(True)

        if errors:
            dlg = Adw.MessageDialog(
                transient_for=self,
                heading="Write access faults encountered",
                body="\n".join(errors[:8])
            )
            dlg.add_response("ok", "OK")
            dlg.connect("response", lambda d, _: d.destroy())
            dlg.present()

        if deleted_count == 0 and not errors:
            self._status_label.set_label("Modification process dropped")
            return GLib.SOURCE_REMOVE

        self._status_label.set_label(f"Successfully dropped {deleted_count} system tracking items")

        if deleted_count > 0 and hasattr(self, "_last_history_id"):
            entries = load_history()
            for e in entries:
                if e.id == self._last_history_id:
                    e.total_deleted = sum(entry.size for cat in self._categories for entry in cat.entries if entry.selected)
                    e.deleted_count = deleted_count
                    break
            save_history(entries)

        if deleted_count > 0 and self._settings.auto_rescan:
            self._post_delete = True
            self._start_scan(mode=self._scan_mode, root=self._scan_root)
        return GLib.SOURCE_REMOVE


# ---------------------------------------------------------------------------
# Application Initialization
# ---------------------------------------------------------------------------

class FileCleanerApp(Adw.Application):
    def __init__(self):
        super().__init__(application_id="io.github.filecleaner", flags=Gio.ApplicationFlags.DEFAULT_FLAGS)
        self.connect("activate", self._on_activate)

    def _on_activate(self, *_):
        apply_color_scheme()
        provider = Gtk.CssProvider()
        provider.load_from_data(CSS)
        Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

        settings = Settings.load()
        win = FileCleanerWindow(self, settings)
        win.present()


if __name__ == "__main__":
    import sys
    app = FileCleanerApp()
    sys.exit(app.run(sys.argv))
