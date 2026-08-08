#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>

#include <atomic>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common.hpp"
#include "scan.hpp"
#include "scan_page.hpp"
#include "settings.hpp"

namespace fc {

// Top-level application window: scan mode menu, scan-in-progress page,
// results list (grouped by category, with per-file rows), and the
// delete-selected workflow (confirmation, pkexec elevation, history
// recording, optional auto-rescan).
class FileCleanerWindow {
   public:
    FileCleanerWindow(AdwApplication *app, Settings settings);

    GtkWidget *widget() const { return window_; }

   private:
    GtkWidget *window_;
    Settings settings_;
    std::deque<Category> categories_;
    bool scanning_ = false;
    std::string scan_mode_ = "quick";
    std::optional<fs::path> scan_root_;
    bool post_delete_ = false;
    std::string last_history_id_;
    int scan_gen_ = 0;
    std::shared_ptr<CancelToken> current_cancel_;

    // Set to false the moment this window starts being destroyed, so any
    // g_idle_add callback still in flight from a background scan/delete
    // thread can detect that `this` is no longer safe to touch instead of
    // dereferencing a freed pointer.
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    // widgets
    GtkWidget *stack_ = nullptr;
    GtkWidget *list_box_outer_ = nullptr;
    GtkWidget *scan_btn_ = nullptr;
    GtkWidget *delete_btn_ = nullptr;
    GtkWidget *status_label_ = nullptr;
    std::unique_ptr<ScanPage> scan_page_;

    void build_ui(AdwApplication *app);
    void install_actions();

    void start_scan(const std::string &mode, std::optional<fs::path> root);
    void scan_worker(std::string mode, std::optional<fs::path> root, int gen,
                      std::shared_ptr<CancelToken> cancel);
    void on_category_found(Category cat, std::uint64_t running_total, int gen);
    void scan_done(int gen, std::uint64_t total_found);
    void show_results();

    void clear_results_list();
    void add_category_group(Category &cat, bool animate);
    void reveal_group(GtkWidget *revealer);
    void select_all_in_cat(Category &cat, bool selected);
    void set_all_selected(bool selected);
    void on_file_toggle(FileEntry &entry, bool active);
    void refresh_delete_btn();
    void on_scroll_changed(GtkAdjustment *adj);

    void open_settings();
    void on_delete_clicked();
    void start_delete(std::vector<fs::path> paths, std::uint64_t total);
    void delete_worker(std::vector<fs::path> paths, std::uint64_t total);
    void delete_done(int deleted_count, std::vector<std::string> errors, std::uint64_t deleted_total);

    static void on_destroy(GtkWidget *widget, gpointer user_data);
};

}  // namespace fc
