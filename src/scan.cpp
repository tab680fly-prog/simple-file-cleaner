#include "scan.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fc {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool ends_with(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(const std::string &s, const std::string &prefix) { return s.rfind(prefix, 0) == 0; }

bool path_exists(const fs::path &p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

std::vector<fs::path> list_dir(const fs::path &d) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec))
        out.push_back(it->path());
    return out;
}

std::uint64_t file_size_or_zero(const fs::path &p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return 0;
    auto sz = fs::file_size(p, ec);
    return ec ? 0 : sz;
}

// A single-location category (e.g. "pip cache" -> ~/.cache/pip).
struct DirTarget {
    std::string setting;  // SCAN_CATEGORIES key that enables it
    std::string key, title, subtitle, icon;
    fs::path path;
    std::uint64_t min_size = 1;
};

void emit_dir_targets(const std::vector<DirTarget> &targets, const Settings &settings, const ProgressCb &progress,
                      const CategoryCb &emit, CancelToken &cancel) {
    std::vector<fs::path> to_size;
    for (const auto &t : targets)
        if (settings.cat_on(t.setting) && path_exists(t.path) && !settings.path_excluded(t.path)) to_size.push_back(t.path);
    if (to_size.empty()) return;
    if (progress) progress("Measuring common cache locations…");
    auto sizes = dir_size_many(to_size, progress, cancel);
    for (const auto &t : targets) {
        if (cancel.is_cancelled()) return;
        auto it = sizes.find(t.path);
        if (it == sizes.end() || it->second < t.min_size) continue;
        emit({t.key, t.title, t.subtitle.empty() ? display_path(t.path) : t.subtitle, t.icon, {{t.path, it->second, true}}});
    }
}

// Emits one category holding each immediate child of `dirs` (e.g. the items
// in the Trash), optionally filtered by `keep`.
void emit_children(const std::string &key, const std::string &title, const std::string &subtitle,
                   const std::string &icon, const std::vector<fs::path> &dirs, const Settings &settings,
                   const ProgressCb &progress, const CategoryCb &emit, CancelToken &cancel,
                   const std::function<bool(const fs::path &)> &keep = nullptr, std::uint64_t min_size = 1) {
    std::vector<fs::path> items, subdirs;
    for (const auto &d : dirs) {
        if (!path_exists(d) || settings.path_excluded(d)) continue;
        for (auto &item : list_dir(d)) {
            if (cancel.is_cancelled()) return;
            if (settings.path_excluded(item) || is_link(item)) continue;
            if (keep && !keep(item)) continue;
            items.push_back(item);
            std::error_code ec;
            if (fs::is_directory(item, ec)) subdirs.push_back(item);
        }
    }
    auto dir_sizes = dir_size_many(subdirs, progress, cancel);
    std::vector<FileEntry> entries;
    for (auto &item : items) {
        auto it = dir_sizes.find(item);
        std::uint64_t sz = it != dir_sizes.end() ? it->second : file_size_or_zero(item);
        if (sz >= min_size) entries.push_back({item, sz, true});
    }
    if (!entries.empty()) emit({key, title, subtitle, icon, std::move(entries)});
}

bool older_than(const fs::path &p, std::chrono::hours age) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return false;
    return t < fs::file_time_type::clock::now() - age;
}

}  // namespace

std::uint64_t dir_size(const fs::path &p, const ProgressCb &progress, CancelToken &cancel) {
    if (cancel.is_cancelled()) return 0;
    if (progress) progress(path_str(p));

    std::uint64_t total = 0;
    std::error_code ec;
    fs::directory_iterator it(p, fs::directory_options::skip_permission_denied, ec);
    fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        if (cancel.is_cancelled()) break;
        if (is_link(*it)) continue;
        std::error_code sec;
        auto st = it->status(sec);
        if (sec) continue;
        if (fs::is_regular_file(st)) {
            std::error_code fec;
            auto sz = fs::file_size(it->path(), fec);
            if (!fec) total += sz;
        } else if (fs::is_directory(st)) {
            total += dir_size(it->path(), progress, cancel);
        }
    }
    return total;
}

