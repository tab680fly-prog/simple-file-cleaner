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
    std::string icon;
    std::vector<FileEntry> entries;

    std::uint64_t total_size() const {
        std::uint64_t s = 0;
        for (const auto &e : entries) s += e.size;
        return s;
    }

    std::uint64_t selected_size() const {
        std::uint64_t s = 0;
        for (const auto &e : entries)
            if (e.selected) s += e.size;
        return s;
    }
};

std::string fmt_size(std::uint64_t n);

// Returns the home-relative display form ("~/...") when path is under home.
std::string display_path(const fs::path &p);

}  // namespace fc
