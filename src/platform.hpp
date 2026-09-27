#pragma once

// Everything that differs between Linux and Windows lives behind this
// header, so the scanner, settings and UI code stay platform-neutral.

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace fc {

namespace fs = std::filesystem;

#ifdef _WIN32
constexpr bool IS_WINDOWS = true;
#else
constexpr bool IS_WINDOWS = false;
#endif

// UTF-8 conversions. On Windows, fs::path::string() uses the ANSI code page
// and mangles non-ASCII names, while GTK and the settings files use UTF-8.
std::string path_str(const fs::path &p);
fs::path path_from(const std::string &utf8);

// Normalized form used for comparing paths: lexically normalized, no
// trailing separator, and on Windows lower-cased with "\" separators
// (Windows paths are case-insensitive).
std::string path_key(const fs::path &p);

// True if `key` equals `ancestor_key` or lies inside it (both from path_key).
bool key_is_under(const std::string &key, const std::string &ancestor_key);

// Environment variable as a path (UTF-8 safe), or empty if unset.
fs::path env_path(const char *name);

fs::path home_dir();
fs::path settings_file();
fs::path history_file();
fs::path downloads_dir();

// Expands "~" (and on Windows, %VARIABLES%) and normalizes. Returns UTF-8.
std::string expand_user_path(const std::string &raw);

// Directory trees that are always protected: never scanned, never deleted,
// regardless of settings. On Windows this includes C:\Windows (and the
// actual %SystemRoot%), Program Files and ProgramData.
const std::vector<std::string> &builtin_protected_keys();

bool is_writable(const fs::path &p);
std::optional<std::chrono::system_clock::time_point> last_access_time(const fs::path &p);

// True for symlinks and, on Windows, junctions and other reparse points.
// These are never followed or sized, and deleting one removes only the
// link itself, never its target.
bool is_link(const fs::directory_entry &e);
bool is_link(const fs::path &p);

// Recursively deletes p without following links. On Windows it also clears
// the read-only attribute, which otherwise makes deletion of many
// development folders (e.g. node_modules, .git objects) fail.
void remove_tree(const fs::path &p, std::error_code &ec);

bool running_as_admin();

void open_in_file_manager(const fs::path &p);

// Recycle Bin / Trash folders that belong to the current user.
std::vector<fs::path> trash_dirs();

}  // namespace fc