std::map<fs::path, std::uint64_t> dir_size_many(const std::vector<fs::path> &paths,
                                                 const ProgressCb &progress, CancelToken &cancel) {
    std::map<fs::path, std::uint64_t> result;
    std::vector<fs::path> existing;
    for (const auto &p : paths)
        if (path_exists(p)) existing.push_back(p);
    if (existing.empty()) return result;
    if (progress) progress(path_str(existing.front()));

    std::mutex result_mutex;
    std::atomic<std::size_t> idx{0};
    unsigned n_threads = std::min<unsigned>(
        {std::max<unsigned>(1, std::thread::hardware_concurrency()), (unsigned)existing.size(), 8u});

    auto worker = [&]() {
        for (;;) {
            std::size_t i = idx.fetch_add(1);
            if (i >= existing.size() || cancel.is_cancelled()) return;
            const auto &path = existing[i];
            std::error_code ec;
            std::uint64_t sz = 0;
            if (is_link(path)) {
                sz = 0;
            } else if (fs::is_directory(path, ec)) {
                sz = dir_size(path, progress, cancel);
            } else {
                sz = file_size_or_zero(path);
            }
            std::lock_guard<std::mutex> lk(result_mutex);
            result[path] = sz;
        }
    };

    std::vector<std::thread> workers;
    for (unsigned t = 0; t < n_threads; ++t) workers.emplace_back(worker);
    for (auto &w : workers) w.join();
    return result;
}

// ---------------------------------------------------------------------------
// Quick scan
// ---------------------------------------------------------------------------

