#include "settings.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "common.hpp"
#include "third_party/nlohmann/json.hpp"

namespace fc {

using json = nlohmann::json;

const std::vector<std::pair<std::string, std::string>> SCAN_CATEGORIES = {
#ifdef _WIN32
    {"trash", "Recycle Bin"},
    {"temp", "Temporary files (unused for 7+ days)"},
    {"thumbs", "Thumbnail cache"},
    {"browser_cache", "Browser caches (Chrome, Edge, Firefox, Brave)"},
    {"pkg_cache", "Package manager caches (pip, npm, Yarn, NuGet)"},
    {"logs", "Crash dumps and error reports"},
#else
    {"trash", "Trash"},
    {"thumbs", "Thumbnail cache"},
    {"browser_cache", "Browser caches (Firefox, Chrome, Chromium)"},
    {"flatpak", "Flatpak application caches"},
    {"pkg_cache", "Package manager caches (yay, paru, pip)"},
    {"logs", "Old logs and user journal files"},
#endif
    {"downloads", "Large, unused downloads (over 50 MB, 30+ days)"},
    {"node_modules", "node_modules folders"},
    {"pycache", "Python caches (__pycache__, mypy, pytest, ruff)"},
    {"build_dirs", "Build output folders (dist, build, target, …)"},
    {"dart", "Dart and Flutter caches"},
    {"stale_bytecode", "Compiled bytecode files (.pyc, .pyo, .class)"},
};

const std::map<std::string, std::set<std::string>> DEEP_JUNK_CATEGORY_MAP = {
    {"node_modules", {"node_modules"}},
    {"pycache", {"__pycache__", ".mypy_cache", ".pytest_cache", ".ruff_cache"}},
    {"build_dirs", {"dist", "build", ".gradle", ".m2", "target"}},
    {"dart", {".dart_tool", ".pub-cache"}},
};

bool is_known_category(const std::string &key) {
    for (const auto &kv : SCAN_CATEGORIES)
        if (kv.first == key) return true;
    return false;
}

std::filesystem::path settings_path() { return settings_file(); }

Settings::Settings() {
    for (const auto &kv : SCAN_CATEGORIES) enabled[kv.first] = true;
}

bool Settings::cat_on(const std::string &key) const {
    auto it = enabled.find(key);
    return it == enabled.end() ? true : it->second;
}

const std::vector<std::string> &Settings::normalized_exclusions() const {
    if (excl_cache_valid_ && excl_cache_src_ == excluded_paths) return excl_cache_;

    excl_cache_valid_ = true;
    excl_cache_src_ = excluded_paths;
    excl_cache_.clear();

    auto add = [&](const std::string &key) {
        if (key.empty()) return;
        if (std::find(excl_cache_.begin(), excl_cache_.end(), key) == excl_cache_.end()) excl_cache_.push_back(key);
    };

    // Built-in protections (e.g. C:\Windows) apply whatever the settings say.
    for (const auto &k : builtin_protected_keys()) add(k);

    const std::string home = path_key(path_from(expand_user_path(path_str(home_dir()))));
    std::error_code ec;
    std::string canon_home = path_key(fs::weakly_canonical(home_dir(), ec));
    if (ec) canon_home = home;

    auto add_with_home_aliases = [&](const std::string &key) {
        add(key);
        // Scanned paths are built from the home directory as reported by
        // the environment, while paths picked in a file chooser may come
        // back in canonical form (e.g. /var/home on Fedora Atomic), or vice
        // versa.
        auto swap_prefix = [&](const std::string &from, const std::string &to) {
            if (from != to && key_is_under(key, from)) add(to + key.substr(from.size()));
        };
        swap_prefix(home, canon_home);
        swap_prefix(canon_home, home);
    };

    for (const auto &raw : excluded_paths) {
        const fs::path norm = path_from(expand_user_path(raw));
        if (norm.empty() || !norm.is_absolute()) continue;
        add_with_home_aliases(path_key(norm));
        std::error_code cec;
        fs::path canon = fs::weakly_canonical(norm, cec);
        if (!cec) add_with_home_aliases(path_key(canon));
    }
    return excl_cache_;
}

bool Settings::path_excluded(const fs::path &p) const {
    const std::string key = path_key(p);
    for (const auto &ex : normalized_exclusions())
        if (key_is_under(key, ex)) return true;
    return false;
}

bool Settings::contains_excluded(const fs::path &p) const {
    const std::string key = path_key(p);
    for (const auto &ex : normalized_exclusions())
        if (ex != key && key_is_under(ex, key)) return true;
    return false;
}

bool Settings::must_preserve(const fs::path &p) const { return path_excluded(p) || contains_excluded(p); }

void Settings::save() const {
    json j;
    json enabled_j = json::object();
    for (const auto &kv : enabled) enabled_j[kv.first] = kv.second;
    j["enabled"] = enabled_j;
    j["excluded_paths"] = excluded_paths;
    j["custom_scan_paths"] = custom_scan_paths;
    j["animation"] = animation;
    j["auto_rescan"] = auto_rescan;
    j["exclusion_warning_acknowledged"] = exclusion_warning_acknowledged;

    std::error_code ec;
    fs::create_directories(settings_path().parent_path(), ec);
    std::ofstream f(settings_path(), std::ios::binary);
    if (f) f << j.dump(2);
}

Settings Settings::load() {
    Settings s;
    std::ifstream f(settings_path(), std::ios::binary);
    if (!f) return s;

    json j;
    try {
        f >> j;
    } catch (...) {
        return s;
    }

    if (j.contains("enabled") && j["enabled"].is_object()) {
        for (auto it = j["enabled"].begin(); it != j["enabled"].end(); ++it) {
            if (is_known_category(it.key()) && it.value().is_boolean())
                s.enabled[it.key()] = it.value().get<bool>();
        }
    }
    if (j.contains("excluded_paths") && j["excluded_paths"].is_array()) {
        for (const auto &v : j["excluded_paths"])
            if (v.is_string()) s.excluded_paths.push_back(v.get<std::string>());
    }
    if (j.contains("custom_scan_paths") && j["custom_scan_paths"].is_array()) {
        for (const auto &v : j["custom_scan_paths"])
            if (v.is_string()) s.custom_scan_paths.push_back(v.get<std::string>());
    }
    if (j.contains("animation") && j["animation"].is_string()) {
        std::string a = j["animation"].get<std::string>();
        if (a == "magnifier" || a == "spinner") s.animation = a;
    }
    if (j.contains("auto_rescan") && j["auto_rescan"].is_boolean())
        s.auto_rescan = j["auto_rescan"].get<bool>();
    if (j.contains("exclusion_warning_acknowledged") && j["exclusion_warning_acknowledged"].is_boolean())
        s.exclusion_warning_acknowledged = j["exclusion_warning_acknowledged"].get<bool>();

    return s;
}

}  // namespace fc
