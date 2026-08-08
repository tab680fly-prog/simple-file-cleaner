#include "scan.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace fc {

namespace {

fs::path home_dir() {
    if (const char *h = std::getenv("HOME")) return fs::path(h);
    return fs::path("/");
}

bool is_writable(const fs::path &p) { return ::access(p.c_str(), W_OK) == 0; }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool ends_with(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

std::uint64_t dir_size(const fs::path &p, const ProgressCb &progress, CancelToken &cancel) {
    if (cancel.is_cancelled()) return 0;
    if (progress) progress(p.string());

    std::uint64_t total = 0;
    std::error_code ec;
    fs::directory_iterator it(p, fs::directory_options::skip_permission_denied, ec);
    fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        if (cancel.is_cancelled()) break;
        std::error_code sec;
        auto st = it->symlink_status(sec);
        if (sec || fs::is_symlink(st)) continue;
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
    for (const auto &p : paths) {
        std::error_code ec;
        if (fs::exists(p, ec)) existing.push_back(p);
    }
    if (existing.empty()) return result;
    if (progress) progress(existing.front().string());

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
            if (fs::is_directory(path, ec)) {
                sz = dir_size(path, progress, cancel);
            } else {
                sz = fs::file_size(path, ec);
                if (ec) sz = 0;
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

void scan_generator(const Settings &settings, const ProgressCb &progress, const CategoryCb &emit,
                     CancelToken &cancel) {
    const fs::path home = home_dir();

    auto excl = [&](const fs::path &p) { return settings.path_excluded(p); };

    if (!settings.custom_scan_paths.empty()) {
        if (progress) progress("Querying custom rulesets...");
        std::vector<fs::path> custom_paths;
        for (const auto &s : settings.custom_scan_paths) {
            fs::path p(s);
            std::error_code ec;
            if (fs::exists(p, ec)) custom_paths.push_back(p);
        }
        if (!custom_paths.empty()) {
            auto sizes = dir_size_many(custom_paths, progress, cancel);
            Category cat{"custom_paths", "Custom Targeted Paths", "User-specified scan configuration paths",
                          "folder-saved-search-symbolic", {}};
            for (const auto &cp : custom_paths) {
                if (excl(cp)) continue;
                std::uint64_t sz = sizes.count(cp) ? sizes.at(cp) : 0;
                if (sz) cat.entries.push_back({cp, sz, true});
            }
            if (!cat.entries.empty()) emit(std::move(cat));
        }
    }
    if (cancel.is_cancelled()) return;

    std::vector<fs::path> known_dirs = {
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
    };
    if (progress) progress("Sizing common junk paths…");
    auto batch_sizes = dir_size_many(known_dirs, progress, cancel);
    auto bsz = [&](const fs::path &p) { return batch_sizes.count(p) ? batch_sizes.at(p) : 0; };

    if (cancel.is_cancelled()) return;

    if (settings.cat_on("trash")) {
        if (progress) progress("~/.local/share/Trash");
        std::vector<FileEntry> entries;
        for (const fs::path &d : {home / ".local/share/Trash/files", home / ".local/share/Trash/info"}) {
            std::error_code ec;
            if (!fs::exists(d, ec) || excl(d)) continue;
            std::vector<fs::path> items;
            for (auto &e : fs::directory_iterator(d, fs::directory_options::skip_permission_denied, ec)) {
                if (ec) break;
                items.push_back(e.path());
            }
            std::vector<fs::path> dirs;
            for (auto &i : items) {
                std::error_code dec;
                if (fs::is_directory(i, dec)) dirs.push_back(i);
            }
            auto item_sizes = dir_size_many(dirs, progress, cancel);
            for (auto &item : items) {
                if (excl(item)) continue;
                std::error_code iec;
                std::uint64_t sz;
                if (item_sizes.count(item)) {
                    sz = item_sizes.at(item);
                } else {
                    sz = fs::is_regular_file(item, iec) ? fs::file_size(item, iec) : 0;
                }
                if (sz) entries.push_back({item, sz, true});
            }
        }
        if (!entries.empty())
            emit({"trash", "Trash", "~/.local/share/Trash", "user-trash-symbolic", std::move(entries)});
    }
    if (cancel.is_cancelled()) return;

    if (settings.cat_on("thumbs")) {
        fs::path thumb = home / ".cache/thumbnails";
        std::error_code ec;
        if (fs::exists(thumb, ec) && !excl(thumb)) {
            auto sz = bsz(thumb);
            if (sz)
                emit({"thumbs", "Thumbnail cache", "~/.cache/thumbnails", "image-x-generic-symbolic",
                      {{thumb, sz, true}}});
        }
    }
    if (cancel.is_cancelled()) return;

    if (settings.cat_on("browser_cache")) {
        std::vector<std::pair<std::string, fs::path>> browsers = {
            {"Firefox cache", home / ".cache/mozilla/firefox"},
            {"Chromium cache", home / ".cache/chromium"},
            {"Chrome cache", home / ".config/google-chrome/Default/Cache"},
        };
        for (auto &[label, p] : browsers) {
            std::error_code ec;
            if (fs::exists(p, ec) && !excl(p)) {
                auto sz = bsz(p);
                if (sz)
                    emit({"browser_" + label, label, display_path(p), "web-browser-symbolic",
                          {{p, sz, true}}});
            }
        }
    }
    if (cancel.is_cancelled()) return;

    if (settings.cat_on("flatpak")) {
        std::vector<FileEntry> flatpak_entries;
        fs::path user_flatpak = home / ".local/share/flatpak/repo";
        std::error_code ec;
        if (fs::exists(user_flatpak, ec) && !excl(user_flatpak) && is_writable(user_flatpak)) {
            auto sz = bsz(user_flatpak);
            if (sz > (1u << 20)) flatpak_entries.push_back({user_flatpak, sz, true});
        }

        fs::path var_app = home / ".var/app";
        if (fs::exists(var_app, ec) && !excl(var_app)) {
            std::vector<fs::path> app_caches;
            for (auto &d : fs::directory_iterator(var_app, fs::directory_options::skip_permission_denied, ec)) {
                if (ec) break;
                fs::path cache = d.path() / "cache";
                std::error_code cec;
                if (fs::exists(cache, cec)) app_caches.push_back(cache);
            }
            if (!app_caches.empty()) {
                auto cache_sizes = dir_size_many(app_caches, progress, cancel);
                for (auto &[path, sz] : cache_sizes) {
                    if (sz > (5u << 20) && !excl(path)) flatpak_entries.push_back({path, sz, true});
                }
            }
        }
        if (!flatpak_entries.empty())
            emit({"flatpak_repo", "Flatpak application storage", "Flatpak cache layers & local repos",
                  "package-x-generic-symbolic", std::move(flatpak_entries)});
    }
    if (cancel.is_cancelled()) return;

    if (settings.cat_on("pkg_cache")) {
        for (const std::string name : {"yay", "paru"}) {
            fs::path p = home / ".cache" / name;
            std::error_code ec;
            if (fs::exists(p, ec) && !excl(p)) {
                auto sz = bsz(p);
                if (sz)
                    emit({name, name + " package cache", display_path(p), "package-x-generic-symbolic",
                          {{p, sz, true}}});
            }
        }
        fs::path pip_cache = home / ".cache/pip";
        std::error_code ec;
        if (fs::exists(pip_cache, ec) && !excl(pip_cache)) {
            auto sz = bsz(pip_cache);
            if (sz)
                emit({"pip", "pip cache", "~/.cache/pip", "application-x-python-bytecode-symbolic",
                      {{pip_cache, sz, true}}});
        }
    }
    if (cancel.is_cancelled()) return;

    if (settings.cat_on("logs")) {
        std::vector<FileEntry> log_entries;
        fs::path xorg_dir = home / ".local/share/xorg";
        std::error_code ec;
        if (fs::exists(xorg_dir, ec)) {
            for (auto &e : fs::directory_iterator(xorg_dir, fs::directory_options::skip_permission_denied, ec)) {
                if (ec) break;
                std::string name = e.path().filename().string();
                bool matches = name.rfind("Xorg.", 0) == 0 &&
                               (ends_with(name, ".old") || ends_with(name, ".log.old"));
                if (!matches) continue;
                if (excl(e.path()) || !is_writable(e.path())) continue;
                std::error_code fec;
                if (fs::is_regular_file(e.path(), fec)) {
                    auto sz = fs::file_size(e.path(), fec);
                    if (!fec) log_entries.push_back({e.path(), sz, true});
                }
            }
        }

        std::vector<fs::path> journal_paths = {
            fs::path("/run/user") / std::to_string(getuid()) / "systemd/journal",
            home / ".local/share/systemd",
        };
        for (auto &jpath : journal_paths) {
            if (fs::exists(jpath, ec) && !excl(jpath) && is_writable(jpath)) {
                auto sz = dir_size(jpath, progress, cancel);
                if (sz) log_entries.push_back({jpath, sz, true});
            }
        }
        if (!log_entries.empty())
            emit({"logs", "System logs", "Xorg diagnostic logs and local user journal instances",
                  "text-x-generic-symbolic", std::move(log_entries)});
    }
    if (cancel.is_cancelled()) return;

    if (settings.cat_on("downloads")) {
        fs::path downloads = home / "Downloads";
        std::error_code ec;
        if (fs::exists(downloads, ec) && !excl(downloads)) {
            auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(30 * 24);
            std::vector<FileEntry> dl_entries;
            std::vector<fs::path> top_items;
            for (auto &e : fs::directory_iterator(downloads, fs::directory_options::skip_permission_denied, ec)) {
                if (ec) break;
                top_items.push_back(e.path());
            }
            if (progress)
                progress("Inspecting " + std::to_string(top_items.size()) + " items inside Downloads…");
            std::vector<fs::path> dirs;
            for (auto &i : top_items) {
                std::error_code dec;
                if (fs::is_directory(i, dec)) dirs.push_back(i);
            }
            auto dl_dir_sizes = dir_size_many(dirs, progress, cancel);
            for (auto &item : top_items) {
                if (excl(item)) continue;
                if (progress) progress(item.string());
                std::error_code sec;
                bool is_file = fs::is_regular_file(item, sec);
                std::uint64_t sz = is_file ? fs::file_size(item, sec)
                                            : (dl_dir_sizes.count(item) ? dl_dir_sizes.at(item) : 0);
                struct stat st{};
                if (::stat(item.c_str(), &st) != 0) continue;
                auto atime = std::chrono::system_clock::from_time_t(st.st_atime);
                if (sz >= (50u << 20) && atime < cutoff) dl_entries.push_back({item, sz, true});
            }
            if (!dl_entries.empty())
                emit({"downloads", "Large historical downloads",
                      "~/Downloads — files over 50 MB untouched for 30+ days", "folder-download-symbolic",
                      std::move(dl_entries)});
        }
    }
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
};

struct JunkMeta {
    std::string title, subtitle, icon;
};

const std::map<std::string, JunkMeta> DEEP_JUNK_META = {
    {"node_modules", {"node_modules directories", "JavaScript code dependencies", "code-symbolic"}},
    {"__pycache__",
     {"Python bytecode", "Compiled bytecode targets", "application-x-python-bytecode-symbolic"}},
    {".mypy_cache", {"mypy cache", "Python type checker cache", "text-x-generic-symbolic"}},
    {".pytest_cache", {"pytest cache", "Test execution cache artifacts", "text-x-generic-symbolic"}},
    {".ruff_cache", {"ruff cache", "Linter caching systems", "text-x-generic-symbolic"}},
    {"dist", {"Distribution targets", "Compiled distribution outputs", "package-x-generic-symbolic"}},
    {"build", {"Build outputs", "Build compilation folders", "package-x-generic-symbolic"}},
    {".gradle", {".gradle environment", "Gradle build caching blocks", "package-x-generic-symbolic"}},
    {".m2", {".m2 local cache", "Maven repository cache trees", "package-x-generic-symbolic"}},
    {"target", {"Rust target profiles", "Rust/Cargo target cache artifacts", "package-x-generic-symbolic"}},
    {".dart_tool", {".dart_tool assets", "Dart environment metadata", "package-x-generic-symbolic"}},
    {".pub-cache",
     {"Dart package caches", "Dart/Flutter external dependencies", "package-x-generic-symbolic"}},
    {"stale_bytecode",
     {"Isolated binary artifacts", "Loose .pyc, .class, and .o files", "text-x-generic-symbolic"}},
};

// Fixed vs. the original Python: it checked filename suffix membership in a
// set that included ".so.old", but Path.suffix() only ever returns the
// *last* extension component ("libfoo.so.old".suffix == ".old", never
// ".so.old") — so files matching that pattern were silently never flagged
// despite the category's own name/intent. Here the compound suffix is
// matched explicitly against the filename.
bool is_stale_bytecode(const std::string &filename) {
    static const std::set<std::string> exact = {".pyc", ".pyo", ".o", ".a", ".class"};
    std::string lname = lower(filename);
    if (ends_with(lname, ".so.old")) return true;
    fs::path p(filename);
    return exact.count(lower(p.extension().string())) > 0;
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
        std::string name = e.path().filename().string();
        if (DEEP_SKIP_DIRS.count(name) || settings.path_excluded(e.path())) continue;

        std::error_code sec;
        auto st = e.symlink_status(sec);
        if (sec || fs::is_symlink(st)) continue;

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
            progress("Analyzing " + std::to_string(junk_found.size()) + " artifacts in " +
                      p.filename().string() + "/…");
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
        if (progress) progress(sub.string());
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
    if (progress) progress("Querying standard file-system paths…");
    scan_generator(settings, progress, emit, cancel);
    if (cancel.is_cancelled()) return;

    if (progress) progress("Performing full file-system structural search…");
    auto buckets = deep_walk(home, settings, progress, cancel);
    if (cancel.is_cancelled()) return;
    emit_deep_buckets(buckets, settings, progress, emit, cancel, "deep");
}

void folder_scan_generator(const fs::path &root, const Settings &settings, const ProgressCb &progress,
                            const CategoryCb &emit, CancelToken &cancel) {
    if (progress) progress("Inspecting directory target " + root.string() + "…");
    auto buckets = deep_walk(root, settings, progress, cancel);
    if (cancel.is_cancelled()) return;

    std::vector<FileEntry> large_entries;
    std::error_code ec;
    for (auto &item : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        std::error_code sec;
        auto st = item.symlink_status(sec);
        if (sec || fs::is_symlink(st) || !fs::is_regular_file(st)) continue;
        if (settings.path_excluded(item.path())) continue;
        std::error_code fec;
        auto sz = fs::file_size(item.path(), fec);
        if (!fec && sz >= (50u << 20)) large_entries.push_back({item.path(), sz, true});
    }
    if (!large_entries.empty())
        emit({"folder_large", "Large root objects", "Objects ≥50 MB inside " + root.filename().string(),
              "folder-download-symbolic", std::move(large_entries)});
    if (cancel.is_cancelled()) return;

    emit_deep_buckets(buckets, settings, progress, emit, cancel, "folder");
}

}  // namespace fc