namespace {

void scan_custom_paths(const Settings &settings, const ProgressCb &progress, const CategoryCb &emit,
                       CancelToken &cancel) {
    if (settings.custom_scan_paths.empty()) return;
    if (progress) progress("Examining custom scan locations…");
    std::vector<fs::path> custom_paths;
    for (const auto &s : settings.custom_scan_paths) {
        fs::path p = path_from(expand_user_path(s));
        if (!is_protected_path(p) && path_exists(p) && !settings.path_excluded(p)) custom_paths.push_back(p);
    }
    if (custom_paths.empty()) return;
    auto sizes = dir_size_many(custom_paths, progress, cancel);
    Category cat{"custom_paths", "Custom Locations", "Locations added in Preferences", "folder-saved-search-symbolic",
                 {}};
    for (const auto &cp : custom_paths) {
        auto it = sizes.find(cp);
        if (it != sizes.end() && it->second) cat.entries.push_back({cp, it->second, true});
    }
    if (!cat.entries.empty()) emit(std::move(cat));
}

void scan_downloads(const Settings &settings, const ProgressCb &progress, const CategoryCb &emit,
                    CancelToken &cancel) {
    if (!settings.cat_on("downloads")) return;
    const fs::path downloads = downloads_dir();
    const auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(30 * 24);
    if (progress) progress("Examining items in Downloads…");
    emit_children("downloads", "Large, unused downloads",
                  "Items in " + display_path(downloads) + " over 50 MB that have not been opened in 30 days",
                  "folder-download-symbolic", {downloads}, settings, progress, emit, cancel,
                  [&](const fs::path &item) {
                      auto atime = last_access_time(item);
                      return atime && *atime < cutoff;
                  },
                  50ull << 20);
}

#ifdef _WIN32

void scan_platform(const Settings &settings, const ProgressCb &progress, const CategoryCb &emit,
                   CancelToken &cancel) {
    const fs::path local = env_path("LOCALAPPDATA").empty() ? home_dir() / "AppData" / "Local" : env_path("LOCALAPPDATA");

    if (settings.cat_on("trash")) {
        if (progress) progress("Examining the Recycle Bin…");
        // Each deleted item is stored as "$R<id>" plus a "$I<id>" metadata
        // file. Only $R items are listed; their $I file is removed with them
        // so Explorer is not left showing broken entries.
        std::vector<Category> found;
        emit_children("trash", "Recycle Bin", "Deleted items on all local drives", "user-trash-symbolic", trash_dirs(),
                      settings, progress, [&](Category &&c) { found.push_back(std::move(c)); }, cancel,
                      [](const fs::path &item) { return starts_with(path_str(item.filename()), "$R"); });
        for (auto &cat : found) {
            for (auto &e : cat.entries) {
                std::string name = path_str(e.path.filename());
                fs::path meta = e.path.parent_path() / path_from("$I" + name.substr(2));
                if (path_exists(meta)) e.companions.push_back(meta);
            }
            emit(std::move(cat));
        }
    }
    if (cancel.is_cancelled()) return;

    if (settings.cat_on("temp")) {
        fs::path temp = env_path("TEMP");
        if (temp.empty()) temp = local / "Temp";
        if (progress) progress("Examining temporary files…");
        emit_children("temp", "Temporary files", display_path(temp) + " — items unused for 7 days or more",
                      "folder-symbolic", {temp}, settings, progress, emit, cancel,
                      [](const fs::path &item) { return older_than(item, std::chrono::hours(7 * 24)); });
    }
    if (cancel.is_cancelled()) return;

    if (settings.cat_on("thumbs")) {
        const fs::path explorer = local / "Microsoft" / "Windows" / "Explorer";
        emit_children("thumbs", "Thumbnail cache", "Windows Explorer thumbnail databases", "image-x-generic-symbolic",
                      {explorer}, settings, progress, emit, cancel, [](const fs::path &item) {
                          const std::string n = lower(path_str(item.filename()));
                          return starts_with(n, "thumbcache_") && ends_with(n, ".db");
                      });
    }
    if (cancel.is_cancelled()) return;

    std::vector<DirTarget> targets;

    // Chromium-based browsers keep one cache per profile ("Default",
    // "Profile 1", ...).
    struct Browser {
        const char *name;
        fs::path user_data;
    };
    const Browser chromium_browsers[] = {
        {"Google Chrome", local / "Google" / "Chrome" / "User Data"},
        {"Microsoft Edge", local / "Microsoft" / "Edge" / "User Data"},
        {"Brave", local / "BraveSoftware" / "Brave-Browser" / "User Data"},
        {"Chromium", local / "Chromium" / "User Data"},
    };
    for (const auto &b : chromium_browsers) {
        if (!path_exists(b.user_data)) continue;
        for (const auto &profile : list_dir(b.user_data)) {
            const std::string pn = path_str(profile.filename());
            if (pn != "Default" && !starts_with(pn, "Profile ")) continue;
            for (const char *sub : {"Cache", "Code Cache"}) {
                std::string title = std::string(b.name) + " " + (std::string(sub) == "Cache" ? "cache" : "code cache");
                if (pn != "Default") title += " (" + pn + ")";
                targets.push_back({"browser_cache", "browser_" + lower(title), title, "", "web-browser-symbolic",
                                   profile / sub});
            }
        }
    }
    const fs::path ff_profiles = local / "Mozilla" / "Firefox" / "Profiles";
    for (const auto &profile : list_dir(ff_profiles))
        targets.push_back({"browser_cache", "browser_firefox_" + path_str(profile.filename()), "Firefox cache", "",
                           "web-browser-symbolic", profile / "cache2"});

    targets.push_back({"pkg_cache", "pip", "pip cache", "", "package-x-generic-symbolic", local / "pip" / "Cache"});
    targets.push_back({"pkg_cache", "npm", "npm cache", "", "package-x-generic-symbolic", local / "npm-cache"});
    targets.push_back({"pkg_cache", "yarn", "Yarn cache", "", "package-x-generic-symbolic", local / "Yarn" / "Cache"});
    targets.push_back(
        {"pkg_cache", "nuget", "NuGet HTTP cache", "", "package-x-generic-symbolic", local / "NuGet" / "v3-cache"});

    targets.push_back({"logs", "crash_dumps", "Crash dumps", "", "text-x-generic-symbolic", local / "CrashDumps"});
    targets.push_back({"logs", "wer_archive", "Archived error reports", "", "text-x-generic-symbolic",
                       local / "Microsoft" / "Windows" / "WER" / "ReportArchive"});
    targets.push_back({"logs", "wer_queue", "Queued error reports", "", "text-x-generic-symbolic",
                       local / "Microsoft" / "Windows" / "WER" / "ReportQueue"});

    emit_dir_targets(targets, settings, progress, emit, cancel);
}

#else

void scan_platform(const Settings &settings, const ProgressCb &progress, const CategoryCb &emit,
                   CancelToken &cancel) {
    const fs::path home = home_dir();

    if (settings.cat_on("trash")) {
        if (progress) progress("~/.local/share/Trash");
        emit_children("trash", "Trash", "~/.local/share/Trash", "user-trash-symbolic", trash_dirs(), settings, progress,
                      emit, cancel);
    }
    if (cancel.is_cancelled()) return;

    std::vector<DirTarget> targets = {
        {"thumbs", "thumbs", "Thumbnail cache", "", "image-x-generic-symbolic", home / ".cache/thumbnails"},
        {"browser_cache", "browser_Firefox cache", "Firefox cache", "", "web-browser-symbolic",
         home / ".cache/mozilla/firefox"},
        {"browser_cache", "browser_Chromium cache", "Chromium cache", "", "web-browser-symbolic", home / ".cache/chromium"},
        {"browser_cache", "browser_Chrome cache", "Chrome cache", "", "web-browser-symbolic",
         home / ".config/google-chrome/Default/Cache"},
        {"pkg_cache", "yay", "yay package cache", "", "package-x-generic-symbolic", home / ".cache/yay"},
        {"pkg_cache", "paru", "paru package cache", "", "package-x-generic-symbolic", home / ".cache/paru"},
        {"pkg_cache", "pip", "pip cache", "", "package-x-generic-symbolic", home / ".cache/pip"},
    };
    emit_dir_targets(targets, settings, progress, emit, cancel);
    if (cancel.is_cancelled()) return;

    // Only per-application cache folders are offered. The user Flatpak
    // repository (~/.local/share/flatpak/repo) is deliberately not: it is
    // not a cache, and deleting it corrupts every user-installed Flatpak
    // application. Use `flatpak uninstall --unused` instead.
    if (settings.cat_on("flatpak")) {
        std::vector<fs::path> app_caches;
        for (const auto &d : list_dir(home / ".var/app"))
            if (path_exists(d / "cache")) app_caches.push_back(d / "cache");
        auto cache_sizes = dir_size_many(app_caches, progress, cancel);
        std::vector<FileEntry> entries;
        for (auto &[path, sz] : cache_sizes)
            if (sz > (5u << 20) && !settings.path_excluded(path)) entries.push_back({path, sz, true});
        if (!entries.empty())
            emit({"flatpak_cache", "Flatpak application caches", "~/.var/app/*/cache", "package-x-generic-symbolic",
                  std::move(entries)});
    }
    if (cancel.is_cancelled()) return;

    if (settings.cat_on("logs")) {
        std::vector<FileEntry> log_entries;
        for (const auto &e : list_dir(home / ".local/share/xorg")) {
            std::string name = path_str(e.filename());
            bool matches = starts_with(name, "Xorg.") && (ends_with(name, ".old") || ends_with(name, ".log.old"));
            if (!matches || settings.path_excluded(e) || !is_writable(e)) continue;
            if (auto sz = file_size_or_zero(e)) log_entries.push_back({e, sz, true});
        }
        const std::vector<fs::path> journal_paths = {
            fs::path("/run/user") / std::to_string(getuid()) / "systemd/journal",
            home / ".local/share/systemd",
        };
        for (const auto &jpath : journal_paths) {
            if (path_exists(jpath) && !settings.path_excluded(jpath) && is_writable(jpath)) {
                if (auto sz = dir_size(jpath, progress, cancel)) log_entries.push_back({jpath, sz, true});
            }
        }
        if (!log_entries.empty())
            emit({"logs", "Logs", "Old Xorg logs and user journal files", "text-x-generic-symbolic",
                  std::move(log_entries)});
    }
}

#endif

}  // namespace

