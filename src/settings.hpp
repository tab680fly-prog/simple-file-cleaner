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
    // The magnifying glass is the default scan animation.
    std::string animation = "magnifier";
    bool auto_rescan = true;
    // Set once the first-launch "protect important folders" notice has been
    // answered, so it is only shown a single time.
    bool exclusion_warning_acknowledged = false;

    Settings();

    void save() const;
    static Settings load();

    bool cat_on(const std::string &key) const;

    // Fixed vs. the original Python: matches the exact path or a path
    // rooted under an excluded directory, rather than a raw string prefix
    // (which previously made an exclusion of "/home/user/dev" also match
    // an unrelated sibling like "/home/user/development").
    bool path_excluded(const fs::path &p) const;

    // True when an excluded path lies strictly inside p. Deleting p would
    // then also delete the excluded path, so p must not be offered either.
    bool contains_excluded(const fs::path &p) const;

    // Convenience: path_excluded(p) || contains_excluded(p).
    bool must_preserve(const fs::path &p) const;

   private:
    // Normalized exclusion keys (see path_key), including the built-in
    // protected folders ("~" expanded, trailing separator removed, plus
    // the canonical form when the path goes through a symlink such as
    // /home -> /var/home on Fedora Atomic). Rebuilt whenever
    // excluded_paths changes. Each Settings copy is only ever used from one
    // thread at a time, so the lazy rebuild needs no locking.
    mutable bool excl_cache_valid_ = false;
    mutable std::vector<std::string> excl_cache_src_;
    mutable std::vector<std::string> excl_cache_;
    const std::vector<std::string> &normalized_exclusions() const;
};

std::filesystem::path settings_path();

}  // namespace fc
