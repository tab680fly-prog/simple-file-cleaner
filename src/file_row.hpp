#pragma once

#include <gtk/gtk.h>

#include <functional>

#include "common.hpp"

namespace fc {

// Wraps a single AdwActionRow representing one FileEntry inside a scan
// results category: checkbox, icon, size, and a "show in file manager"
// button. Clicking anywhere on the row toggles the checkbox.
class FileRow {
   public:
    using ToggleCb = std::function<void(FileEntry &, bool)>;

    FileRow(FileEntry &entry, ToggleCb on_toggle);

    GtkWidget *widget() const { return row_; }
    GtkWidget *check() const { return check_; }

   private:
    GtkWidget *row_;
    GtkWidget *check_;
};

}  // namespace fc
