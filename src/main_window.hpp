#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>

#include <atomic>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common.hpp"
#include "scan.hpp"
#include "scan_page.hpp"
#include "settings.hpp"

namespace fc {

// Top-level application window: start page with the three scan modes,
// scan-in-progress page, results list (grouped by category, with per-file
// rows), and the delete-selected workflow (confirmation, pkexec elevation,
// history recording, optional automatic rescan).
class FileCleanerWindow {
   public:
    FileCleanerWindow(AdwApplication *app, Settings settings);

    GtkWidget *widget() const { return window_; }

    // Shows the one-time "protect important folders" notice if it has not
    // been acknowledged yet.
    void maybe_show_first_run_warning();

   private:
    GtkWidget *window_;
    Settings settings_;
    std::deque<Category> categories_;
    bool scanning_ = false;
    bool deleting_ = false;
    std::string scan_mode_ = "quick";
    std::optional<fs::path> scan_root_;
    bool post_delete_ = false;
    std::string last_history_id_;
    int scan_gen_ = 0;
    std::shared_ptr<CancelToken> current_cancel_;
    // Suppresses per-row refresh_delete_btn() calls while many checkboxes
    // are being changed programmatically at once.
    bool bulk_update_ = false;

    // Set to false the moment this window starts being destroyed, so any
    // g_idle_add callback still in flight from a background scan/delete
    // thread can detect that `this` is no longer safe to touch instead of
    // dereferencing a freed pointer.
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    // widgets
    GtkWidget *toast_overlay_ = nullptr;
    GtkWidget *toolbar_view_ = nullptr;
    GtkWidget *back_btn_ = nullptr;
    GtkWidget *stack_ = nullptr;
    GtkWidget *list_box_outer_ = nullptr;
    GtkWidget *scroll_ = nullptr;
    GtkWidget *scan_btn_ = nullptr;
    GtkWidget *delete_btn_ = nullptr;
    GtkWidget *status_label_ = nullptr;
    GtkWidget *clean_page_ = nullptr;
    std::unique_ptr<ScanPage> scan_page_;
    // Every visible result row's checkbox, paired with the entry it controls.
    std::vector<std::pair<FileEntry *, GtkWidget *>> row_checks_;

    void build_ui(AdwApplication *app);
    GtkWidget *build_start_page();
    void install_actions(AdwApplication *app);
    void show_page(const char *name);

    void choose_folder_and_scan();
    void start_scan(const std::string &mode, std::optional<fs::path> root);
    void cancel_scan();
    void on_category_found(Category cat, int gen);
    void scan_done(int gen, bool cancelled);
    void show_results(bool animate = true);
    void show_clean_page();

    void clear_results_list();
    void add_category_group(Category &cat, bool animate);
    void select_all_in_cat(Category &cat, bool selected);
    void set_all_selected(bool selected);
    void on_file_toggle(FileEntry &entry, bool active);
    void refresh_delete_btn();
    void on_scroll_changed(GtkAdjustment *adj);
    // Drops result entries that are now excluded (or no longer exist) and
    // re-renders. Returns true if anything was removed.
    bool prune_results(bool drop_missing);

    void toast(const std::string &text);
    void open_settings(const char *page = nullptr);
    void show_about();
    void on_delete_clicked();
    void start_delete(std::vector<std::pair<fs::path, std::uint64_t>> items, std::vector<fs::path> companions);
    void delete_done(int deleted_count, std::uint64_t deleted_bytes, std::vector<std::string> errors);

    static void on_destroy(GtkWidget *widget, gpointer user_data);
};

}  // namespace fc
