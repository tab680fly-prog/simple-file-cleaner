// Simple File Cleaner for Windows — command-line edition.
//
// This is a from-scratch Windows port of the scanning/cleaning logic in the
// GTK/Libadwaita Linux app (../src). It targets Windows-specific junk
// locations (Temp, Recycle Bin, thumbnail cache, browser caches, WER crash
// dumps, ...) instead of the Linux ones, and ships as a console executable
// rather than a GNOME GUI.

#include <algorithm>
#include <iostream>
#include <sstream>

#include "common.hpp"
#include "delete_engine.hpp"
#include "scan.hpp"
#include "settings.hpp"

namespace {

using namespace fc;

void print_usage(const char *argv0) {
    std::cout <<
        "Simple File Cleaner (Windows CLI)\n\n"
        "Usage:\n"
        "  " << argv0 << " scan [--deep] [--json]\n"
        "  " << argv0 << " clean [--deep] [--yes]\n"
        "  " << argv0 << " folder <path> [--yes]\n"
        "  " << argv0 << " categories\n\n"
        "Commands:\n"
        "  scan        Dry run: list what would be cleaned, no deletion.\n"
        "  clean       Scan, then delete everything found (asks to confirm\n"
        "              unless --yes is given).\n"
        "  folder <p>  Scan a single directory for junk subfolders and large\n"
        "              files, same as the GUI's \"Scan Folder\" action.\n"
        "  categories  List scan categories and whether each is enabled.\n\n"
        "Options:\n"
        "  --deep      Also walk the whole user profile for node_modules,\n"
        "              __pycache__, build outputs, etc. (scan/clean only).\n"
        "  --yes       Don't ask for confirmation before deleting.\n"
        "  --json      Print scan results as JSON instead of text.\n";
}

std::vector<Category> run_scan(const Settings &settings, bool deep, CancelToken &cancel) {
    std::vector<Category> categories;
    auto emit = [&](Category &&c) { categories.push_back(std::move(c)); };
    auto progress = [](const std::string &line) {
        std::cerr << "\r\x1b[K" << line.substr(0, 100) << std::flush;
    };
    if (deep)
        deep_scan_generator(settings, progress, emit, cancel);
    else
        scan_generator(settings, progress, emit, cancel);
    std::cerr << "\r\x1b[K" << std::flush;
    return categories;
}

void print_categories_text(const std::vector<Category> &categories) {
    std::uint64_t grand_total = 0;
    for (const auto &cat : categories) {
        std::uint64_t total = cat.total_size();
        grand_total += total;
        std::cout << cat.title << "  (" << fmt_size(total) << ")\n";
        if (!cat.subtitle.empty()) std::cout << "  " << cat.subtitle << "\n";
        for (const auto &entry : cat.entries) {
            std::string shown = entry.path.string() == fc::kRecycleBinMarker
                                     ? std::string(fc::kRecycleBinMarker)
                                     : display_path(entry.path);
            std::cout << "    " << fmt_size(entry.size) << "\t" << shown << "\n";
        }
        std::cout << "\n";
    }
    std::cout << "Total reclaimable: " << fmt_size(grand_total) << " across " << categories.size()
               << " categories\n";
}

std::string json_escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\')
            out += '\\', out += c;
        else if (c == '\n')
            out += "\\n";
        else
            out += c;
    }
    return out;
}

void print_categories_json(const std::vector<Category> &categories) {
    std::cout << "{\n  \"categories\": [\n";
    for (std::size_t i = 0; i < categories.size(); ++i) {
        const auto &cat = categories[i];
        std::cout << "    {\n"
                   << "      \"key\": \"" << json_escape(cat.key) << "\",\n"
                   << "      \"title\": \"" << json_escape(cat.title) << "\",\n"
                   << "      \"total_bytes\": " << cat.total_size() << ",\n"
                   << "      \"entries\": [\n";
        for (std::size_t j = 0; j < cat.entries.size(); ++j) {
            const auto &e = cat.entries[j];
            std::string path_str =
                e.path.string() == fc::kRecycleBinMarker ? std::string(fc::kRecycleBinMarker) : e.path.string();
            std::cout << "        {\"path\": \"" << json_escape(path_str) << "\", \"bytes\": " << e.size
                       << "}" << (j + 1 < cat.entries.size() ? "," : "") << "\n";
        }
        std::cout << "      ]\n    }" << (i + 1 < categories.size() ? "," : "") << "\n";
    }
    std::cout << "  ]\n}\n";
}

