#pragma once

#include <string>
#include <vector>

#include "common.hpp"

namespace fc {

struct DeleteResult {
    int deleted_count = 0;
    std::vector<fs::path> deleted_paths;
    std::vector<std::string> errors;
    // Paths that failed specifically because of a permission error — the
    // caller can suggest re-running the tool from an elevated ("Run as
    // administrator") prompt and retrying just these.
    std::vector<fs::path> permission_failed;
};

// Deletes paths directly (no privilege escalation). A permission error on
// one path does not abort the whole batch: every path is attempted, and
// paths that fail due to permissions are reported separately.
DeleteResult delete_direct(const std::vector<fs::path> &paths);

// Empties the Recycle Bin. Used instead of delete_direct() for the
// "recycle_bin" category, whose FileEntry is a pseudo-path
// (kRecycleBinMarker) rather than a real filesystem path.
bool empty_recycle_bin();

}  // namespace fc
