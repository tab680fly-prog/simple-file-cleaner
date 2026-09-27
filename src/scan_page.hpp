#pragma once

#include <gtk/gtk.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "scan_ring.hpp"

namespace fc {

// The full-page scan indicator shown while a scan runs: the animated
// ScanRing plus a title, a live "currently examining" path label, a
// "N categories found" pill that appears once results start arriving, and a
// Cancel button.
class ScanPage {
   public:
    explicit ScanPage(const std::string &animation_style);

    GtkWidget *widget() const { return root_; }

    void refresh_style(const std::string &animation_style);
    void start(const std::string &label, bool cancellable = true);
    void stop();
    void set_path(const std::string &path);
    void set_found(int n, std::uint64_t total);
    void set_done(int n, std::uint64_t total);

    void set_title(const std::string &text);
    void set_path_label(const std::string &text);

    void set_on_cancel(std::function<void()> cb) { on_cancel_ = std::move(cb); }

   private:
    GtkWidget *root_;
    GtkWidget *title_;
    GtkWidget *path_label_;
    GtkWidget *found_label_;
    GtkWidget *cancel_btn_;
    std::unique_ptr<ScanRing> ring_;
    std::function<void()> on_cancel_;
};

}  // namespace fc
