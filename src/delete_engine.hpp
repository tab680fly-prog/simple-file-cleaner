#pragma once

#include <string>
#include <vector>

#include "common.hpp"

namespace fc {

struct DeleteResult {
    int deleted_count = 0;
    std::vector<std::string> errors;
    // Paths that failed specifically because of a permission error — good
    // candidates for a pkexec-elevated retry. Kept separate from `errors`
    // so a caller retrying only these (rather than the whole original
    // list) doesn't re-attempt paths that already succeeded.
    std::vector<fs::path> permission_failed;
};

// Deletes paths directly (no privilege escalation). Unlike the original
// Python implementation, a permission error on one path does not abort the
// whole batch and discard the count/errors accumulated so far — every path
// is attempted, and paths that fail due to permissions are reported
// separately so the caller can retry just those via delete_with_pkexec.
DeleteResult delete_direct(const std::vector<fs::path> &paths);

// Retries deletion of `paths` via `pkexec`, prompting for authentication.
DeleteResult delete_with_pkexec(const std::vector<fs::path> &paths);

}  // namespace fc
