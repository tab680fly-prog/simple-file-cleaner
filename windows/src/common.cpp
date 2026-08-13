#include "common.hpp"

#include <cstdio>
#include <cstdlib>

namespace fc {

std::string fmt_size(std::uint64_t n) {
    char buf[64];
    if (n >= (1ull << 30)) {
        std::snprintf(buf, sizeof(buf), "%.1f GB", n / double(1ull << 30));
    } else if (n >= (1ull << 20)) {
        std::snprintf(buf, sizeof(buf), "%.0f MB", n / double(1ull << 20));
    } else if (n >= (1ull << 10)) {
        std::snprintf(buf, sizeof(buf), "%.0f KB", n / double(1ull << 10));
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)n);
    }
    return buf;
}

fs::path home_dir() {
    if (const char *p = std::getenv("USERPROFILE")) return fs::path(p);
    const char *drive = std::getenv("HOMEDRIVE");
    const char *path = std::getenv("HOMEPATH");
    if (drive && path) return fs::path(std::string(drive) + path);
    return fs::path(".");
}

std::string display_path(const fs::path &p) {
    std::string sp = p.string();
    std::string home = home_dir().string();
    if (!home.empty() && sp.size() >= home.size() && sp.compare(0, home.size(), home) == 0)
        return "%USERPROFILE%" + sp.substr(home.size());
    return sp;
}

}  // namespace fc
