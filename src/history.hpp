#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fc {

struct HistoryEntry {
    std::string id;
    std::string date;  // ISO-8601, local time, seconds precision
    std::string mode;
    int categories_found = 0;
    std::uint64_t total_found = 0;
    std::uint64_t total_deleted = 0;
    int deleted_count = 0;

    std::string date_fmt() const;
    std::string total_found_fmt() const;
    std::string total_deleted_fmt() const;
};

std::filesystem::path history_path();

std::vector<HistoryEntry> load_history();
void save_history(const std::vector<HistoryEntry> &entries);
void add_history_entry(const HistoryEntry &entry);

// Returns the current local time as an ISO-8601 string suitable for
// HistoryEntry::date.
std::string iso_now();

}  // namespace fc
