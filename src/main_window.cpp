#include "main_window.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "delete_engine.hpp"
#include "file_row.hpp"
#include "history.hpp"
#include "settings_window.hpp"

namespace fc {

namespace {

constexpr const char *APP_VERSION = "1.3.0";

// Categories with more entries than this are collapsed into an expander
// row so a deep scan with hundreds of hits stays readable.
constexpr std::size_t COLLAPSE_THRESHOLD = 6;

struct MainCtx {
    std::shared_ptr<std::atomic<bool>> alive;
    std::function<void()> fn;
};

gboolean main_ctx_trampoline(gpointer data) {
    auto *c = static_cast<MainCtx *>(data);
    if (c->alive->load()) c->fn();
    return G_SOURCE_REMOVE;
}

void main_ctx_free(gpointer data) { delete static_cast<MainCtx *>(data); }

void run_on_main(const std::shared_ptr<std::atomic<bool>> &alive, std::function<void()> fn) {
    auto *ctx = new MainCtx{alive, std::move(fn)};
    g_idle_add_full(G_PRIORITY_DEFAULT, main_ctx_trampoline, ctx, main_ctx_free);
}

void run_on_main_delayed(const std::shared_ptr<std::atomic<bool>> &alive, unsigned ms, std::function<void()> fn) {
    auto *ctx = new MainCtx{alive, std::move(fn)};
    g_timeout_add_full(G_PRIORITY_DEFAULT, ms, main_ctx_trampoline, ctx, main_ctx_free);
}

std::string items_phrase(std::size_t n) { return std::to_string(n) + (n == 1 ? " item" : " items"); }

std::string mode_title(const std::string &mode) {
    if (mode == "quick") return "Quick Scan";
    if (mode == "deep") return "Deep Scan";
    return "Folder Scan";
}

bool path_gone(const fs::path &p) {
    std::error_code ec;
    return !fs::exists(fs::symlink_status(p, ec));
}

// Folders commonly holding irreplaceable personal files or source code,
// offered for protection on first launch when they exist.
const char *const FIRST_RUN_CANDIDATES[] = {
    "Desktop", "Documents", "Pictures", "Music",    "Videos",      "Projects", "projects",
    "src",     "code",      "Code",     "dev",      "Development", "workspace", "repos",
    "source",  "OneDrive",
};

}  // namespace

FileCleanerWindow::FileCleanerWindow(AdwApplication *app, Settings settings) : settings_(std::move(settings)) {
    build_ui(app);
    install_actions(app);

    g_signal_connect(window_, "destroy", G_CALLBACK(&FileCleanerWindow::on_destroy), this);
    g_object_set_data_full(G_OBJECT(window_), "fc-cpp-wrapper", this,
                            +[](gpointer p) { delete static_cast<FileCleanerWindow *>(p); });
}

void FileCleanerWindow::on_destroy(GtkWidget *, gpointer user_data) {
    auto *self = static_cast<FileCleanerWindow *>(user_data);
    self->alive_->store(false);
    if (self->current_cancel_) self->current_cancel_->cancelled.store(true);
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void FileCleanerWindow::build_ui(AdwApplication *app) {
    window_ = adw_application_window_new(GTK_APPLICATION(app));
    gtk_window_set_title(GTK_WINDOW(window_), "File Cleaner");
    gtk_window_set_default_size(GTK_WINDOW(window_), 680, 760);
    gtk_widget_set_size_request(window_, 360, 480);

    toast_overlay_ = adw_toast_overlay_new();
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window_), toast_overlay_);

