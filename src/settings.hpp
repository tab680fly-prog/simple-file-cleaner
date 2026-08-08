#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace fc {

namespace fs = std::filesystem;

// Ordered list (key, display label) — order matters for the Settings UI.
extern const std::vector<std::pair<std::string, std::string>> SCAN_CATEGORIES;

// Maps a settings category key to the set of directory names it governs
// during a deep/folder scan.
extern const std::map<std::string, std::set<std::string>> DEEP_JUNK_CATEGORY_MAP;

bool is_known_category(const std::string &key);

struct Settings {
    std::map<std::string, bool> enabled;
    std::vector<std::string> excluded_paths;
    std::vector<std::string> custom_scan_paths;
    std::string animation = "magnifier";
    bool auto_rescan = true;

    Settings();

    void save() const;
    static Settings load();

    bool cat_on(const std::string &key) const;

    // Fixed vs. the original Python: matches the exact path or a path
    // rooted under an excluded directory, rather than a raw string prefix
    // (which previously made an exclusion of "/home/user/dev" also match
    // an unrelated sibling like "/home/user/development").
    bool path_excluded(const fs::path &p) const;
};

std::filesystem::path settings_path();

}  // namespace fc