bool confirm(const std::string &prompt) {
    std::cout << prompt << " [y/N] " << std::flush;
    std::string line;
    std::getline(std::cin, line);
    return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
}

int do_clean(const std::vector<Category> &categories, bool assume_yes) {
    std::uint64_t total = 0;
    int n_paths = 0;
    for (const auto &cat : categories) {
        total += cat.total_size();
        n_paths += (int)cat.entries.size();
    }
    if (categories.empty()) {
        std::cout << "Nothing found to clean.\n";
        return 0;
    }
    print_categories_text(categories);
    if (!assume_yes && !confirm("Delete all " + std::to_string(n_paths) + " items above (" +
                                 fmt_size(total) + ")?")) {
        std::cout << "Aborted.\n";
        return 1;
    }

    int deleted = 0;
    std::uint64_t deleted_bytes = 0;
    std::vector<std::string> errors;
    std::vector<fs::path> permission_failed;

    for (const auto &cat : categories) {
        if (cat.key == "recycle_bin") {
            if (empty_recycle_bin()) {
                deleted += (int)cat.entries.size();
                deleted_bytes += cat.total_size();
            } else {
                errors.push_back("Recycle Bin: failed to empty");
            }
            continue;
        }
        std::vector<fs::path> paths;
        for (const auto &e : cat.entries) paths.push_back(e.path);
        auto result = delete_direct(paths);
        deleted += result.deleted_count;
        for (const auto &e : cat.entries) {
            bool ok = std::find(result.deleted_paths.begin(), result.deleted_paths.end(), e.path) !=
                      result.deleted_paths.end();
            if (ok) deleted_bytes += e.size;
        }
        errors.insert(errors.end(), result.errors.begin(), result.errors.end());
        permission_failed.insert(permission_failed.end(), result.permission_failed.begin(),
                                  result.permission_failed.end());
    }

    std::cout << "\nDeleted " << deleted << " item(s), freed approximately " << fmt_size(deleted_bytes)
               << ".\n";
    if (!permission_failed.empty()) {
        std::cout << permission_failed.size()
                  << " item(s) could not be deleted due to permissions. Re-run this tool from an "
                     "elevated (\"Run as administrator\") prompt to retry:\n";
        for (const auto &p : permission_failed) std::cout << "  " << p.string() << "\n";
    }
    for (const auto &e : errors) std::cout << "Error: " << e << "\n";
    return errors.empty() && permission_failed.empty() ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    std::string cmd = args[0];
    bool deep = std::find(args.begin(), args.end(), "--deep") != args.end();
    bool assume_yes = std::find(args.begin(), args.end(), "--yes") != args.end();
    bool as_json = std::find(args.begin(), args.end(), "--json") != args.end();

    Settings settings = Settings::load();
    CancelToken cancel;

    if (cmd == "scan") {
        auto categories = run_scan(settings, deep, cancel);
        if (as_json)
            print_categories_json(categories);
        else
            print_categories_text(categories);
        return 0;
    }

    if (cmd == "clean") {
        auto categories = run_scan(settings, deep, cancel);
        return do_clean(categories, assume_yes);
    }

    if (cmd == "folder") {
        if (args.size() < 2) {
            std::cerr << "folder: missing <path> argument\n";
            return 1;
        }
        fs::path root(args[1]);
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
            std::cerr << "folder: not a directory: " << root.string() << "\n";
            return 1;
        }
        std::vector<Category> categories;
        auto emit = [&](Category &&c) { categories.push_back(std::move(c)); };
        auto progress = [](const std::string &line) { std::cerr << "\r\x1b[K" << line.substr(0, 100) << std::flush; };
        folder_scan_generator(root, settings, progress, emit, cancel);
        std::cerr << "\r\x1b[K" << std::flush;
        if (as_json) {
            print_categories_json(categories);
            return 0;
        }
        return do_clean(categories, assume_yes);
    }

    if (cmd == "categories") {
        for (const auto &[key, label] : SCAN_CATEGORIES)
            std::cout << (settings.cat_on(key) ? "[x] " : "[ ] ") << key << "  " << label << "\n";
        return 0;
    }

    print_usage(argv[0]);
    return 1;
}