    toolbar_view_ = adw_toolbar_view_new();
    adw_toolbar_view_set_bottom_bar_style(ADW_TOOLBAR_VIEW(toolbar_view_), ADW_TOOLBAR_RAISED);
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(toast_overlay_), toolbar_view_);

    // ---- Header bar
    GtkWidget *header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view_), header);

    back_btn_ = gtk_button_new_from_icon_name("go-previous-symbolic");
    gtk_widget_set_tooltip_text(back_btn_, "Back to Start");
    gtk_widget_set_visible(back_btn_, FALSE);
    g_signal_connect_swapped(back_btn_, "clicked",
                              G_CALLBACK(+[](FileCleanerWindow *self) { self->show_page("start"); }), this);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), back_btn_);

    GMenu *app_menu = g_menu_new();
    GMenu *section1 = g_menu_new();
    g_menu_append(section1, "Preferences", "win.preferences");
    g_menu_append(section1, "Protected Locations", "win.exclusions");
    g_menu_append(section1, "Scan History", "win.history");
    g_menu_append_section(app_menu, nullptr, G_MENU_MODEL(section1));
    g_object_unref(section1);
    GMenu *section2 = g_menu_new();
    g_menu_append(section2, "About File Cleaner", "win.about");
    g_menu_append_section(app_menu, nullptr, G_MENU_MODEL(section2));
    g_object_unref(section2);

    GtkWidget *menu_btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn), "open-menu-symbolic");
    gtk_menu_button_set_primary(GTK_MENU_BUTTON(menu_btn), TRUE);
    gtk_widget_set_tooltip_text(menu_btn, "Main Menu");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn), G_MENU_MODEL(app_menu));
    g_object_unref(app_menu);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_btn);

    // ---- Pages
    stack_ = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack_), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(stack_), 220);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view_), stack_);

    gtk_stack_add_named(GTK_STACK(stack_), build_start_page(), "start");

    scan_page_ = std::make_unique<ScanPage>(settings_.animation);
    scan_page_->set_on_cancel([this]() { cancel_scan(); });
    gtk_stack_add_named(GTK_STACK(stack_), scan_page_->widget(), "scanning");

    clean_page_ = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(clean_page_), "emblem-ok-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(clean_page_), "Nothing to Clean");
    adw_status_page_set_description(ADW_STATUS_PAGE(clean_page_),
                                     "No removable files were found in the enabled scan categories.");
    GtkWidget *clean_back = gtk_button_new_with_label("Return to Start");
    gtk_widget_add_css_class(clean_back, "pill");
    gtk_widget_set_halign(clean_back, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(clean_back, "clicked",
                              G_CALLBACK(+[](FileCleanerWindow *self) { self->show_page("start"); }), this);
    adw_status_page_set_child(ADW_STATUS_PAGE(clean_page_), clean_back);
    gtk_stack_add_named(GTK_STACK(stack_), clean_page_, "clean");

    scroll_ = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll_, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_stack_add_named(GTK_STACK(stack_), scroll_, "results");

    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 760);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll_), clamp);

    list_box_outer_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
    gtk_widget_set_margin_top(list_box_outer_, 24);
    gtk_widget_set_margin_bottom(list_box_outer_, 24);
    gtk_widget_set_margin_start(list_box_outer_, 12);
    gtk_widget_set_margin_end(list_box_outer_, 12);
    adw_clamp_set_child(ADW_CLAMP(clamp), list_box_outer_);

    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll_));
    g_signal_connect(adj, "value-changed",
                      G_CALLBACK(+[](GtkAdjustment *a, gpointer user_data) {
                          static_cast<FileCleanerWindow *>(user_data)->on_scroll_changed(a);
                      }),
                      this);

    // ---- Bottom bar (results page only)
    GtkWidget *bottom_bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(bottom_bar, "bottom-bar-box");

    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(bottom_bar), btn_row);

    GMenu *scan_menu = g_menu_new();
    g_menu_append(scan_menu, "Quick Scan", "win.scan-quick");
    g_menu_append(scan_menu, "Deep Scan", "win.scan-deep");
    g_menu_append(scan_menu, "Scan a Folder…", "win.scan-folder");

    scan_btn_ = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(scan_btn_), "New Scan");
    gtk_menu_button_set_direction(GTK_MENU_BUTTON(scan_btn_), GTK_ARROW_UP);
    gtk_widget_add_css_class(scan_btn_, "pill");
    gtk_widget_add_css_class(scan_btn_, "action-btn");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(scan_btn_), G_MENU_MODEL(scan_menu));
    g_object_unref(scan_menu);
    gtk_box_append(GTK_BOX(btn_row), scan_btn_);

    delete_btn_ = gtk_button_new_with_label("Delete");
    gtk_widget_add_css_class(delete_btn_, "destructive-action");
    gtk_widget_add_css_class(delete_btn_, "pill");
    gtk_widget_add_css_class(delete_btn_, "action-btn");
    gtk_widget_set_sensitive(delete_btn_, FALSE);
    g_signal_connect_swapped(delete_btn_, "clicked",
                              G_CALLBACK(+[](FileCleanerWindow *self) { self->on_delete_clicked(); }), this);
    gtk_box_append(GTK_BOX(btn_row), delete_btn_);

    status_label_ = gtk_label_new("");
    gtk_widget_add_css_class(status_label_, "dim-label");
    gtk_widget_add_css_class(status_label_, "caption");
    gtk_widget_set_halign(status_label_, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(bottom_bar), status_label_);

    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar_view_), bottom_bar);

    show_page("start");
}

GtkWidget *FileCleanerWindow::build_start_page() {
    GtkWidget *page = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), "user-trash-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(page), "File Cleaner");
    adw_status_page_set_description(ADW_STATUS_PAGE(page),
                                     "Select a scan type to find files that can be safely removed. "
                                     "Nothing is deleted until you review and confirm the results.");

    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 460);

    GtkWidget *list = gtk_list_box_new();
    gtk_widget_add_css_class(list, "boxed-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);

    struct Option {
        const char *action, *icon, *title, *subtitle;
    };
    static const Option options[] = {
        {"win.scan-quick", "edit-find-symbolic", "Quick Scan",
         IS_WINDOWS ? "The Recycle Bin, temporary files, application caches and large, unused downloads"
                    : "The Trash, application caches, logs and large, unused downloads"},
        {"win.scan-deep", "drive-harddisk-symbolic", "Deep Scan",
         "Everything in Quick Scan, plus development files throughout your home folder"},
        {"win.scan-folder", "folder-open-symbolic", "Scan a Folder",
         "Development files and large files inside a folder you choose"},
    };
    for (const auto &opt : options) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), opt.title);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), opt.subtitle);
        GtkWidget *icon = gtk_image_new_from_icon_name(opt.icon);
        gtk_widget_add_css_class(icon, "start-row-icon");
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), icon);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        g_object_set_data(G_OBJECT(row), "fc-action", const_cast<char *>(opt.action));
        gtk_list_box_append(GTK_LIST_BOX(list), row);
    }
    g_signal_connect(list, "row-activated", G_CALLBACK(+[](GtkListBox *, GtkListBoxRow *row, gpointer data) {
                         auto *self = static_cast<FileCleanerWindow *>(data);
                         const char *action = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "fc-action"));
                         if (action) gtk_widget_activate_action(self->window_, action, nullptr);
                     }),
                     this);

    adw_clamp_set_child(ADW_CLAMP(clamp), list);
    adw_status_page_set_child(ADW_STATUS_PAGE(page), clamp);
    return page;
}

