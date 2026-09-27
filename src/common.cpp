#include "common.hpp"

#include <cstdio>

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

std::string display_path(const fs::path &p) {
    const std::string home = expand_user_path(path_str(home_dir()));
    const std::string sp = path_str(p);
    if (!home.empty() && key_is_under(path_key(p), path_key(path_from(home))) && sp.size() >= home.size())
        return "~" + sp.substr(home.size());
    return sp;
}

bool is_protected_path(const fs::path &p) {
    const std::string expanded = expand_user_path(path_str(p));
    if (expanded.empty()) return true;
    const fs::path ep = path_from(expanded);
    if (!ep.is_absolute()) return true;

    // Filesystem roots and top-level directories such as /usr, /home,
    // C:\Users or C:\Windows.
    int depth = 0;
    for (const auto &part : ep.relative_path()) {
        if (!part.empty()) ++depth;
    }
    if (depth <= 1) return true;

    // The home directory itself or any of its ancestors.
    const std::string key = path_key(ep);
    if (key_is_under(path_key(home_dir()), key)) return true;

    for (const auto &b : builtin_protected_keys())
        if (key_is_under(key, b)) return true;
    return false;
}

}  // namespace fc
