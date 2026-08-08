#include "history.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

#include "common.hpp"
#include "third_party/nlohmann/json.hpp"

namespace fc {

using json = nlohmann::json;
namespace fs = std::filesystem;

static fs::path home_dir() {
    if (const char *h = std::getenv("HOME")) return fs::path(h);
    return fs::path("/");
}

std::filesystem::path history_path() {
    return home_dir() / ".local" / "share" / "filecleaner" / "history.json";
}

std::string iso_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

std::string HistoryEntry::date_fmt() const {
    std::tm tm{};
    if (strptime(date.c_str(), "%Y-%m-%dT%H:%M:%S", &tm) == nullptr) return date;
    char buf[64];
    std::strftime(buf, sizeof(buf), "%d %b %Y, %H:%M", &tm);
    return buf;
}

std::string HistoryEntry::total_found_fmt() const { return fmt_size(total_found); }
std::string HistoryEntry::total_deleted_fmt() const { return fmt_size(total_deleted); }

std::vector<HistoryEntry> load_history() {
    std::vector<HistoryEntry> out;
    std::ifstream f(history_path());
    if (!f) return out;

    json j;
    try {
        f >> j;
    } catch (...) {
        return out;
    }
    if (!j.is_array()) return out;

    for (const auto &item : j) {
        try {
            HistoryEntry e;
            e.id = item.value("id", "");
            e.date = item.value("date", "");
            e.mode = item.value("mode", "");
            e.categories_found = item.value("categories_found", 0);
            e.total_found = item.value("total_found", (std::uint64_t)0);
            e.total_deleted = item.value("total_deleted", (std::uint64_t)0);
            e.deleted_count = item.value("deleted_count", 0);
            out.push_back(std::move(e));
        } catch (...) {
        }
    }
    return out;
}

void save_history(const std::vector<HistoryEntry> &entries) {
    json arr = json::array();
    for (const auto &e : entries) {
        json item;
        item["id"] = e.id;
        item["date"] = e.date;
        item["mode"] = e.mode;
        item["categories_found"] = e.categories_found;
        item["total_found"] = e.total_found;
        item["total_deleted"] = e.total_deleted;
        item["deleted_count"] = e.deleted_count;
        arr.push_back(std::move(item));
    }

    std::error_code ec;
    fs::create_directories(history_path().parent_path(), ec);
    std::ofstream f(history_path());
    if (f) f << arr.dump(2);
}

void add_history_entry(const HistoryEntry &entry) {
    auto entries = load_history();
    entries.insert(entries.begin(), entry);
    if (entries.size() > 100) entries.resize(100);
    save_history(entries);
}

}  // namespace fc