void FileCleanerWindow::install_actions(AdwApplication *app) {
    struct ActionSpec {
        const char *name;
        void (*fn)(FileCleanerWindow *);
    };
    static const ActionSpec specs[] = {
        {"scan-quick", [](FileCleanerWindow *s) { s->start_scan("quick", std::nullopt); }},
        {"scan-deep", [](FileCleanerWindow *s) { s->start_scan("deep", std::nullopt); }},
        {"scan-folder", [](FileCleanerWindow *s) { s->choose_folder_and_scan(); }},
        {"preferences", [](FileCleanerWindow *s) { s->open_settings(); }},
        {"exclusions", [](FileCleanerWindow *s) { s->open_settings("exclusions"); }},
        {"history", [](FileCleanerWindow *s) { s->open_settings("history"); }},
        {"about", [](FileCleanerWindow *s) { s->show_about(); }},
    };

    struct ActionCtx {
        FileCleanerWindow *self;
        void (*fn)(FileCleanerWindow *);
    };

    for (const auto &spec : specs) {
        GSimpleAction *action = g_simple_action_new(spec.name, nullptr);
        g_signal_connect_data(
            action, "activate",
            G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer data) {
                auto *ctx = static_cast<ActionCtx *>(data);
                ctx->fn(ctx->self);
            }),
            new ActionCtx{this, spec.fn},
            (GClosureNotify) + [](gpointer data, GClosure *) { delete static_cast<ActionCtx *>(data); },
            (GConnectFlags)0);
        g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(action));
        g_object_unref(action);
    }

    if (!g_action_map_lookup_action(G_ACTION_MAP(app), "quit")) {
        GSimpleAction *quit = g_simple_action_new("quit", nullptr);
        g_signal_connect_swapped(quit, "activate", G_CALLBACK(g_application_quit), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(quit));
        g_object_unref(quit);
    }

    static const char *const prefs_accels[] = {"<Control>comma", nullptr};
    static const char *const quit_accels[] = {"<Control>q", nullptr};
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.preferences", prefs_accels);
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", quit_accels);
}

void FileCleanerWindow::show_page(const char *name) {
    const std::string page = name;
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), name);
    adw_toolbar_view_set_reveal_bottom_bars(ADW_TOOLBAR_VIEW(toolbar_view_), page == "results");
    gtk_widget_set_visible(back_btn_, page == "results" || page == "clean");
}

void FileCleanerWindow::toast(const std::string &text) {
    AdwToast *t = adw_toast_new(text.c_str());
    adw_toast_set_timeout(t, 4);
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(toast_overlay_), t);
}

void FileCleanerWindow::open_settings(const char *page) {
    // Owns itself: freed when its dialog is finalized (see SettingsWindow's
    // constructor).
    auto *sw = new SettingsWindow(window_, settings_, [this]() {
        scan_page_->refresh_style(settings_.animation);
        // Exclusions added after a scan must also apply to its results.
        if (!scanning_ && !deleting_ && !categories_.empty() && prune_results(false)) {
            toast("Results updated to reflect your protected locations");
            if (categories_.empty())
                show_clean_page();
            else
                show_results(false);
        }
    });
    sw->present(page);
}

void FileCleanerWindow::show_about() {
    AdwDialog *about = adw_about_dialog_new();
    AdwAboutDialog *a = ADW_ABOUT_DIALOG(about);
    adw_about_dialog_set_application_name(a, "File Cleaner");
    adw_about_dialog_set_application_icon(a, "io.github.filecleaner");
    adw_about_dialog_set_version(a, APP_VERSION);
    adw_about_dialog_set_developer_name(a, "Tab680fly");
    adw_about_dialog_set_comments(a, "Find and remove unnecessary files from your home folder.");
    adw_about_dialog_set_license_type(a, GTK_LICENSE_MIT_X11);
    adw_about_dialog_set_website(a, "https://github.com/tab680fly-prog/simple-file-cleaner");
    adw_dialog_present(about, window_);
}

// ---------------------------------------------------------------------------
// First-launch warning
// ---------------------------------------------------------------------------

