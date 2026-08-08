#include "main_window.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "delete_engine.hpp"
#include "file_row.hpp"
#include "history.hpp"
#include "settings_window.hpp"

namespace fc {

namespace {

constexpr const char *APP_VERSION = "1.2.0";

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

gboolean timeout_ctx_trampoline(gpointer data) {
    auto *c = static_cast<MainCtx *>(data);
    if (c->alive->load()) c->fn();
    return G_SOURCE_REMOVE;
}

void run_on_main_delayed(const std::shared_ptr<std::atomic<bool>> &alive, unsigned ms, std::function<void()> fn) {
    auto *ctx = new MainCtx{alive, std::move(fn)};
    g_timeout_add_full(G_PRIORITY_DEFAULT, ms, timeout_ctx_trampoline, ctx, main_ctx_free);
}

}  // namespace

FileCleanerWindow::FileCleanerWindow(AdwApplication *app, Settings settings) : settings_(std::move(settings)) {
    build_ui(app);
    install_actions();

    g_signal_connect(window_, "destroy", G_CALLBACK(&FileCleanerWindow::on_destroy), this);
    g_object_set_data_full(G_OBJECT(window_), "fc-cpp-wrapper", this,
                            +[](gpointer p) { delete static_cast<FileCleanerWindow *>(p); });
}

void FileCleanerWindow::on_destroy(GtkWidget *, gpointer user_data) {
    auto *self = static_cast<FileCleanerWindow *>(user_data);
    self->alive_->store(false);
    if (self->current_cancel_) self->current_cancel_->cancelled.store(true);
}

void FileCleanerWindow::build_ui(AdwApplication *app) {
    window_ = adw_application_window_new(GTK_APPLICATION(app));
    gtk_window_set_title(GTK_WINDOW(window_), "File Cleaner");
    gtk_window_set_default_size(GTK_WINDOW(window_), 640, 720);
    gtk_widget_set_size_request(window_, 480, 420);

    GtkWidget *toolbar_view = adw_toolbar_view_new();
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window_), toolbar_view);

    GtkWidget *header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header);

    GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(title_box, GTK_ALIGN_CENTER);
    GtkWidget *title_label = gtk_label_new("File Cleaner");
    gtk_widget_add_css_class(title_label, "title-4");
    GtkWidget *version_label = gtk_label_new((std::string("v") + APP_VERSION).c_str());
    gtk_widget_add_css_class(version_label, "dim-label");
    gtk_widget_add_css_class(version_label, "caption");
    gtk_box_append(GTK_BOX(title_box), title_label);
    gtk_box_append(GTK_BOX(title_box), version_label);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title_box);

    GtkWidget *settings_btn = gtk_button_new_from_icon_name("preferences-system-symbolic");
    g_signal_connect_swapped(settings_btn, "clicked", G_CALLBACK(+[](FileCleanerWindow *self) {
                                  self->open_settings();
                              }),
                              this);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), settings_btn);

    stack_ = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack_), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(stack_), 220);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), stack_);

    GtkWidget *placeholder = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(placeholder), "user-trash-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(placeholder), "Ready");
    adw_status_page_set_description(ADW_STATUS_PAGE(placeholder),
                                     "Select a structural scanning pattern to review temporary file targets.");
    gtk_stack_add_named(GTK_STACK(stack_), placeholder, "empty");

    scan_page_ = std::make_unique<ScanPage>(settings_.animation);
    gtk_stack_add_named(GTK_STACK(stack_), scan_page_->widget(), "scanning");

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_stack_add_named(GTK_STACK(stack_), scroll, "results");

    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 800);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);

    list_box_outer_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_top(list_box_outer_, 18);
    gtk_widget_set_margin_bottom(list_box_outer_, 18);
    gtk_widget_set_margin_start(list_box_outer_, 12);
    gtk_widget_set_margin_end(list_box_outer_, 12);
    adw_clamp_set_child(ADW_CLAMP(clamp), list_box_outer_);

    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    g_signal_connect(adj, "value-changed",
                      G_CALLBACK(+[](GtkAdjustment *a, gpointer user_data) {
                          static_cast<FileCleanerWindow *>(user_data)->on_scroll_changed(a);
                      }),
                      this);

    GtkWidget *bottom_bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(bottom_bar, "bottom-bar-box");

    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(bottom_bar), btn_row);

    GMenu *scan_menu = g_menu_new();
    g_menu_append(scan_menu, "Quick scan (standard targets)", "win.scan-quick");
    g_menu_append(scan_menu, "Deep scan (full search)", "win.scan-deep");
    g_menu_append(scan_menu, "Isolate folder mapping…", "win.scan-folder");

    scan_btn_ = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(scan_btn_), "Scan");
    gtk_widget_add_css_class(scan_btn_, "suggested-action");
    gtk_widget_add_css_class(scan_btn_, "action-btn");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(scan_btn_), G_MENU_MODEL(scan_menu));
    g_object_unref(scan_menu);
    gtk_box_append(GTK_BOX(btn_row), scan_btn_);

    delete_btn_ = gtk_button_new_with_label("Clear targets");
    gtk_widget_add_css_class(delete_btn_, "destructive-action");
    gtk_widget_add_css_class(delete_btn_, "action-btn");
    gtk_widget_set_sensitive(delete_btn_, FALSE);
    g_signal_connect_swapped(delete_btn_, "clicked", G_CALLBACK(+[](FileCleanerWindow *self) {
                                  self->on_delete_clicked();
                              }),
                              this);
    gtk_box_append(GTK_BOX(btn_row), delete_btn_);

    status_label_ = gtk_label_new("Execution runtime standing by");
    gtk_widget_add_css_class(status_label_, "dim-label");
    gtk_widget_add_css_class(status_label_, "caption");
    gtk_widget_set_halign(status_label_, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(status_label_, 6);
    gtk_box_append(GTK_BOX(bottom_bar), status_label_);

    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar_view), bottom_bar);

    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "empty");
}

