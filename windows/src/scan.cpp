#include "scan.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <set>
#include <thread>

namespace fc {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool ends_with(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

fs::path getenv_path(const char *name) {
    if (const char *v = std::getenv(name)) return fs::path(v);
    return {};
}

bool is_writable(const fs::path &p) {
    std::error_code ec;
    auto perms = fs::status(p, ec).permissions();
    if (ec) return false;
    return (perms & fs::perms::owner_write) != fs::perms::none;
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

namespace {

void emit_recycle_bin(const Settings &settings, const CategoryCb &emit) {
    if (!settings.cat_on("recycle_bin")) return;
    SHQUERYRBINFO info{};
    info.cbSize = sizeof(info);
    // NULL queries the Recycle Bin across all drives.
    if (SHQueryRecycleBinW(nullptr, &info) != S_OK) return;
    if (info.i64Size <= 0) return;
    Category cat{"recycle_bin", "Recycle Bin", "Files and folders you've deleted", {}};
    cat.entries.push_back({fs::path(kRecycleBinMarker), (std::uint64_t)info.i64Size, true});
    emit(std::move(cat));
}

void emit_browser_caches(const Settings &settings, const ProgressCb &progress, const CategoryCb &emit,
                          CancelToken &cancel) {
    if (!settings.cat_on("browser_cache")) return;
    fs::path local = getenv_path("LOCALAPPDATA");
    if (local.empty()) return;

    std::vector<std::pair<std::string, fs::path>> browsers = {
        {"Chrome cache", local / "Google/Chrome/User Data/Default/Cache"},
        {"Edge cache", local / "Microsoft/Edge/User Data/Default/Cache"},
    };
    for (auto &[label, p] : browsers) {
        std::error_code ec;
        if (!fs::exists(p, ec) || settings.path_excluded(p)) continue;
        auto sz = dir_size(p, progress, cancel);
        if (sz) emit({"browser_" + label, label, display_path(p), {{p, sz, true}}});
    }

    // Firefox keeps one cache2 folder per profile.
    fs::path ff_profiles = local / "Mozilla/Firefox/Profiles";
    std::error_code ec;
    if (fs::exists(ff_profiles, ec)) {
        for (auto &prof : fs::directory_iterator(ff_profiles, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            fs::path cache = prof.path() / "cache2";
            std::error_code cec;
            if (!fs::exists(cache, cec) || settings.path_excluded(cache)) continue;
            auto sz = dir_size(cache, progress, cancel);
            if (sz)
                emit({"browser_Firefox_" + prof.path().filename().string(), "Firefox cache",
                      display_path(cache), {{cache, sz, true}}});
        }
    }
}

void emit_temp(const Settings &settings, const ProgressCb &progress, const CategoryCb &emit,
                CancelToken &cancel) {
    if (!settings.cat_on("temp")) return;
    std::vector<fs::path> temp_dirs;
    if (auto t = getenv_path("TEMP"); !t.empty()) temp_dirs.push_back(t);
    if (auto local = getenv_path("LOCALAPPDATA"); !local.empty()) temp_dirs.push_back(local / "Temp");

    std::set<std::string> seen;
    Category cat{"temp", "Temp folders", "Leftover installer and runtime scratch files", {}};
    for (auto &t : temp_dirs) {
        std::error_code ec;
        std::string key = lower(t.lexically_normal().string());
        if (!fs::exists(t, ec) || settings.path_excluded(t) || seen.count(key)) continue;
        seen.insert(key);
        std::vector<fs::path> items;
        for (auto &e : fs::directory_iterator(t, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (!settings.path_excluded(e.path())) items.push_back(e.path());
        }
        auto sizes = dir_size_many(items, progress, cancel);
        for (auto &item : items) {
            auto sz = sizes.count(item) ? sizes.at(item) : 0;
            if (sz) cat.entries.push_back({item, sz, true});
        }
    }
    if (!cat.entries.empty()) emit(std::move(cat));
}

void emit_thumbs(const Settings &settings, const CategoryCb &emit) {
    if (!settings.cat_on("thumbs")) return;
    fs::path local = getenv_path("LOCALAPPDATA");
    if (local.empty()) return;
    fs::path explorer = local / "Microsoft/Windows/Explorer";
    std::error_code ec;
    if (!fs::exists(explorer, ec)) return;

    Category cat{"thumbs", "Thumbnail & icon cache", display_path(explorer), {}};
    for (auto &e : fs::directory_iterator(explorer, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        std::string name = lower(e.path().filename().string());
        if (name.rfind("thumbcache_", 0) != 0 && name.rfind("iconcache_", 0) != 0) continue;
        if (settings.path_excluded(e.path())) continue;
        std::error_code fec;
        if (fs::is_regular_file(e.path(), fec)) {
            auto sz = fs::file_size(e.path(), fec);
            if (!fec) cat.entries.push_back({e.path(), sz, true});
        }
    }
    if (!cat.entries.empty()) emit(std::move(cat));
}

void emit_pkg_cache(const Settings &settings, const ProgressCb &progress, const CategoryCb &emit,
                     CancelToken &cancel) {
    if (!settings.cat_on("pkg_cache")) return;
    fs::path local = getenv_path("LOCALAPPDATA");
    fs::path roaming = getenv_path("APPDATA");

    std::vector<std::pair<std::string, fs::path>> caches;
    if (!local.empty()) {
        caches.push_back({"pip cache", local / "pip/Cache"});
        caches.push_back({"npm cache", local / "npm-cache"});
    }
    if (!roaming.empty()) caches.push_back({"npm cache", roaming / "npm-cache"});

    for (auto &[label, p] : caches) {
        std::error_code ec;
        if (!fs::exists(p, ec) || settings.path_excluded(p)) continue;
        auto sz = dir_size(p, progress, cancel);
        if (sz) emit({label + "_" + p.string(), label, display_path(p), {{p, sz, true}}});
    }
}

void emit_logs(const Settings &settings, const ProgressCb &progress, const CategoryCb &emit,
                CancelToken &cancel) {
    if (!settings.cat_on("logs")) return;
    fs::path local = getenv_path("LOCALAPPDATA");
    if (local.empty()) return;

    std::vector<fs::path> log_dirs = {
        local / "CrashDumps",
        local / "Microsoft/Windows/WER/ReportArchive",
        local / "Microsoft/Windows/WER/ReportQueue",
    };
    std::vector<FileEntry> entries;
    for (auto &d : log_dirs) {
        std::error_code ec;
        if (!fs::exists(d, ec) || settings.path_excluded(d) || !is_writable(d)) continue;
        auto sz = dir_size(d, progress, cancel);
        if (sz) entries.push_back({d, sz, true});
    }
    if (!entries.empty())
        emit({"logs", "Crash dumps & error reports", "Windows Error Reporting archives", std::move(entries)});
}

void emit_downloads(const Settings &settings, const ProgressCb &progress, const CategoryCb &emit,
                     CancelToken &cancel) {
    if (!settings.cat_on("downloads")) return;
    fs::path downloads = home_dir() / "Downloads";
    std::error_code ec;
    if (!fs::exists(downloads, ec) || settings.path_excluded(downloads)) return;

    auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(30 * 24);
    std::vector<fs::path> top_items;
    for (auto &e : fs::directory_iterator(downloads, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        top_items.push_back(e.path());
    }
    if (progress) progress("Inspecting " + std::to_string(top_items.size()) + " items inside Downloads...");

    std::vector<fs::path> dirs;
    for (auto &i : top_items) {
        std::error_code dec;
        if (fs::is_directory(i, dec)) dirs.push_back(i);
    }
    auto dl_dir_sizes = dir_size_many(dirs, progress, cancel);

    std::vector<FileEntry> entries;
    for (auto &item : top_items) {
        if (settings.path_excluded(item)) continue;
        std::error_code sec;
        bool is_file = fs::is_regular_file(item, sec);
        std::uint64_t sz =
            is_file ? fs::file_size(item, sec) : (dl_dir_sizes.count(item) ? dl_dir_sizes.at(item) : 0);
        std::error_code tec;
        auto mtime = fs::last_write_time(item, tec);
        if (tec) continue;
        if (sz >= (50u << 20) && mtime < cutoff) entries.push_back({item, sz, true});
    }
    if (!entries.empty())
        emit({"downloads", "Large historical downloads",
              "Downloads - files over 50 MB untouched for 30+ days", std::move(entries)});
}

}  // namespace

void scan_generator(const Settings &settings, const ProgressCb &progress, const CategoryCb &emit,
                     CancelToken &cancel) {
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
            Category cat{"custom_paths", "Custom Targeted Paths", "User-specified scan configuration paths", {}};
            for (const auto &cp : custom_paths) {
                if (settings.path_excluded(cp)) continue;
                std::uint64_t sz = sizes.count(cp) ? sizes.at(cp) : 0;
                if (sz) cat.entries.push_back({cp, sz, true});
            }
            if (!cat.entries.empty()) emit(std::move(cat));
        }
    }
    if (cancel.is_cancelled()) return;

    if (progress) progress("Querying the Recycle Bin...");
    emit_recycle_bin(settings, emit);
    if (cancel.is_cancelled()) return;

    if (progress) progress("Sizing Temp folders...");
    emit_temp(settings, progress, emit, cancel);
    if (cancel.is_cancelled()) return;

    if (progress) progress("Sizing thumbnail cache...");
    emit_thumbs(settings, emit);
    if (cancel.is_cancelled()) return;

    if (progress) progress("Sizing browser caches...");
    emit_browser_caches(settings, progress, emit, cancel);
    if (cancel.is_cancelled()) return;

    if (progress) progress("Sizing package manager caches...");
    emit_pkg_cache(settings, progress, emit, cancel);
    if (cancel.is_cancelled()) return;

    if (progress) progress("Sizing crash dumps & error reports...");
    emit_logs(settings, progress, emit, cancel);
    if (cancel.is_cancelled()) return;

    if (progress) progress("Inspecting Downloads...");
    emit_downloads(settings, progress, emit, cancel);
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
    ".git",  ".hg",     ".svn",   ".cargo", ".rustup", ".nvm",  ".rbenv",
    ".sdkman", "AppData", "$RECYCLE.BIN",  "System Volume Information",
    "Windows", "Program Files", "Program Files (x86)", "ProgramData",
    "Music", "Videos", "Pictures", "Games",
};

struct JunkMeta {
    std::string title, subtitle;
};

const std::map<std::string, JunkMeta> DEEP_JUNK_META = {
    {"node_modules", {"node_modules directories", "JavaScript code dependencies"}},
    {"__pycache__", {"Python bytecode", "Compiled bytecode targets"}},
    {".mypy_cache", {"mypy cache", "Python type checker cache"}},
    {".pytest_cache", {"pytest cache", "Test execution cache artifacts"}},
    {".ruff_cache", {"ruff cache", "Linter caching systems"}},
    {"dist", {"Distribution targets", "Compiled distribution outputs"}},
    {"build", {"Build outputs", "Build compilation folders"}},
    {".gradle", {".gradle environment", "Gradle build caching blocks"}},
    {".m2", {".m2 local cache", "Maven repository cache trees"}},
    {"target", {"Rust target profiles", "Rust/Cargo target cache artifacts"}},
    {".dart_tool", {".dart_tool assets", "Dart environment metadata"}},
    {".pub-cache", {"Dart package caches", "Dart/Flutter external dependencies"}},
    {"stale_bytecode", {"Isolated binary artifacts", "Loose .pyc, .class, and .obj files"}},
};

bool is_stale_bytecode(const std::string &filename) {
    static const std::set<std::string> exact = {".pyc", ".pyo", ".obj", ".lib", ".class"};
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
                      p.filename().string() + "...");
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
        if (!entries.empty()) emit({prefix + "_" + key, title, subtitle, std::move(entries)});
    }
}

}  // namespace

void deep_scan_generator(const Settings &settings, const ProgressCb &progress, const CategoryCb &emit,
                          CancelToken &cancel) {
    fs::path home = home_dir();
    if (progress) progress("Querying standard file-system paths...");
    scan_generator(settings, progress, emit, cancel);
    if (cancel.is_cancelled()) return;

    if (progress) progress("Performing full profile structural search...");
    auto buckets = deep_walk(home, settings, progress, cancel);
    if (cancel.is_cancelled()) return;
    emit_deep_buckets(buckets, settings, progress, emit, cancel, "deep");
}

void folder_scan_generator(const fs::path &root, const Settings &settings, const ProgressCb &progress,
                            const CategoryCb &emit, CancelToken &cancel) {
    if (progress) progress("Inspecting directory target " + root.string() + "...");
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
        emit({"folder_large", "Large root objects", "Objects >=50 MB inside " + root.filename().string(),
              std::move(large_entries)});
    if (cancel.is_cancelled()) return;

    emit_deep_buckets(buckets, settings, progress, emit, cancel, "folder");
}

}  // namespace fc
