#include "settings.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

#include "common.hpp"
#include "third_party/nlohmann/json.hpp"

namespace fc {

using json = nlohmann::json;

const std::vector<std::pair<std::string, std::string>> SCAN_CATEGORIES = {
    {"recycle_bin", "Recycle Bin"},
    {"temp", "Windows/user Temp folders"},
    {"thumbs", "Thumbnail & icon cache"},
    {"browser_cache", "Browser caches (Chrome, Edge, Firefox)"},
    {"pkg_cache", "Package manager caches (pip, npm)"},
    {"logs", "Crash dumps & error reports"},
    {"downloads", "Large old downloads (>50 MB, >30 days)"},
    {"node_modules", "node_modules folders"},
    {"pycache", "Python __pycache__ / bytecode"},
    {"build_dirs", "Build output folders (dist, build, target…)"},
    {"dart", "Dart / Flutter caches"},
    {"stale_bytecode", "Stale bytecode files (.pyc, .obj, .class)"},
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

std::filesystem::path settings_path() {
    if (const char *appdata = std::getenv("APPDATA"))
        return fs::path(appdata) / "FileCleaner" / "settings.json";
    return home_dir() / "FileCleaner" / "settings.json";
}

Settings::Settings() {
    for (const auto &kv : SCAN_CATEGORIES) enabled[kv.first] = true;
}

bool Settings::cat_on(const std::string &key) const {
    auto it = enabled.find(key);
    return it == enabled.end() ? true : it->second;
}

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
}  // namespace

bool Settings::path_excluded(const fs::path &p) const {
    // Windows paths are case-insensitive, so exclusions are matched
    // case-insensitively (unlike the Linux version).
    const std::string sp = lower(p.lexically_normal().string());
    for (const auto &ex_raw : excluded_paths) {
        std::string ex = lower(fs::path(ex_raw).lexically_normal().string());
        while (ex.size() > 1 && (ex.back() == '\\' || ex.back() == '/')) ex.pop_back();
        if (sp == ex) return true;
        if (sp.size() > ex.size() && sp.compare(0, ex.size(), ex) == 0 &&
            (sp[ex.size()] == '\\' || sp[ex.size()] == '/'))
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

    return s;
}

}  // namespace fc