void FileCleanerWindow::install_actions() {
    struct ActionSpec {
        const char *name;
        std::string mode;
    };
    static const ActionSpec specs[] = {
        {"scan-quick", "quick"},
        {"scan-deep", "deep"},
        {"scan-folder", "folder"},
    };

    struct ActionCtx {
        FileCleanerWindow *self;
        std::string mode;
    };

    for (const auto &spec : specs) {
        GSimpleAction *action = g_simple_action_new(spec.name, nullptr);
        std::string mode = spec.mode;
        g_signal_connect_data(
            action, "activate",
            G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer data) {
                auto *ctx = static_cast<ActionCtx *>(data);
                FileCleanerWindow *self = ctx->self;
                const std::string &mode = ctx->mode;
                if (mode == "folder") {
                    GtkFileDialog *dialog = gtk_file_dialog_new();
                    gtk_file_dialog_select_folder(
                        dialog, GTK_WINDOW(self->window_), nullptr,
                        +[](GObject *source, GAsyncResult *result, gpointer user_data) {
                            auto *self2 = static_cast<FileCleanerWindow *>(user_data);
                            GError *error = nullptr;
                            GFile *folder =
                                gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
                            if (folder) {
                                char *path = g_file_get_path(folder);
                                if (path) {
                                    self2->start_scan("folder", fs::path(path));
                                    g_free(path);
                                }
                                g_object_unref(folder);
                            }
                            if (error) g_error_free(error);
                        },
                        self);
                } else {
                    self->start_scan(mode, std::nullopt);
                }
            }),
            new ActionCtx{this, mode},
            (GClosureNotify) + [](gpointer data, GClosure *) { delete static_cast<ActionCtx *>(data); },
            (GConnectFlags)0);
        g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(action));
        g_object_unref(action);
    }
}

void FileCleanerWindow::open_settings() {
    // Owns itself: freed when its GTK window is finalized (see
    // SettingsWindow's constructor).
    auto *sw = new SettingsWindow(GTK_WINDOW(window_), settings_,
                                   [this]() { scan_page_->refresh_style(settings_.animation); });
    sw->present();
}

