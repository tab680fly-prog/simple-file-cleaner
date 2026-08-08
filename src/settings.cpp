#include "settings.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "third_party/nlohmann/json.hpp"

namespace fc {

using json = nlohmann::json;

const std::vector<std::pair<std::string, std::string>> SCAN_CATEGORIES = {
    {"trash", "Trash"},
    {"thumbs", "Thumbnail cache"},
    {"browser_cache", "Browser caches (Firefox, Chrome, Chromium)"},
    {"flatpak", "Flatpak app caches"},
    {"pkg_cache", "Package manager caches (yay, paru, pip)"},
    {"logs", "Old logs & systemd journal records"},
    {"downloads", "Large old downloads (>50 MB, >30 days)"},
    {"node_modules", "node_modules folders"},
    {"pycache", "Python __pycache__ / bytecode"},
    {"build_dirs", "Build output folders (dist, build, target…)"},
    {"dart", "Dart / Flutter caches"},
    {"stale_bytecode", "Stale bytecode files (.pyc, .o, .class)"},
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

static fs::path home_dir() {
    if (const char *h = std::getenv("HOME")) return fs::path(h);
    return fs::path("/");
}

std::filesystem::path settings_path() {
    return home_dir() / ".config" / "filecleaner.json";
}

Settings::Settings() {
    for (const auto &kv : SCAN_CATEGORIES) enabled[kv.first] = true;
}

bool Settings::cat_on(const std::string &key) const {
    auto it = enabled.find(key);
    return it == enabled.end() ? true : it->second;
}

bool Settings::path_excluded(const fs::path &p) const {
    const std::string sp = p.lexically_normal().string();
    for (const auto &ex_raw : excluded_paths) {
        std::string ex = fs::path(ex_raw).lexically_normal().string();
        while (ex.size() > 1 && ex.back() == '/') ex.pop_back();
        if (sp == ex) return true;
        if (sp.size() > ex.size() && sp.compare(0, ex.size(), ex) == 0 && sp[ex.size()] == '/')
            return true;
    }
    return false;
}

void Settings::save() const {
    json j;
    json enabled_j = json::object();
    for (const auto &kv : enabled) enabled_j[kv.first] = kv.second;
    j["enabled"] = enabled_j;
    j["excluded_paths"] = excluded_paths;
    j["custom_scan_paths"] = custom_scan_paths;
    j["animation"] = animation;
    j["auto_rescan"] = auto_rescan;

    std::error_code ec;
    fs::create_directories(settings_path().parent_path(), ec);
    std::ofstream f(settings_path());
    if (f) f << j.dump(2);
}

Settings Settings::load() {
    Settings s;
    std::ifstream f(settings_path());
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

    return s;
}

}  // namespace fc
