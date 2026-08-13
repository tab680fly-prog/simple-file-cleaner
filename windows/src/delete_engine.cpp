#include "delete_engine.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shellapi.h>

namespace fc {

DeleteResult delete_direct(const std::vector<fs::path> &paths) {
    DeleteResult result;
    for (const auto &p : paths) {
        std::error_code ec;
        fs::remove_all(p, ec);
        if (!ec) {
            result.deleted_count++;
            result.deleted_paths.push_back(p);
        } else if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted) {
            result.permission_failed.push_back(p);
        } else {
            result.errors.push_back(p.filename().string() + ": " + ec.message());
        }
    }
    return result;
}

bool empty_recycle_bin() {
    HRESULT hr = SHEmptyRecycleBinW(nullptr, nullptr,
                                     SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
    // S_FALSE / ERROR_FILE_NOT_FOUND (as HRESULT) mean the bin was already empty.
    return hr == S_OK || hr == S_FALSE || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

}  // namespace fc