// ---------------------------------------------------------------------------
// Scan workflow
// ---------------------------------------------------------------------------

void FileCleanerWindow::start_scan(const std::string &mode, std::optional<fs::path> root) {
    if (scanning_) return;
    scanning_ = true;
    scan_mode_ = mode;
    scan_root_ = root;
    categories_.clear();
    gtk_widget_set_sensitive(scan_btn_, FALSE);
    gtk_widget_set_sensitive(delete_btn_, FALSE);
    gtk_button_set_label(GTK_BUTTON(delete_btn_), "Clear targets");

    std::string label = (mode == "folder" && root) ? "Scanning " + root->filename().string() + "…"
                         : post_delete_             ? "Rescanning targets…"
                                                     : "Scanning filesystem structures…";
    gtk_label_set_label(GTK_LABEL(status_label_), label.c_str());

    clear_results_list();

    scan_gen_++;
    int my_gen = scan_gen_;

    if (current_cancel_) current_cancel_->cancelled.store(true);
    auto cancel = std::make_shared<CancelToken>();
    current_cancel_ = cancel;

    scan_page_->start(label);
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "scanning");

    std::thread(&FileCleanerWindow::scan_worker, this, mode, root, my_gen, cancel).detach();
}

void FileCleanerWindow::scan_worker(std::string mode, std::optional<fs::path> root, int gen,
                                     std::shared_ptr<CancelToken> cancel) {
    auto last_update = std::make_shared<std::atomic<long long>>(0);
    ProgressCb progress = [this, gen, last_update](const std::string &path) {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        long long prev = last_update->load();
        const long long threshold_ns = 125'000'000;  // 0.125s, matches original throttle
        if (now - prev > threshold_ns && last_update->compare_exchange_strong(prev, now)) {
            run_on_main(alive_, [this, path, gen]() {
                if (gen == scan_gen_) scan_page_->set_path(path);
            });
        }
    };

    auto found_bytes = std::make_shared<std::uint64_t>(0);
    CategoryCb emit = [this, gen, found_bytes](Category &&cat) {
        *found_bytes += cat.total_size();
        run_on_main(alive_, [this, cat = std::move(cat), total = *found_bytes, gen]() mutable {
            on_category_found(std::move(cat), total, gen);
        });
    };

    if (mode == "quick") {
        scan_generator(settings_, progress, emit, *cancel);
    } else if (mode == "deep") {
        deep_scan_generator(settings_, progress, emit, *cancel);
    } else if (root) {
        folder_scan_generator(*root, settings_, progress, emit, *cancel);
    }

    std::uint64_t total = *found_bytes;
    run_on_main(alive_, [this, gen, total]() {
        if (gen == scan_gen_) scan_done(gen, total);
    });
}

void FileCleanerWindow::on_category_found(Category cat, std::uint64_t /*running_total*/, int gen) {
    if (gen != scan_gen_) return;
    categories_.push_back(std::move(cat));
    std::uint64_t total = 0;
    for (auto &c : categories_) total += c.total_size();
    scan_page_->set_found(static_cast<int>(categories_.size()), total);
}

void FileCleanerWindow::scan_done(int gen, std::uint64_t /*total_found*/) {
    if (gen != scan_gen_) return;
    scanning_ = false;
    gtk_widget_set_sensitive(scan_btn_, TRUE);

    std::uint64_t total = 0;
    for (auto &c : categories_) total += c.total_size();
    scan_page_->set_done(static_cast<int>(categories_.size()), total);

    HistoryEntry entry;
    entry.id = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
    entry.date = iso_now();
    entry.mode = scan_mode_;
    entry.categories_found = static_cast<int>(categories_.size());
    entry.total_found = total;
    entry.total_deleted = 0;
    entry.deleted_count = 0;
    add_history_entry(entry);
    last_history_id_ = entry.id;

    run_on_main_delayed(alive_, 900, [this, gen]() {
        if (gen == scan_gen_) show_results();
    });
}