void FileCleanerWindow::maybe_show_first_run_warning() {
    if (settings_.exclusion_warning_acknowledged) return;

    std::vector<std::string> folders;
    const fs::path home = home_dir();
    for (const char *name : FIRST_RUN_CANDIDATES) {
        fs::path p = home / name;
        std::error_code ec;
        if (fs::is_directory(p, ec) && !settings_.path_excluded(p)) folders.push_back(path_str(p));
    }

    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        "Protect Your Important Folders",
        "File Cleaner permanently deletes the files you select, and they cannot be recovered. Deep scans and "
        "folder scans may also match folders such as “build”, “dist” or “target” inside your own projects.\n\n"
        "Before scanning, it is strongly recommended that you add exclusions for folders containing important "
        "files. Excluded folders are never scanned or deleted. Exclusions can be changed at any time in "
        "Preferences."));

    struct FirstRunCtx {
        FileCleanerWindow *self;
        std::shared_ptr<std::atomic<bool>> alive;
        std::vector<std::pair<GtkWidget *, std::string>> switches;
    };
    auto *ctx = new FirstRunCtx{this, alive_, {}};

    if (!folders.empty()) {
        GtkWidget *list = gtk_list_box_new();
        gtk_widget_add_css_class(list, "boxed-list");
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
        for (const auto &f : folders) {
            GtkWidget *row = adw_switch_row_new();
            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), display_path(f).c_str());
            adw_switch_row_set_active(ADW_SWITCH_ROW(row), TRUE);
            gtk_list_box_append(GTK_LIST_BOX(list), row);
            ctx->switches.emplace_back(row, f);
        }
        adw_alert_dialog_set_extra_child(dialog, list);
    }

    adw_alert_dialog_add_response(dialog, "skip", "Not Now");
    adw_alert_dialog_add_response(dialog, "settings", "Choose Folders…");
    if (!folders.empty()) {
        adw_alert_dialog_add_response(dialog, "add", "Protect Selected");
        adw_alert_dialog_set_response_appearance(dialog, "add", ADW_RESPONSE_SUGGESTED);
        adw_alert_dialog_set_default_response(dialog, "add");
    } else {
        adw_alert_dialog_set_response_appearance(dialog, "settings", ADW_RESPONSE_SUGGESTED);
        adw_alert_dialog_set_default_response(dialog, "settings");
    }
    adw_alert_dialog_set_close_response(dialog, "skip");
    adw_alert_dialog_set_prefer_wide_layout(dialog, TRUE);

    g_signal_connect_data(
        dialog, "response",
        G_CALLBACK(+[](AdwAlertDialog *, const char *response, gpointer data) {
            auto *c = static_cast<FirstRunCtx *>(data);
            if (!c->alive->load()) return;
            FileCleanerWindow *self = c->self;
            const std::string r = response;

            self->settings_.exclusion_warning_acknowledged = true;
            if (r == "add") {
                int added = 0;
                for (const auto &[row, path] : c->switches) {
                    if (!adw_switch_row_get_active(ADW_SWITCH_ROW(row))) continue;
                    if (std::find(self->settings_.excluded_paths.begin(), self->settings_.excluded_paths.end(),
                                  path) != self->settings_.excluded_paths.end())
                        continue;
                    self->settings_.excluded_paths.push_back(path);
                    ++added;
                }
                self->settings_.save();
                if (added > 0)
                    self->toast(std::to_string(added) + (added == 1 ? " folder is" : " folders are") +
                                " now protected");
            } else {
                self->settings_.save();
            }
            if (r == "settings") self->open_settings("exclusions");
        }),
        ctx, (GClosureNotify) + [](gpointer data, GClosure *) { delete static_cast<FirstRunCtx *>(data); },
        (GConnectFlags)0);

    adw_dialog_present(ADW_DIALOG(dialog), window_);
}

// ---------------------------------------------------------------------------
// Scan workflow
// ---------------------------------------------------------------------------

void FileCleanerWindow::choose_folder_and_scan() {
    if (scanning_ || deleting_) return;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select a Folder to Scan");

    struct FolderCtx {
        FileCleanerWindow *self;
        std::shared_ptr<std::atomic<bool>> alive;
    };
    gtk_file_dialog_select_folder(
        dialog, GTK_WINDOW(window_), nullptr,
        +[](GObject *source, GAsyncResult *result, gpointer user_data) {
            auto *c = static_cast<FolderCtx *>(user_data);
            GError *error = nullptr;
            GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
            if (folder) {
                char *path = g_file_get_path(folder);
                if (path && c->alive->load()) c->self->start_scan("folder", path_from(path));
                g_free(path);
                g_object_unref(folder);
            }
            if (error) g_error_free(error);
            g_object_unref(source);
            delete c;
        },
        new FolderCtx{this, alive_});
}

