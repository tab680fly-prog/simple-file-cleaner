#pragma once

#include <gtk/gtk.h>

#include <cstdint>
#include <memory>
#include <string>

#include "scan_ring.hpp"

namespace fc {

// The full-page scan indicator shown while a scan runs: the animated
// ScanRing plus a title, a live "currently inspecting" path label, and a
// "found N categories" pill that appears once results start arriving.
class ScanPage {
   public:
    explicit ScanPage(const std::string &animation_style);

    GtkWidget *widget() const { return root_; }

    void refresh_style(const std::string &animation_style);
    void start(const std::string &label);
    void stop();
    void set_path(const std::string &path);
    void set_found(int n, std::uint64_t total);
    void set_done(int n, std::uint64_t total);

    void set_title(const std::string &text);
    void set_path_label(const std::string &text);

   private:
    GtkWidget *root_;
    GtkWidget *title_;
    GtkWidget *path_label_;
    GtkWidget *found_label_;
    std::unique_ptr<ScanRing> ring_;
};

}  // namespace fc
