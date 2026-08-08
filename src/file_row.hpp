#pragma once

#include <gtk/gtk.h>

#include <functional>

#include "common.hpp"

namespace fc {

// Wraps a single AdwActionRow representing one FileEntry inside a scan
// results category: checkbox, icon, size, and an "open containing folder"
// button.
class FileRow {
   public:
    using ToggleCb = std::function<void(FileEntry &, bool)>;

    FileRow(FileEntry &entry, ToggleCb on_toggle);

    GtkWidget *widget() const { return row_; }

   private:
    GtkWidget *row_;
};

}  // namespace fc