void FileCleanerWindow::start_scan(const std::string &mode, std::optional<fs::path> root) {
    if (scanning_ || deleting_) return;
    scanning_ = true;
    scan_mode_ = mode;
    scan_root_ = root;
    categories_.clear();
    clear_results_list();
    gtk_widget_set_sensitive(scan_btn_, FALSE);
    gtk_widget_set_sensitive(delete_btn_, FALSE);

    std::string label = post_delete_                ? "Refreshing Results…"
                        : (mode == "folder" && root) ? "Scanning " + path_str(root->filename()) + "…"
                        : mode == "deep"             ? "Performing Deep Scan…"
                                                     : "Performing Quick Scan…";

    scan_gen_++;
    const int gen = scan_gen_;

    if (current_cancel_) current_cancel_->cancelled.store(true);
    auto cancel = std::make_shared<CancelToken>();
    current_cancel_ = cancel;

    scan_page_->refresh_style(settings_.animation);
    scan_page_->start(label);
    show_page("scanning");

    // The worker gets its own copy of the settings: the Preferences dialog
    // can edit settings_ on the main thread while a scan is running, which
    // was previously a data race. The worker also never touches `this`
    // directly — only via run_on_main, which checks `alive` first — so the
    // window can be closed mid-scan safely.
    Settings snapshot = settings_;
    auto alive = alive_;
    std::thread([this, alive, snapshot = std::move(snapshot), mode, root, gen, cancel]() {
        auto last_update = std::make_shared<std::atomic<long long>>(0);
        ProgressCb progress = [this, alive, gen, last_update](const std::string &path) {
            auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            long long prev = last_update->load();
            const long long threshold_ns = 125'000'000;  // 0.125s
            if (now - prev > threshold_ns && last_update->compare_exchange_strong(prev, now)) {
                run_on_main(alive, [this, path, gen]() {
                    if (gen == scan_gen_) scan_page_->set_path(path);
                });
            }
        };

        CategoryCb emit = [this, alive, gen, &snapshot](Category &&cat) {
            // Central safety net: whatever a scanner produced, never offer a
            // path that is excluded or that contains an excluded path.
            auto &es = cat.entries;
            es.erase(std::remove_if(es.begin(), es.end(),
                                    [&](const FileEntry &e) {
                                        return snapshot.must_preserve(e.path) || is_protected_path(e.path);
                                    }),
                     es.end());
            if (es.empty()) return;
            run_on_main(alive, [this, cat = std::move(cat), gen]() mutable { on_category_found(std::move(cat), gen); });
        };

        if (mode == "quick") {
            scan_generator(snapshot, progress, emit, *cancel);
        } else if (mode == "deep") {
            deep_scan_generator(snapshot, progress, emit, *cancel);
        } else if (root) {
            folder_scan_generator(*root, snapshot, progress, emit, *cancel);
        }

        const bool cancelled = cancel->is_cancelled();
        run_on_main(alive, [this, gen, cancelled]() { scan_done(gen, cancelled); });
    }).detach();
}

void FileCleanerWindow::cancel_scan() {
    if (!scanning_ || !current_cancel_) return;
    current_cancel_->cancelled.store(true);
    scan_page_->set_title("Cancelling…");
}

void FileCleanerWindow::on_category_found(Category cat, int gen) {
    if (gen != scan_gen_) return;
    categories_.push_back(std::move(cat));
    std::uint64_t total = 0;
    for (auto &c : categories_) total += c.total_size();
    scan_page_->set_found(static_cast<int>(categories_.size()), total);
}

void FileCleanerWindow::scan_done(int gen, bool cancelled) {
    if (gen != scan_gen_) return;
    scanning_ = false;
    gtk_widget_set_sensitive(scan_btn_, TRUE);

    if (cancelled) {
        post_delete_ = false;
        categories_.clear();
        scan_page_->stop();
        show_page("start");
        toast("The scan was cancelled");
        return;
    }

    std::uint64_t total = 0;
    for (auto &c : categories_) total += c.total_size();
    scan_page_->set_done(static_cast<int>(categories_.size()), total);

    // A refresh after deleting is not a new scan: deletions from it are
    // added to the original scan's history record instead.
    if (!post_delete_) {
        HistoryEntry entry;
        entry.id = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
        entry.date = iso_now();
        entry.mode = scan_mode_;
        entry.categories_found = static_cast<int>(categories_.size());
        entry.total_found = total;
        add_history_entry(entry);
        last_history_id_ = entry.id;
    }

    const bool was_post_delete = post_delete_;
    post_delete_ = false;
    run_on_main_delayed(alive_, was_post_delete ? 300 : 900, [this, gen]() {
        if (gen != scan_gen_ || scanning_ || deleting_) return;
        if (categories_.empty())
            show_clean_page();
        else
            show_results();
    });
}

void FileCleanerWindow::show_clean_page() {
    clear_results_list();
    show_page("clean");
}