void FileCleanerWindow::show_results() {
    post_delete_ = false;

    if (categories_.empty()) {
        gtk_stack_set_visible_child_name(GTK_STACK(stack_), "scanning");
        gtk_label_set_label(GTK_LABEL(status_label_), "Filesystem uniform cleanly structure verified");
        scan_page_->set_title("Verified clean");
        scan_page_->set_path_label("No targeted elements observed inside ruleset criteria");
        return;
    }

    std::uint64_t total = 0;
    std::size_t total_items = 0;
    for (auto &c : categories_) {
        total += c.total_size();
        total_items += c.entries.size();
    }
    std::string status = "Review matrix: " + fmt_size(total) + " across " + std::to_string(total_items) +
                          " points";
    gtk_label_set_label(GTK_LABEL(status_label_), status.c_str());

    clear_results_list();

    GtkWidget *summary = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(summary, "summary-bar");
    GtkWidget *summary_label = gtk_label_new(nullptr);
    std::string markup = "<b>" + fmt_size(total) + "</b> indexed across <b>" +
                          std::to_string(categories_.size()) + "</b> areas";
    gtk_label_set_markup(GTK_LABEL(summary_label), markup.c_str());
    gtk_widget_add_css_class(summary_label, "summary-total");
    gtk_widget_set_halign(summary_label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(summary_label, TRUE);

    GtkWidget *none_btn = gtk_button_new_with_label("Deselect all");
    gtk_widget_add_css_class(none_btn, "flat");
    g_signal_connect_swapped(none_btn, "clicked",
                              G_CALLBACK(+[](FileCleanerWindow *self) { self->set_all_selected(false); }), this);
    GtkWidget *all_btn = gtk_button_new_with_label("Select all");
    gtk_widget_add_css_class(all_btn, "flat");
    g_signal_connect_swapped(all_btn, "clicked",
                              G_CALLBACK(+[](FileCleanerWindow *self) { self->set_all_selected(true); }), this);

    gtk_box_append(GTK_BOX(summary), summary_label);
    gtk_box_append(GTK_BOX(summary), none_btn);
    gtk_box_append(GTK_BOX(summary), all_btn);
    gtk_box_append(GTK_BOX(list_box_outer_), summary);

    std::vector<GtkWidget *> revealers;
    for (auto &cat : categories_) {
        add_category_group(cat, true);
        revealers.push_back(gtk_widget_get_last_child(list_box_outer_));
    }

    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "results");
    for (std::size_t i = 0; i < revealers.size(); ++i) {
        GtkWidget *rev = revealers[i];
        // Held alive independently of list_box_outer_'s ownership: if a new
        // scan clears the results list before this timer fires, the
        // revealer would otherwise have been finalized already.
        g_object_ref(rev);
        run_on_main_delayed(alive_, static_cast<unsigned>(i) * 80, [this, rev]() {
            reveal_group(rev);
            g_object_unref(rev);
        });
    }

    refresh_delete_btn();
}

void FileCleanerWindow::clear_results_list() {
    GtkWidget *child = gtk_widget_get_first_child(list_box_outer_);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(list_box_outer_), child);
        child = next;
    }
}

void FileCleanerWindow::reveal_group(GtkWidget *revealer) {
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), TRUE);
}

