#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common.hpp"
#include "settings.hpp"

namespace fc {

// Shared by a scan worker thread and its owner: when a new scan supersedes
// an in-flight one, the owner flips this so the old walk actually stops
// instead of continuing to burn CPU/disk IO in the background producing
// results nobody will look at (the original Python implementation had no
// such mechanism — a superseded scan ran to completion regardless).
struct CancelToken {
    std::atomic<bool> cancelled{false};
    bool is_cancelled() const { return cancelled.load(std::memory_order_relaxed); }
};

using ProgressCb = std::function<void(const std::string &)>;
using CategoryCb = std::function<void(Category &&)>;

// Recursively sums the apparent size of a file/directory tree. Symlinks are
// not followed and not counted, matching the original scanner's semantics.
// Native traversal — no `du`/`timeout` subprocess spawned per call, which
// is the dominant cost of the original scanner when sizing many small
// directories (trash items, node_modules hits, etc).
std::uint64_t dir_size(const fs::path &p, const ProgressCb &progress, CancelToken &cancel);

// Sizes many independent paths in parallel using a small worker pool.
std::map<fs::path, std::uint64_t> dir_size_many(const std::vector<fs::path> &paths,
                                                 const ProgressCb &progress, CancelToken &cancel);

// Emits Category objects (via emit) for the standard "quick scan" target
// set: trash, thumbnail cache, browser caches, flatpak storage, package
// manager caches, logs, and stale downloads — plus any user-configured
// custom scan paths.
void scan_generator(const Settings &settings, const ProgressCb &progress, const CategoryCb &emit,
                     CancelToken &cancel);

// Runs the quick scan, then walks the whole home directory looking for
// junk directories (node_modules, __pycache__, build outputs, ...) and
// stale bytecode files.
void deep_scan_generator(const Settings &settings, const ProgressCb &progress,
                          const CategoryCb &emit, CancelToken &cancel);

// Walks a single user-chosen directory for junk directories/files plus any
// large (>=50MB) top-level files.
void folder_scan_generator(const fs::path &root, const Settings &settings,
                            const ProgressCb &progress, const CategoryCb &emit,
                            CancelToken &cancel);

}  // namespace fc
