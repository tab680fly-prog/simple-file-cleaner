#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace fc {

namespace fs = std::filesystem;

// Ordered list (key, display label) — order matters for CLI listing.
extern const std::vector<std::pair<std::string, std::string>> SCAN_CATEGORIES;

// Maps a settings category key to the set of directory names it governs
// during a deep/folder scan.
extern const std::map<std::string, std::set<std::string>> DEEP_JUNK_CATEGORY_MAP;

bool is_known_category(const std::string &key);

struct Settings {
    std::map<std::string, bool> enabled;
    std::vector<std::string> excluded_paths;
    std::vector<std::string> custom_scan_paths;

    Settings();

    void save() const;
    static Settings load();

    bool cat_on(const std::string &key) const;

    // Matches the exact path or a path rooted under an excluded directory,
    // rather than a raw string prefix.
    bool path_excluded(const fs::path &p) const;
};

std::filesystem::path settings_path();

}  // namespace fc