void FileCleanerWindow::show_results(bool animate) {
    std::uint64_t total = 0;
    std::size_t total_items = 0;
    for (auto &c : categories_) {
        total += c.total_size();
        total_items += c.entries.size();
    }

    clear_results_list();

    // ---- Summary card
    GtkWidget *summary = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(summary, "card");
    gtk_widget_add_css_class(summary, "summary-card");

    GtkWidget *summary_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(summary_text, TRUE);
    gtk_widget_set_valign(summary_text, GTK_ALIGN_CENTER);

    GtkWidget *mode_label = gtk_label_new(mode_title(scan_mode_).c_str());
    gtk_widget_add_css_class(mode_label, "caption-heading");
    gtk_widget_add_css_class(mode_label, "dim-label");
    gtk_widget_set_halign(mode_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(summary_text), mode_label);

    GtkWidget *size_label = gtk_label_new(fmt_size(total).c_str());
    gtk_widget_add_css_class(size_label, "summary-size");
    gtk_widget_set_halign(size_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(summary_text), size_label);

    std::string detail = "Found in " + items_phrase(total_items) + " across " + std::to_string(categories_.size()) +
                         (categories_.size() == 1 ? " category" : " categories");
    GtkWidget *detail_label = gtk_label_new(detail.c_str());
    gtk_widget_add_css_class(detail_label, "dim-label");
    gtk_widget_set_halign(detail_label, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(detail_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(detail_label), 0);
    gtk_box_append(GTK_BOX(summary_text), detail_label);

    GtkWidget *sel_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_valign(sel_box, GTK_ALIGN_CENTER);
    GtkWidget *all_btn = gtk_button_new_with_label("Select All");
    g_signal_connect_swapped(all_btn, "clicked",
                              G_CALLBACK(+[](FileCleanerWindow *self) { self->set_all_selected(true); }), this);
    GtkWidget *none_btn = gtk_button_new_with_label("Select None");
    g_signal_connect_swapped(none_btn, "clicked",
                              G_CALLBACK(+[](FileCleanerWindow *self) { self->set_all_selected(false); }), this);
    gtk_box_append(GTK_BOX(sel_box), all_btn);
    gtk_box_append(GTK_BOX(sel_box), none_btn);

    gtk_box_append(GTK_BOX(summary), summary_text);
    gtk_box_append(GTK_BOX(summary), sel_box);
    gtk_box_append(GTK_BOX(list_box_outer_), summary);

    // ---- Category groups, largest first
    std::vector<Category *> ordered;
    for (auto &cat : categories_) ordered.push_back(&cat);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Category *a, const Category *b) { return a->total_size() > b->total_size(); });

    std::vector<GtkWidget *> revealers;
    for (Category *cat : ordered) {
        add_category_group(*cat, animate);
        revealers.push_back(gtk_widget_get_last_child(list_box_outer_));
    }

    show_page("results");
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll_));
    gtk_adjustment_set_value(adj, 0);

    if (animate) {
        for (std::size_t i = 0; i < revealers.size(); ++i) {
            GtkWidget *rev = revealers[i];
            // Held alive independently of list_box_outer_'s ownership: if a
            // new scan clears the results list before this timer fires, the
            // revealer would otherwise have been finalized already.
            g_object_ref(rev);
            run_on_main_delayed(alive_, static_cast<unsigned>(i) * 70, [rev]() {
                gtk_revealer_set_reveal_child(GTK_REVEALER(rev), TRUE);
                g_object_unref(rev);
            });
        }
    }

    refresh_delete_btn();
}

void FileCleanerWindow::clear_results_list() {
    row_checks_.clear();
    GtkWidget *child = gtk_widget_get_first_child(list_box_outer_);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(list_box_outer_), child);
        child = next;
    }
}

void FileCleanerWindow::add_category_group(Category &cat, bool animate) {
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    char *escaped_title = g_markup_escape_text(cat.title.c_str(), -1);
    adw_preferences_group_set_title(group, escaped_title);
    g_free(escaped_title);
    std::string desc = cat.subtitle + " · " + fmt_size(cat.total_size());
    char *escaped_desc = g_markup_escape_text(desc.c_str(), -1);
    adw_preferences_group_set_description(group, escaped_desc);
    g_free(escaped_desc);

    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(header_box, "linked");
    gtk_widget_set_valign(header_box, GTK_ALIGN_CENTER);
    GtkWidget *select_all_btn = gtk_button_new_with_label("All");
    gtk_widget_set_tooltip_text(select_all_btn, "Select all items in this category");
    gtk_widget_add_css_class(select_all_btn, "caption");
    GtkWidget *select_none_btn = gtk_button_new_with_label("None");
    gtk_widget_set_tooltip_text(select_none_btn, "Deselect all items in this category");
    gtk_widget_add_css_class(select_none_btn, "caption");

    struct SelCtx {
        FileCleanerWindow *self;
        Category *cat;
        bool selected;
    };
    for (auto [btn, sel] : {std::pair{select_all_btn, true}, std::pair{select_none_btn, false}}) {
        g_signal_connect_data(
            btn, "clicked",
            G_CALLBACK(+[](GtkButton *, gpointer data) {
                auto *c = static_cast<SelCtx *>(data);
                c->self->select_all_in_cat(*c->cat, c->selected);
            }),
            new SelCtx{this, &cat, sel},
            (GClosureNotify) + [](gpointer data, GClosure *) { delete static_cast<SelCtx *>(data); },
            (GConnectFlags)0);
    }

    gtk_box_append(GTK_BOX(header_box), select_all_btn);
    gtk_box_append(GTK_BOX(header_box), select_none_btn);
    adw_preferences_group_set_header_suffix(group, header_box);

    // Largest items first within a category too.
    std::stable_sort(cat.entries.begin(), cat.entries.end(),
                     [](const FileEntry &a, const FileEntry &b) { return a.size > b.size; });

    GtkWidget *expander = nullptr;
    if (cat.entries.size() > COLLAPSE_THRESHOLD) {
        expander = adw_expander_row_new();
        std::string title = items_phrase(cat.entries.size());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(expander), title.c_str());
        adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(expander), "Expand to review individual items");
        adw_preferences_group_add(group, expander);
    }

    for (auto &entry : cat.entries) {
        FileRow row(entry, [this](FileEntry &e, bool active) { on_file_toggle(e, active); });
        row_checks_.emplace_back(&entry, row.check());
        if (expander)
            adw_expander_row_add_row(ADW_EXPANDER_ROW(expander), row.widget());
        else
            adw_preferences_group_add(group, row.widget());
    }

    GtkWidget *revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), 260);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), !animate);
    gtk_revealer_set_child(GTK_REVEALER(revealer), GTK_WIDGET(group));
    gtk_box_append(GTK_BOX(list_box_outer_), revealer);
}