void FileCleanerWindow::add_category_group(Category &cat, bool animate) {
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, cat.title.c_str());
    std::string desc = cat.subtitle + "  ·  " + fmt_size(cat.total_size());
    adw_preferences_group_set_description(group, desc.c_str());

    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *select_all_btn = gtk_button_new_with_label("All");
    gtk_widget_add_css_class(select_all_btn, "flat");
    gtk_widget_add_css_class(select_all_btn, "caption");
    GtkWidget *select_none_btn = gtk_button_new_with_label("None");
    gtk_widget_add_css_class(select_none_btn, "flat");
    gtk_widget_add_css_class(select_none_btn, "caption");

    struct SelCtx {
        FileCleanerWindow *self;
        Category *cat;
        bool selected;
    };
    auto *sel_ctx_all = new SelCtx{this, &cat, true};
    g_signal_connect_data(
        select_all_btn, "clicked",
        G_CALLBACK(+[](GtkButton *, gpointer data) {
            auto *c = static_cast<SelCtx *>(data);
            c->self->select_all_in_cat(*c->cat, c->selected);
        }),
        sel_ctx_all, (GClosureNotify) + [](gpointer data, GClosure *) { delete static_cast<SelCtx *>(data); },
        (GConnectFlags)0);
    auto *sel_ctx_none = new SelCtx{this, &cat, false};
    g_signal_connect_data(
        select_none_btn, "clicked",
        G_CALLBACK(+[](GtkButton *, gpointer data) {
            auto *c = static_cast<SelCtx *>(data);
            c->self->select_all_in_cat(*c->cat, c->selected);
        }),
        sel_ctx_none, (GClosureNotify) + [](gpointer data, GClosure *) { delete static_cast<SelCtx *>(data); },
        (GConnectFlags)0);

    gtk_box_append(GTK_BOX(header_box), select_all_btn);
    gtk_box_append(GTK_BOX(header_box), select_none_btn);
    adw_preferences_group_set_header_suffix(group, header_box);

    for (auto &entry : cat.entries) {
        FileRow row(entry, [this](FileEntry &e, bool active) { on_file_toggle(e, active); });
        adw_preferences_group_add(group, row.widget());
    }

    GtkWidget *revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), 280);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), !animate);
    gtk_revealer_set_child(GTK_REVEALER(revealer), GTK_WIDGET(group));
    gtk_box_append(GTK_BOX(list_box_outer_), revealer);
}

void FileCleanerWindow::select_all_in_cat(Category &cat, bool selected) {
    for (auto &entry : cat.entries) entry.selected = selected;
    refresh_delete_btn();
}

void FileCleanerWindow::set_all_selected(bool selected) {
    for (auto &cat : categories_)
        for (auto &entry : cat.entries) entry.selected = selected;
    refresh_delete_btn();
}

void FileCleanerWindow::on_file_toggle(FileEntry &entry, bool active) {
    entry.selected = active;
    refresh_delete_btn();
}