void scan_generator(const Settings &settings, const ProgressCb &progress, const CategoryCb &emit,
                     CancelToken &cancel) {
    scan_custom_paths(settings, progress, emit, cancel);
    if (cancel.is_cancelled()) return;
    scan_platform(settings, progress, emit, cancel);
    if (cancel.is_cancelled()) return;
    scan_downloads(settings, progress, emit, cancel);
}

// ---------------------------------------------------------------------------
// Deep scan
// ---------------------------------------------------------------------------

namespace {

const std::set<std::string> DEEP_JUNK_DIRS = {
    "node_modules", "__pycache__", ".mypy_cache", ".pytest_cache",
    ".ruff_cache",  "dist",        "build",       ".gradle",
    ".m2",          "target",      ".dart_tool",  ".pub-cache",
};

const std::set<std::string> DEEP_SKIP_DIRS = {
    ".git",  ".hg",     ".svn",   ".var",   ".steam", "steam",   "Steam",  ".cargo",
    ".rustup", ".nvm",  ".rbenv", ".rvm",   ".sdkman", ".java",  "go",     "snap",
    "Games", "games",   "Music",  "Videos", "Pictures", "proc",  "sys",    "dev",    "run",
    // Application data and package stores. These routinely contain folders
    // named "build", "dist" or "target" that are part of installed software
    // (e.g. ~/.vscode/extensions/*/dist, ~/.nuget/packages/*/build), not
    // disposable build output. Their real caches are covered by quick scan.
    ".local", ".config", ".mozilla", ".thunderbird", ".wine", ".proton",
    ".nuget", ".dotnet", ".vscode", ".vscode-oss", ".cursor", ".npm",
    // Windows: per-user application data (its caches are covered by quick
    // scan) and the legacy compatibility junctions in the profile folder.
    "AppData", "Application Data", "Local Settings", "$Recycle.Bin",
};

struct JunkMeta {
    std::string title, subtitle, icon;
};

const std::map<std::string, JunkMeta> DEEP_JUNK_META = {
    {"node_modules", {"node_modules folders", "Installed JavaScript dependencies", "code-symbolic"}},
    {"__pycache__",
     {"Python bytecode caches", "__pycache__ folders", "application-x-python-bytecode-symbolic"}},
    {".mypy_cache", {"mypy caches", "Python type checker caches", "text-x-generic-symbolic"}},
    {".pytest_cache", {"pytest caches", "Python test runner caches", "text-x-generic-symbolic"}},
    {".ruff_cache", {"Ruff caches", "Python linter caches", "text-x-generic-symbolic"}},
    {"dist", {"dist folders", "Packaged distribution output", "package-x-generic-symbolic"}},
    {"build", {"build folders", "Compiled build output", "package-x-generic-symbolic"}},
    {".gradle", {"Gradle caches", ".gradle folders", "package-x-generic-symbolic"}},
    {".m2", {"Maven repositories", "Local .m2 dependency caches", "package-x-generic-symbolic"}},
    {"target", {"target folders", "Rust and Maven build output", "package-x-generic-symbolic"}},
    {".dart_tool", {"Dart tool folders", ".dart_tool build metadata", "package-x-generic-symbolic"}},
    {".pub-cache",
     {"Dart package caches", "Downloaded Dart and Flutter packages", "package-x-generic-symbolic"}},
    {"stale_bytecode",
     {"Compiled bytecode files", "Individual .pyc, .pyo and .class files", "text-x-generic-symbolic"}},
};

// Only interpreter bytecode is matched. Native .o and .a files used to be
// included, but toolchains and SDKs ship them as real, non-regenerable
// files (e.g. a compiler's crt*.o or an SDK's static libraries), and
// deleting them breaks those installations.
bool is_stale_bytecode(const std::string &filename) {
    static const std::set<std::string> exact = {".pyc", ".pyo", ".class"};
    fs::path p = path_from(filename);
    return exact.count(lower(path_str(p.extension()))) > 0;
}

std::set<std::string> enabled_deep_junk_dirs(const Settings &settings) {
    std::set<std::string> enabled;
    for (const auto &[cat_key, dir_names] : DEEP_JUNK_CATEGORY_MAP) {
        if (settings.cat_on(cat_key)) enabled.insert(dir_names.begin(), dir_names.end());
    }
    return enabled;
}

using Buckets = std::map<std::string, std::vector<fs::path>>;

void deep_walk_recurse(const fs::path &p, int depth, const Settings &settings,
                        const std::set<std::string> &enabled_dirs, const ProgressCb &progress,
                        CancelToken &cancel, Buckets &buckets) {
    if (depth > 10 || cancel.is_cancelled()) return;

    std::error_code ec;
    std::vector<fs::directory_entry> entries;
    for (auto &e : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        entries.push_back(e);
    }

    std::vector<std::pair<std::string, fs::path>> junk_found;
    std::vector<fs::path> recurse_into;

    for (auto &e : entries) {
        std::string name = path_str(e.path().filename());
        if (DEEP_SKIP_DIRS.count(name) || settings.path_excluded(e.path())) continue;
        if (is_link(e)) continue;

        std::error_code sec;
        auto st = e.status(sec);
        if (sec) continue;

        if (fs::is_directory(st)) {
            if (enabled_dirs.count(name))
                junk_found.emplace_back(name, e.path());
            else
                recurse_into.push_back(e.path());
        } else if (fs::is_regular_file(st)) {
            if (settings.cat_on("stale_bytecode") && is_stale_bytecode(name) &&
                !settings.path_excluded(e.path())) {
                buckets["stale_bytecode"].push_back(e.path());
            }
        }
    }

    if (!junk_found.empty()) {
        if (progress)
            progress("Measuring " + std::to_string(junk_found.size()) + " items in " +
                      path_str(p.filename()) + "/…");
        std::vector<fs::path> paths;
        for (auto &[key, pth] : junk_found) paths.push_back(pth);
        auto sizes = dir_size_many(paths, progress, cancel);
        for (auto &[key, pth] : junk_found) {
            auto sz = sizes.count(pth) ? sizes.at(pth) : 0;
            if (sz) buckets[key].push_back(pth);
        }
    }

    for (auto &sub : recurse_into) {
        if (cancel.is_cancelled()) return;
        if (progress) progress(path_str(sub));
        deep_walk_recurse(sub, depth + 1, settings, enabled_dirs, progress, cancel, buckets);
    }
}

Buckets deep_walk(const fs::path &root, const Settings &settings, const ProgressCb &progress,
                   CancelToken &cancel) {
    auto enabled_dirs = enabled_deep_junk_dirs(settings);
    Buckets buckets;
    for (auto &name : DEEP_JUNK_DIRS) buckets[name] = {};
    buckets["stale_bytecode"] = {};
    deep_walk_recurse(root, 0, settings, enabled_dirs, progress, cancel, buckets);
    return buckets;
}

void emit_deep_buckets(Buckets &buckets, const Settings &settings, const ProgressCb &progress,
                        const CategoryCb &emit, CancelToken &cancel, const std::string &prefix) {
    for (auto &[key, paths] : buckets) {
        if (paths.empty() || cancel.is_cancelled()) continue;
        auto meta_it = DEEP_JUNK_META.find(key);
        std::string title = meta_it != DEEP_JUNK_META.end() ? meta_it->second.title : key;
        std::string subtitle = meta_it != DEEP_JUNK_META.end() ? meta_it->second.subtitle : key;
        std::string icon = meta_it != DEEP_JUNK_META.end() ? meta_it->second.icon : "folder-symbolic";

        std::vector<FileEntry> entries;
        if (key == "stale_bytecode") {
            if (!settings.cat_on("stale_bytecode")) continue;
            for (auto &p : paths) {
                std::error_code ec;
                auto sz = fs::file_size(p, ec);
                if (!ec) entries.push_back({p, sz, true});
            }
        } else {
            auto sizes = dir_size_many(paths, progress, cancel);
            for (auto &p : paths) {
                auto sz = sizes.count(p) ? sizes.at(p) : 0;
                if (sz) entries.push_back({p, sz, true});
            }
        }
        if (!entries.empty()) emit({prefix + "_" + key, title, subtitle, icon, std::move(entries)});
    }
}

}  // namespace

