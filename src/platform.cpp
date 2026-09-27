#include "platform.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <sddl.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fc {

// ---------------------------------------------------------------------------
// Strings and keys
// ---------------------------------------------------------------------------

std::string path_str(const fs::path &p) {
    auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

fs::path path_from(const std::string &utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

static constexpr char SEP = IS_WINDOWS ? '\\' : '/';

static void strip_trailing_sep(std::string &s) {
    // Keep the separator of a root ("/" or "c:\").
    while (s.size() > 1 && s.back() == SEP && !(IS_WINDOWS && s.size() == 3 && s[1] == ':')) s.pop_back();
}

std::string path_key(const fs::path &p) {
    fs::path n = p.lexically_normal();
#ifdef _WIN32
    n.make_preferred();
    std::wstring w = n.wstring();
    if (!w.empty()) CharLowerBuffW(w.data(), static_cast<DWORD>(w.size()));
    std::string s = path_str(fs::path(w));
#else
    std::string s = n.string();
#endif
    strip_trailing_sep(s);
    return s;
}

bool key_is_under(const std::string &key, const std::string &ancestor) {
    if (ancestor.empty()) return false;
    if (key == ancestor) return true;
    if (key.size() <= ancestor.size() || key.compare(0, ancestor.size(), ancestor) != 0) return false;
    return ancestor.back() == SEP || key[ancestor.size()] == SEP;
}

// ---------------------------------------------------------------------------
// Well-known locations
// ---------------------------------------------------------------------------

fs::path env_path(const char *name) {
#ifdef _WIN32
    std::wstring wname(name, name + std::strlen(name));
    if (const wchar_t *v = _wgetenv(wname.c_str()); v && *v) return fs::path(v);
#else
    if (const char *v = std::getenv(name); v && *v) return fs::path(v);
#endif
    return {};
}

fs::path home_dir() {
#ifdef _WIN32
    fs::path h = env_path("USERPROFILE");
    return h.empty() ? fs::path(L"C:\\") : h;
#else
    fs::path h = env_path("HOME");
    return h.empty() ? fs::path("/") : h;
#endif
}

fs::path settings_file() {
#ifdef _WIN32
    fs::path base = env_path("APPDATA");
    if (base.empty()) base = home_dir() / "AppData" / "Roaming";
    return base / "FileCleaner" / "settings.json";
#else
    return home_dir() / ".config" / "filecleaner.json";
#endif
}

fs::path history_file() {
#ifdef _WIN32
    fs::path base = env_path("LOCALAPPDATA");
    if (base.empty()) base = home_dir() / "AppData" / "Local";
    return base / "FileCleaner" / "history.json";
#else
    return home_dir() / ".local" / "share" / "filecleaner" / "history.json";
#endif
}

fs::path downloads_dir() {
#ifdef _WIN32
    PWSTR raw = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &raw)) && raw) result = fs::path(raw);
    if (raw) CoTaskMemFree(raw);
    if (!result.empty()) return result;
#endif
    return home_dir() / "Downloads";
}

std::string expand_user_path(const std::string &raw) {
    std::size_t a = raw.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    std::size_t b = raw.find_last_not_of(" \t\r\n");
    std::string s = raw.substr(a, b - a + 1);

    if (s == "~" || s.rfind("~/", 0) == 0 || (IS_WINDOWS && s.rfind("~\\", 0) == 0))
        s = path_str(home_dir()) + s.substr(1);

#ifdef _WIN32
    std::wstring w = path_from(s).wstring();
    DWORD n = ExpandEnvironmentStringsW(w.c_str(), nullptr, 0);
    if (n > 0) {
        std::wstring out(n, L'\0');
        if (ExpandEnvironmentStringsW(w.c_str(), out.data(), n) > 0) {
            out.resize(wcslen(out.c_str()));
            w = out;
        }
    }
    fs::path p = fs::path(w).lexically_normal();
    p.make_preferred();
    s = path_str(p);
#else
    s = path_str(path_from(s).lexically_normal());
#endif
    strip_trailing_sep(s);
    return s;
}

const std::vector<std::string> &builtin_protected_keys() {
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> out;
#ifdef _WIN32
        auto add = [&](const fs::path &p) {
            if (p.empty()) return;
            std::string k = path_key(p);
            if (std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
        };
        // C:\Windows is always protected, as is the real Windows folder if
        // Windows is installed elsewhere.
        add(fs::path(L"C:\\Windows"));
        wchar_t windir[MAX_PATH];
        if (UINT n = GetWindowsDirectoryW(windir, MAX_PATH); n > 0 && n < MAX_PATH) add(fs::path(windir));
        add(env_path("SystemRoot"));
        add(env_path("windir"));
        add(env_path("ProgramFiles"));
        add(env_path("ProgramFiles(x86)"));
        add(env_path("ProgramW6432"));
        add(env_path("ProgramData"));
        fs::path sys_drive = env_path("SystemDrive");
        if (sys_drive.empty()) sys_drive = fs::path(L"C:");
        add(sys_drive / L"\\Program Files");
        add(sys_drive / L"\\Program Files (x86)");
        add(sys_drive / L"\\ProgramData");
        add(sys_drive / L"\\System Volume Information");
        add(sys_drive / L"\\Recovery");
        add(sys_drive / L"\\Boot");
#endif
        return out;
    }();
    return keys;
}

