#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fc {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------

struct FileEntry {
    fs::path path;
    std::uint64_t size = 0;
    bool selected = true;
};

struct Category {
    std::string key;
    std::string title;
    std::string subtitle;
    std::vector<FileEntry> entries;

    std::uint64_t total_size() const {
        std::uint64_t s = 0;
        for (const auto &e : entries) s += e.size;
        return s;
    }
};

std::string fmt_size(std::uint64_t n);

// Returns the user-profile-relative display form ("%USERPROFILE%\...") when
// path is under the user's profile directory.
std::string display_path(const fs::path &p);

// The current user's profile directory (%USERPROFILE%).
fs::path home_dir();

}  // namespace fc