void deep_scan_generator(const Settings &settings, const ProgressCb &progress, const CategoryCb &emit,
                          CancelToken &cancel) {
    fs::path home = home_dir();
    if (progress) progress("Examining standard locations…");
    scan_generator(settings, progress, emit, cancel);
    if (cancel.is_cancelled()) return;

    if (progress) progress("Searching the home folder for development files…");
    auto buckets = deep_walk(home, settings, progress, cancel);
    if (cancel.is_cancelled()) return;
    emit_deep_buckets(buckets, settings, progress, emit, cancel, "deep");
}

void folder_scan_generator(const fs::path &root, const Settings &settings, const ProgressCb &progress,
                            const CategoryCb &emit, CancelToken &cancel) {
    if (progress) progress("Examining " + display_path(root) + "…");
    auto buckets = deep_walk(root, settings, progress, cancel);
    if (cancel.is_cancelled()) return;

    std::vector<FileEntry> large_entries;
    std::error_code ec;
    for (auto &item : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (is_link(item)) continue;
        std::error_code sec;
        auto st = item.status(sec);
        if (sec || !fs::is_regular_file(st)) continue;
        if (settings.path_excluded(item.path())) continue;
        std::error_code fec;
        auto sz = fs::file_size(item.path(), fec);
        if (!fec && sz >= (50u << 20)) large_entries.push_back({item.path(), sz, true});
    }
    if (!large_entries.empty())
        emit({"folder_large", "Large files", "Files of 50 MB or more directly inside " + path_str(root.filename()),
              "folder-download-symbolic", std::move(large_entries)});
    if (cancel.is_cancelled()) return;

    emit_deep_buckets(buckets, settings, progress, emit, cancel, "folder");
}

}  // namespace fc