void FileCleanerWindow::select_all_in_cat(Category &cat, bool selected) {
    if (cat.entries.empty()) return;
    const FileEntry *first = &cat.entries.front();
    const FileEntry *last = &cat.entries.back();
    // Previously only the model was updated, so the checkboxes kept showing
    // the old state while the Delete button reflected the new one.
    bulk_update_ = true;
    for (auto &[entry, check] : row_checks_) {
        if (entry < first || entry > last) continue;
        entry->selected = selected;
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), selected);
    }
    bulk_update_ = false;
    refresh_delete_btn();
}

void FileCleanerWindow::set_all_selected(bool selected) {
    bulk_update_ = true;
    for (auto &[entry, check] : row_checks_) {
        entry->selected = selected;
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), selected);
    }
    bulk_update_ = false;
    refresh_delete_btn();
}

void FileCleanerWindow::on_file_toggle(FileEntry &entry, bool active) {
    entry.selected = active;
    if (!bulk_update_) refresh_delete_btn();
}

void FileCleanerWindow::refresh_delete_btn() {
    std::uint64_t total = 0;
    std::size_t selected = 0, count = 0;
    for (auto &cat : categories_) {
        for (auto &entry : cat.entries) {
            ++count;
            if (entry.selected) {
                total += entry.size;
                ++selected;
            }
        }
    }

    if (selected > 0) {
        std::string label = "Delete " + fmt_size(total);
        gtk_button_set_label(GTK_BUTTON(delete_btn_), label.c_str());
        gtk_widget_set_sensitive(delete_btn_, !scanning_ && !deleting_);
    } else {
        gtk_button_set_label(GTK_BUTTON(delete_btn_), "Delete");
        gtk_widget_set_sensitive(delete_btn_, FALSE);
    }
    std::string status = std::to_string(selected) + " of " + items_phrase(count) + " selected";
    gtk_label_set_label(GTK_LABEL(status_label_), status.c_str());
}

void FileCleanerWindow::on_scroll_changed(GtkAdjustment *adj) {
    double scroll_y = gtk_adjustment_get_value(adj);
    double viewport_h = gtk_adjustment_get_page_size(adj);
    const double fade_zone = 48;

    GtkWidget *child = gtk_widget_get_first_child(list_box_outer_);
    while (child) {
        graphene_rect_t bounds;
        if (gtk_widget_compute_bounds(child, list_box_outer_, &bounds)) {
            double child_top = bounds.origin.y;
            double child_bottom = bounds.origin.y + bounds.size.height;

            if (child_bottom < scroll_y - fade_zone || child_top > scroll_y + viewport_h + fade_zone) {
                gtk_widget_set_opacity(child, 0.0);
            } else if (child_bottom < scroll_y + fade_zone) {
                gtk_widget_set_opacity(child, std::max(0.12, (child_bottom - scroll_y) / std::max(1.0, fade_zone)));
            } else if (child_top > scroll_y + viewport_h - fade_zone) {
                gtk_widget_set_opacity(
                    child, std::max(0.12, (scroll_y + viewport_h - child_top) / std::max(1.0, fade_zone)));
            } else {
                gtk_widget_set_opacity(child, 1.0);
            }
        }
        child = gtk_widget_get_next_sibling(child);
    }
}

bool FileCleanerWindow::prune_results(bool drop_missing) {
    bool changed = false;
    for (auto &cat : categories_) {
        auto &es = cat.entries;
        auto before = es.size();
        es.erase(std::remove_if(es.begin(), es.end(),
                                [&](const FileEntry &e) {
                                    return settings_.must_preserve(e.path) || (drop_missing && path_gone(e.path));
                                }),
                 es.end());
        if (es.size() != before) changed = true;
    }
    auto before = categories_.size();
    categories_.erase(std::remove_if(categories_.begin(), categories_.end(),
                                     [](const Category &c) { return c.entries.empty(); }),
                      categories_.end());
    return changed || categories_.size() != before;
}

// ---------------------------------------------------------------------------
// Delete workflow
// ---------------------------------------------------------------------------