void FileCleanerWindow::refresh_delete_btn() {
    std::uint64_t total = 0;
    for (auto &cat : categories_)
        for (auto &entry : cat.entries)
            if (entry.selected) total += entry.size;

    if (total > 0) {
        std::string label = "Clear " + fmt_size(total);
        gtk_button_set_label(GTK_BUTTON(delete_btn_), label.c_str());
        gtk_widget_set_sensitive(delete_btn_, TRUE);
    } else {
        gtk_button_set_label(GTK_BUTTON(delete_btn_), "Clear targets");
        gtk_widget_set_sensitive(delete_btn_, FALSE);
    }
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

// ---------------------------------------------------------------------------
// Delete workflow
// ---------------------------------------------------------------------------

void FileCleanerWindow::on_delete_clicked() {
    std::vector<fs::path> paths;
    std::uint64_t total = 0;
    for (auto &cat : categories_) {
        for (auto &entry : cat.entries) {
            if (entry.selected) {
                paths.push_back(entry.path);
                total += entry.size;
            }
        }
    }
    if (paths.empty()) return;

    std::string body = "Selected structures tracking " + fmt_size(total) + " will be unlinked permanently.";
    AdwMessageDialog *dialog = ADW_MESSAGE_DIALOG(
        adw_message_dialog_new(GTK_WINDOW(window_), "Execute permanent removal?", body.c_str()));
    adw_message_dialog_add_response(dialog, "cancel", "Cancel");
    std::string delete_label = "Drop " + fmt_size(total);
    adw_message_dialog_add_response(dialog, "delete", delete_label.c_str());
    adw_message_dialog_set_response_appearance(dialog, "delete", ADW_RESPONSE_DESTRUCTIVE);

    struct DeleteCtx {
        FileCleanerWindow *self;
        std::vector<fs::path> paths;
        std::uint64_t total;
    };
    auto *ctx = new DeleteCtx{this, std::move(paths), total};
    g_signal_connect_data(
        dialog, "response",
        G_CALLBACK(+[](AdwMessageDialog *d, const char *response, gpointer data) {
            auto *c = static_cast<DeleteCtx *>(data);
            if (std::string(response) == "delete") c->self->start_delete(c->paths, c->total);
            gtk_window_destroy(GTK_WINDOW(d));
        }),
        ctx, (GClosureNotify) + [](gpointer data, GClosure *) { delete static_cast<DeleteCtx *>(data); },
        (GConnectFlags)0);
    gtk_window_present(GTK_WINDOW(dialog));
}

void FileCleanerWindow::start_delete(std::vector<fs::path> paths, std::uint64_t total) {
    gtk_widget_set_sensitive(delete_btn_, FALSE);
    gtk_widget_set_sensitive(scan_btn_, FALSE);
    gtk_label_set_label(GTK_LABEL(status_label_), "Modifying file allocations…");
    std::thread(&FileCleanerWindow::delete_worker, this, std::move(paths), total).detach();
}

void FileCleanerWindow::delete_worker(std::vector<fs::path> paths, std::uint64_t deleted_total) {
    // `total` is computed on the main thread at confirmation time (see
    // on_delete_clicked) rather than here: this function runs on a
    // background thread, and categories_/FileEntry::selected can still be
    // mutated by the main thread (per-row checkboxes stay interactive
    // during a delete) — reading them from here would be a data race.
    DeleteResult result = delete_direct(paths);

    if (!result.permission_failed.empty() && geteuid() != 0) {
        DeleteResult pk = delete_with_pkexec(result.permission_failed);
        result.deleted_count += pk.deleted_count;
        result.errors.insert(result.errors.end(), pk.errors.begin(), pk.errors.end());
    } else if (!result.permission_failed.empty()) {
        for (auto &p : result.permission_failed) result.errors.push_back(p.filename().string() + ": permission denied");
    }

    int deleted_count = result.deleted_count;
    std::vector<std::string> errors = std::move(result.errors);
    run_on_main(alive_, [this, deleted_count, errors, deleted_total]() {
        delete_done(deleted_count, errors, deleted_total);
    });
}

void FileCleanerWindow::delete_done(int deleted_count, std::vector<std::string> errors,
                                     std::uint64_t deleted_total) {
    gtk_widget_set_sensitive(scan_btn_, TRUE);
    gtk_widget_set_sensitive(delete_btn_, TRUE);

    if (!errors.empty()) {
        std::string body;
        for (std::size_t i = 0; i < errors.size() && i < 8; ++i) {
            if (i) body += "\n";
            body += errors[i];
        }
        AdwMessageDialog *dlg = ADW_MESSAGE_DIALOG(
            adw_message_dialog_new(GTK_WINDOW(window_), "Write access faults encountered", body.c_str()));
        adw_message_dialog_add_response(dlg, "ok", "OK");
        g_signal_connect(dlg, "response", G_CALLBACK(+[](AdwMessageDialog *d, const char *, gpointer) {
                              gtk_window_destroy(GTK_WINDOW(d));
                          }),
                          nullptr);
        gtk_window_present(GTK_WINDOW(dlg));
    }

    if (deleted_count == 0 && errors.empty()) {
        gtk_label_set_label(GTK_LABEL(status_label_), "Modification process dropped");
        return;
    }

    std::string status = "Successfully dropped " + std::to_string(deleted_count) + " system tracking items";
    gtk_label_set_label(GTK_LABEL(status_label_), status.c_str());

    if (deleted_count > 0 && !last_history_id_.empty()) {
        auto entries = load_history();
        for (auto &e : entries) {
            if (e.id == last_history_id_) {
                e.total_deleted = deleted_total;
                e.deleted_count = deleted_count;
                break;
            }
        }
        save_history(entries);
    }

    if (deleted_count > 0 && settings_.auto_rescan) {
        post_delete_ = true;
        start_scan(scan_mode_, scan_root_);
    }
}

}  // namespace fc
