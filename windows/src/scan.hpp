#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "common.hpp"
#include "settings.hpp"

namespace fc {

// Shared by a scan worker thread and its owner: when a new scan supersedes
// an in-flight one, the owner flips this so the old walk actually stops.
struct CancelToken {
    std::atomic<bool> cancelled{false};
    bool is_cancelled() const { return cancelled.load(std::memory_order_relaxed); }
};

using ProgressCb = std::function<void(const std::string &)>;
using CategoryCb = std::function<void(Category &&)>;

// Pseudo-path used as the single FileEntry for the "recycle_bin" category.
// The Recycle Bin isn't a plain folder, so it can't be sized/deleted with
// std::filesystem; callers must special-case this key (see delete_engine.hpp
// empty_recycle_bin()).
constexpr const char *kRecycleBinMarker = "RecycleBin";

// Recursively sums the apparent size of a file/directory tree.
std::uint64_t dir_size(const fs::path &p, const ProgressCb &progress, CancelToken &cancel);

// Sizes many independent paths in parallel using a small worker pool.
std::map<fs::path, std::uint64_t> dir_size_many(const std::vector<fs::path> &paths,
                                                 const ProgressCb &progress, CancelToken &cancel);

// Emits Category objects for the standard "quick scan" target set: Recycle
// Bin, Temp folders, thumbnail/icon cache, browser caches, package manager
// caches, crash dumps, and stale Downloads — plus any user-configured
// custom scan paths.
void scan_generator(const Settings &settings, const ProgressCb &progress, const CategoryCb &emit,
                     CancelToken &cancel);

// Runs the quick scan, then walks the whole user profile looking for junk
// directories (node_modules, __pycache__, build outputs, ...) and stale
// bytecode files.
void deep_scan_generator(const Settings &settings, const ProgressCb &progress,
                          const CategoryCb &emit, CancelToken &cancel);

// Walks a single user-chosen directory for junk directories/files plus any
// large (>=50MB) top-level files.
void folder_scan_generator(const fs::path &root, const Settings &settings,
                            const ProgressCb &progress, const CategoryCb &emit,
                            CancelToken &cancel);

}  // namespace fc