void FileCleanerWindow::on_delete_clicked() {
    if (scanning_ || deleting_) return;

    std::vector<std::pair<fs::path, std::uint64_t>> items;
    std::uint64_t total = 0;
    std::vector<fs::path> companions;
    std::size_t skipped = 0, selected_count = 0;
    for (auto &cat : categories_) {
        for (auto &entry : cat.entries) {
            if (!entry.selected) continue;
            // Re-checked against the current settings: exclusions may have
            // been added after the scan ran.
            if (settings_.must_preserve(entry.path) || is_protected_path(entry.path)) {
                ++skipped;
                continue;
            }
            items.emplace_back(entry.path, entry.size);
            companions.insert(companions.end(), entry.companions.begin(), entry.companions.end());
            total += entry.size;
            ++selected_count;
        }
    }
    if (items.empty()) {
        if (skipped) toast("The selected items are in protected locations and cannot be deleted");
        return;
    }

    std::string body = items_phrase(selected_count) + " totaling " + fmt_size(total) +
                       " will be permanently deleted. This action cannot be undone.";
    if (skipped)
        body += "\n\n" + items_phrase(skipped) + (skipped == 1 ? " is" : " are") +
                " in a protected location and will not be deleted.";

    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Delete Selected Items?", body.c_str()));
    adw_alert_dialog_add_response(dialog, "cancel", "Cancel");
    adw_alert_dialog_add_response(dialog, "delete", "Delete");
    adw_alert_dialog_set_response_appearance(dialog, "delete", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(dialog, "cancel");
    adw_alert_dialog_set_close_response(dialog, "cancel");

    struct DeleteCtx {
        FileCleanerWindow *self;
        std::shared_ptr<std::atomic<bool>> alive;
        std::vector<std::pair<fs::path, std::uint64_t>> items;
        std::vector<fs::path> companions;
    };
    g_signal_connect_data(
        dialog, "response",
        G_CALLBACK(+[](AdwAlertDialog *, const char *response, gpointer data) {
            auto *c = static_cast<DeleteCtx *>(data);
            if (c->alive->load() && std::string(response) == "delete") c->self->start_delete(std::move(c->items), std::move(c->companions));
        }),
        new DeleteCtx{this, alive_, std::move(items), std::move(companions)},
        (GClosureNotify) + [](gpointer data, GClosure *) { delete static_cast<DeleteCtx *>(data); },
        (GConnectFlags)0);
    adw_dialog_present(ADW_DIALOG(dialog), window_);
}

void FileCleanerWindow::start_delete(std::vector<std::pair<fs::path, std::uint64_t>> items,
                                     std::vector<fs::path> companions) {
    if (scanning_ || deleting_ || items.empty()) return;
    deleting_ = true;
    gtk_widget_set_sensitive(delete_btn_, FALSE);
    gtk_widget_set_sensitive(scan_btn_, FALSE);

    scan_page_->refresh_style(settings_.animation);
    scan_page_->start("Deleting " + items_phrase(items.size()) + "…", false);
    scan_page_->set_path_label("This may take a moment for large folders.");
    show_page("scanning");

    auto alive = alive_;
    std::thread([this, alive, items = std::move(items), companions = std::move(companions)]() {
        std::vector<fs::path> paths;
        for (const auto &[p, sz] : items) paths.push_back(p);
        paths.insert(paths.end(), companions.begin(), companions.end());

        DeleteResult result = delete_direct(paths);
        if (!IS_WINDOWS && !result.permission_failed.empty() && !running_as_admin()) {
            DeleteResult pk = delete_with_pkexec(result.permission_failed);
            result.deleted_count += pk.deleted_count;
            result.errors.insert(result.errors.end(), pk.errors.begin(), pk.errors.end());
        } else {
            for (auto &p : result.permission_failed)
                result.errors.push_back(display_path(p) + (IS_WINDOWS ? ": Access denied. Some items can only be "
                                                                        "deleted when File Cleaner is run as an "
                                                                        "administrator."
                                                                      : ": Permission denied"));
        }

        // Count only what is actually gone, rather than assuming every
        // selected byte was freed even when some deletions failed.
        std::uint64_t freed = 0;
        int deleted_count = 0;
        for (const auto &[p, sz] : items) {
            if (path_gone(p)) {
                freed += sz;
                ++deleted_count;
            }
        }
        run_on_main(alive, [this, deleted_count, freed, errors = std::move(result.errors)]() mutable {
            delete_done(deleted_count, freed, std::move(errors));
        });
    }).detach();
}

void FileCleanerWindow::delete_done(int deleted_count, std::uint64_t deleted_bytes, std::vector<std::string> errors) {
    deleting_ = false;
    gtk_widget_set_sensitive(scan_btn_, TRUE);
    scan_page_->stop();

    if (!errors.empty()) {
        std::string body;
        const std::size_t shown = std::min<std::size_t>(errors.size(), 8);
        for (std::size_t i = 0; i < shown; ++i) {
            if (i) body += "\n";
            body += errors[i];
        }
        if (errors.size() > shown) body += "\n…and " + std::to_string(errors.size() - shown) + " more.";
        AdwAlertDialog *dlg =
            ADW_ALERT_DIALOG(adw_alert_dialog_new("Some Items Could Not Be Deleted", body.c_str()));
        adw_alert_dialog_add_response(dlg, "ok", "OK");
        adw_dialog_present(ADW_DIALOG(dlg), window_);
    }

    if (deleted_count > 0) {
        toast("Deleted " + items_phrase(static_cast<std::size_t>(deleted_count)) + ", freeing " +
              fmt_size(deleted_bytes));

        if (!last_history_id_.empty()) {
            auto entries = load_history();
            for (auto &e : entries) {
                if (e.id == last_history_id_) {
                    e.total_deleted += deleted_bytes;
                    e.deleted_count += deleted_count;
                    break;
                }
            }
            save_history(entries);
        }
    } else if (errors.empty()) {
        toast("No items were deleted");
    }

    if (deleted_count > 0 && settings_.auto_rescan) {
        post_delete_ = true;
        start_scan(scan_mode_, scan_root_);
        return;
    }

    // Without a rescan, remove whatever was deleted from the list so it
    // cannot be selected and "deleted" a second time.
    prune_results(true);
    if (categories_.empty())
        show_clean_page();
    else
        show_results(false);
}

}  // namespace fc