// ---------------------------------------------------------------------------
// File queries
// ---------------------------------------------------------------------------

bool is_writable(const fs::path &p) {
#ifdef _WIN32
    return _waccess(p.c_str(), 2) == 0;
#else
    return ::access(p.c_str(), W_OK) == 0;
#endif
}

std::optional<std::chrono::system_clock::time_point> last_access_time(const fs::path &p) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &data)) return std::nullopt;
    ULARGE_INTEGER t;
    t.LowPart = data.ftLastAccessTime.dwLowDateTime;
    t.HighPart = data.ftLastAccessTime.dwHighDateTime;
    // FILETIME counts 100 ns intervals since 1601-01-01.
    const long long unix_100ns = static_cast<long long>(t.QuadPart) - 116444736000000000LL;
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(unix_100ns * 100)));
#else
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) return std::nullopt;
    return std::chrono::system_clock::from_time_t(st.st_atime);
#endif
}

bool is_link(const fs::path &p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    std::error_code ec;
    return fs::is_symlink(fs::symlink_status(p, ec));
#endif
}

bool is_link(const fs::directory_entry &e) {
#ifdef _WIN32
    return is_link(e.path());
#else
    std::error_code ec;
    return fs::is_symlink(e.symlink_status(ec));
#endif
}

// ---------------------------------------------------------------------------
// Deletion
// ---------------------------------------------------------------------------

#ifdef _WIN32
namespace {

std::error_code win_error(DWORD err) {
    switch (err) {
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
            return std::make_error_code(std::errc::permission_denied);
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return std::make_error_code(std::errc::device_or_resource_busy);
        case ERROR_DIR_NOT_EMPTY:
            return std::make_error_code(std::errc::directory_not_empty);
        default:
            return std::error_code(static_cast<int>(err), std::system_category());
    }
}

// "\\?\" form, which lifts the 260-character path limit that deep
// node_modules trees routinely exceed.
std::wstring long_path(const fs::path &p) {
    std::wstring w = fs::absolute(p).lexically_normal().make_preferred().wstring();
    if (w.rfind(L"\\\\?\\", 0) == 0) return w;
    if (w.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + w.substr(2);
    return L"\\\\?\\" + w;
}

void remove_rec(const std::wstring &p, std::error_code &first_error) {
    DWORD attrs = GetFileAttributesW(p.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND && !first_error) first_error = win_error(err);
        return;
    }
    if (attrs & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);

    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        // A junction or directory symlink: remove only the link itself.
        if (!(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileExW((p + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                        FIND_FIRST_EX_LARGE_FETCH);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                    remove_rec(p + L"\\" + fd.cFileName, first_error);
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        if (!RemoveDirectoryW(p.c_str()) && !first_error) first_error = win_error(GetLastError());
    } else {
        if (!DeleteFileW(p.c_str()) && !first_error) first_error = win_error(GetLastError());
    }
}

}  // namespace
#endif

void remove_tree(const fs::path &p, std::error_code &ec) {
    ec.clear();
#ifdef _WIN32
    remove_rec(long_path(p), ec);
#else
    fs::remove_all(p, ec);
#endif
}

bool running_as_admin() {
#ifdef _WIN32
    return IsUserAnAdmin();
#else
    return geteuid() == 0;
#endif
}

// ---------------------------------------------------------------------------
// Trash
// ---------------------------------------------------------------------------

std::vector<fs::path> trash_dirs() {
#ifdef _WIN32
    std::vector<fs::path> out;
    std::wstring sid;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD len = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &len);
        std::vector<unsigned char> buf(len);
        if (len && GetTokenInformation(token, TokenUser, buf.data(), len, &len)) {
            LPWSTR str = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buf.data())->User.Sid, &str)) {
                sid = str;
                LocalFree(str);
            }
        }
        CloseHandle(token);
    }
    if (sid.empty()) return out;

    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        fs::path bin = fs::path(root) / L"$Recycle.Bin" / sid;
        std::error_code ec;
        if (fs::is_directory(bin, ec)) out.push_back(bin);
    }
    return out;
#else
    return {home_dir() / ".local/share/Trash/files", home_dir() / ".local/share/Trash/info"};
#endif
}

}  // namespace fc
