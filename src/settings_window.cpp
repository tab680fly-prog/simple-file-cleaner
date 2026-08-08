#include "settings_window.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

#include "history.hpp"

namespace fc {

namespace {

std::string trim(const std::string &s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string capitalize(std::string s) {
    if (!s.empty()) s[0] = std::toupper(static_cast<unsigned char>(s[0]));
    return s;
}

void clear_listbox(GtkWidget *listbox) {
    GtkWidget *child = gtk_widget_get_first_child(listbox);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(listbox), child);
        child = next;
    }
}

}  // namespace

SettingsWindow::SettingsWindow(GtkWindow *parent, Settings &settings, std::function<void()> on_close)
    : settings_(settings), on_close_(std::move(on_close)) {
    window_ = adw_preferences_window_new();
    gtk_window_set_transient_for(GTK_WINDOW(window_), parent);
    gtk_window_set_modal(GTK_WINDOW(window_), TRUE);
    gtk_window_set_title(GTK_WINDOW(window_), "Preferences");
    gtk_window_set_default_size(GTK_WINDOW(window_), 520, 600);

    build();

    g_signal_connect(window_, "close-request", G_CALLBACK(&SettingsWindow::on_close_request), this);

    // The window owns this wrapper for the rest of its life: freed once the
    // GTK widget itself is finalized, so callers don't need to manage it.
    g_object_set_data_full(G_OBJECT(window_), "fc-cpp-wrapper", this,
                            +[](gpointer p) { delete static_cast<SettingsWindow *>(p); });
}

void SettingsWindow::present() { gtk_window_present(GTK_WINDOW(window_)); }

gboolean SettingsWindow::on_close_request(GtkWindow *, gpointer user_data) {
    auto *self = static_cast<SettingsWindow *>(user_data);
    self->settings_.save();
    if (self->on_close_) self->on_close_();
    return FALSE;
}

void SettingsWindow::build() {
    // -------------------------------------------------------------- Page 1
    AdwPreferencesPage *scan_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(scan_page, "Scanning");
    adw_preferences_page_set_icon_name(scan_page, "folder-saved-search-symbolic");
    adw_preferences_window_add(ADW_PREFERENCES_WINDOW(window_), scan_page);

    AdwPreferencesGroup *cat_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(cat_group, "Categories");
    adw_preferences_group_set_description(
        cat_group, "Uncheck specific runtime paths to omit them from analysis tasks");
    adw_preferences_page_add(scan_page, cat_group);

    struct SwitchCtx {
        SettingsWindow *self;
        std::string key;
    };
    for (const auto &[key, label] : SCAN_CATEGORIES) {
        GtkWidget *row = adw_switch_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label.c_str());
        adw_switch_row_set_active(ADW_SWITCH_ROW(row), settings_.cat_on(key));
        auto *ctx = new SwitchCtx{this, key};
        g_signal_connect_data(
            row, "notify::active",
            G_CALLBACK(+[](GObject *obj, GParamSpec *, gpointer data) {
                auto *c = static_cast<SwitchCtx *>(data);
                c->self->settings_.enabled[c->key] = adw_switch_row_get_active(ADW_SWITCH_ROW(obj));
            }),
            ctx, (GClosureNotify) + [](gpointer data, GClosure *) { delete static_cast<SwitchCtx *>(data); },
            (GConnectFlags)0);
        adw_preferences_group_add(cat_group, row);
    }

    // -------------------------------------------------------------- Page 2
    AdwPreferencesPage *custom_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(custom_page, "Custom Rules");
    adw_preferences_page_set_icon_name(custom_page, "edit-find-symbolic");
    adw_preferences_window_add(ADW_PREFERENCES_WINDOW(window_), custom_page);

    AdwPreferencesGroup *custom_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(custom_group, "Custom Scanning Locations");
    adw_preferences_group_set_description(
        custom_group, "Register files or folders to be matched dynamically during scan routines");
    adw_preferences_page_add(custom_page, custom_group);

    custom_listbox_ = gtk_list_box_new();
    gtk_widget_add_css_class(custom_listbox_, "boxed-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(custom_listbox_), GTK_SELECTION_NONE);
    adw_preferences_group_add(custom_group, custom_listbox_);
    refresh_custom_list();

    GtkWidget *custom_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(custom_row), "Target new path");

    custom_entry_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(custom_entry_), "/home/user/.cache/app_logs");
    gtk_widget_set_hexpand(custom_entry_, TRUE);
    gtk_widget_set_valign(custom_entry_, GTK_ALIGN_CENTER);

    GtkWidget *custom_add_btn = gtk_button_new_with_label("Add");
    gtk_widget_add_css_class(custom_add_btn, "suggested-action");
    gtk_widget_set_valign(custom_add_btn, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(
        custom_add_btn, "clicked",
        G_CALLBACK(+[](SettingsWindow *self) {
            std::string text = trim(gtk_editable_get_text(GTK_EDITABLE(self->custom_entry_)));
            if (!text.empty() &&
                std::find(self->settings_.custom_scan_paths.begin(), self->settings_.custom_scan_paths.end(),
                          text) == self->settings_.custom_scan_paths.end()) {
                self->settings_.custom_scan_paths.push_back(text);
                gtk_editable_set_text(GTK_EDITABLE(self->custom_entry_), "");
                self->refresh_custom_list();
            }
        }),
        this);

    GtkWidget *custom_browse_btn = gtk_button_new_from_icon_name("folder-open-symbolic");
    gtk_widget_set_valign(custom_browse_btn, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(custom_browse_btn, "flat");
    g_signal_connect_swapped(custom_browse_btn, "clicked",
                              G_CALLBACK(+[](SettingsWindow *self) {
                                  self->browse_for_folder([self](const std::string &p) {
                                      self->add_custom_path(p);
                                  });
                              }),
                              this);

    adw_action_row_add_suffix(ADW_ACTION_ROW(custom_row), custom_entry_);
    adw_action_row_add_suffix(ADW_ACTION_ROW(custom_row), custom_browse_btn);
    adw_action_row_add_suffix(ADW_ACTION_ROW(custom_row), custom_add_btn);
    adw_preferences_group_add(custom_group, custom_row);

    // -------------------------------------------------------------- Page 3
    AdwPreferencesPage *excl_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(excl_page, "Exclusions");
    adw_preferences_page_set_icon_name(excl_page, "changes-prevent-symbolic");
    adw_preferences_window_add(ADW_PREFERENCES_WINDOW(window_), excl_page);

    AdwPreferencesGroup *excl_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(excl_group, "Excluded paths");
    adw_preferences_group_set_description(
        excl_group, "Target rules defined here preserve matching directories from modification");
    adw_preferences_page_add(excl_page, excl_group);

    excl_listbox_ = gtk_list_box_new();
    gtk_widget_add_css_class(excl_listbox_, "boxed-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(excl_listbox_), GTK_SELECTION_NONE);
    adw_preferences_group_add(excl_group, excl_listbox_);
    refresh_excl_list();

    GtkWidget *add_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add_row), "Add exclusion path");

    excl_entry_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(excl_entry_), "/home/user/development");
    gtk_widget_set_hexpand(excl_entry_, TRUE);
    gtk_widget_set_valign(excl_entry_, GTK_ALIGN_CENTER);

    GtkWidget *add_btn = gtk_button_new_with_label("Add");
    gtk_widget_add_css_class(add_btn, "suggested-action");
    gtk_widget_set_valign(add_btn, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(
        add_btn, "clicked",
        G_CALLBACK(+[](SettingsWindow *self) {
            std::string text = trim(gtk_editable_get_text(GTK_EDITABLE(self->excl_entry_)));
            if (!text.empty() &&
                std::find(self->settings_.excluded_paths.begin(), self->settings_.excluded_paths.end(),
                          text) == self->settings_.excluded_paths.end()) {
                self->settings_.excluded_paths.push_back(text);
                gtk_editable_set_text(GTK_EDITABLE(self->excl_entry_), "");
                self->refresh_excl_list();
            }
        }),
        this);

    GtkWidget *browse_btn = gtk_button_new_from_icon_name("folder-open-symbolic");
    gtk_widget_set_tooltip_text(browse_btn, "Browse…");
    gtk_widget_set_valign(browse_btn, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(browse_btn, "flat");
    g_signal_connect_swapped(
        browse_btn, "clicked",
        G_CALLBACK(+[](SettingsWindow *self) {
            self->browse_for_folder([self](const std::string &p) { self->add_excl_path(p); });
        }),
        this);

    adw_action_row_add_suffix(ADW_ACTION_ROW(add_row), excl_entry_);
    adw_action_row_add_suffix(ADW_ACTION_ROW(add_row), browse_btn);
    adw_action_row_add_suffix(ADW_ACTION_ROW(add_row), add_btn);
    adw_preferences_group_add(excl_group, add_row);

    // -------------------------------------------------------------- Page 4
    AdwPreferencesPage *appear_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(appear_page, "Appearance");
    adw_preferences_page_set_icon_name(appear_page, "applications-graphics-symbolic");
    adw_preferences_window_add(ADW_PREFERENCES_WINDOW(window_), appear_page);

    AdwPreferencesGroup *anim_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(anim_group, "Scan animation");
    adw_preferences_group_set_description(anim_group, "Choose interface rendering pattern utilized during scans");
    adw_preferences_page_add(appear_page, anim_group);

    GtkWidget *mag_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(mag_row), "Magnifying glass");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(mag_row), "Filled circle with tracking loop");
    GtkWidget *mag_radio = gtk_check_button_new();
    gtk_widget_set_valign(mag_radio, GTK_ALIGN_CENTER);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(mag_radio), settings_.animation == "magnifier");
    adw_action_row_add_prefix(ADW_ACTION_ROW(mag_row), mag_radio);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(mag_row), mag_radio);
    adw_preferences_group_add(anim_group, mag_row);

    GtkWidget *spin_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(spin_row), "Spinner");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(spin_row), "Clean architectural loading vector");
    GtkWidget *spin_radio = gtk_check_button_new();
    gtk_widget_set_valign(spin_radio, GTK_ALIGN_CENTER);
    gtk_check_button_set_group(GTK_CHECK_BUTTON(spin_radio), GTK_CHECK_BUTTON(mag_radio));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(spin_radio), settings_.animation == "spinner");
    adw_action_row_add_prefix(ADW_ACTION_ROW(spin_row), spin_radio);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(spin_row), spin_radio);
    adw_preferences_group_add(anim_group, spin_row);

    g_signal_connect_swapped(mag_radio, "toggled",
                              G_CALLBACK(+[](SettingsWindow *self, GtkCheckButton *b) {
                                  if (gtk_check_button_get_active(b)) self->settings_.animation = "magnifier";
                              }),
                              this);
    g_signal_connect_swapped(spin_radio, "toggled",
                              G_CALLBACK(+[](SettingsWindow *self, GtkCheckButton *b) {
                                  if (gtk_check_button_get_active(b)) self->settings_.animation = "spinner";
                              }),
                              this);

    AdwPreferencesGroup *behaviour_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(behaviour_group, "Behaviour");
    adw_preferences_page_add(appear_page, behaviour_group);

    GtkWidget *rescan_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(rescan_row), "Rescan automatically after deleting");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(rescan_row),
                                 "Refreshes active mode targets instantly to present state updates");
    adw_switch_row_set_active(ADW_SWITCH_ROW(rescan_row), settings_.auto_rescan);
    g_signal_connect_swapped(rescan_row, "notify::active",
                              G_CALLBACK(+[](SettingsWindow *self, GObject *obj) {
                                  self->settings_.auto_rescan =
                                      adw_switch_row_get_active(ADW_SWITCH_ROW(obj));
                              }),
                              this);
    adw_preferences_group_add(behaviour_group, rescan_row);

    // -------------------------------------------------------------- Page 5
    AdwPreferencesPage *history_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(history_page, "History");
    adw_preferences_page_set_icon_name(history_page, "document-open-recent-symbolic");
    adw_preferences_window_add(ADW_PREFERENCES_WINDOW(window_), history_page);

    hist_group_ = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(hist_group_, "Scan records");
    adw_preferences_group_set_description(hist_group_, "Historical telemetry items indexed over time");
    adw_preferences_page_add(history_page, hist_group_);

    GtkWidget *clear_btn = gtk_button_new_with_label("Clear records");
    gtk_widget_add_css_class(clear_btn, "destructive-action");
    gtk_widget_set_valign(clear_btn, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(clear_btn, "clicked",
                              G_CALLBACK(+[](SettingsWindow *self) {
                                  AdwMessageDialog *dialog = ADW_MESSAGE_DIALOG(adw_message_dialog_new(
                                      GTK_WINDOW(self->window_), "Clear records?",
                                      "Historical data logs will be dropped permanently."));
                                  adw_message_dialog_add_response(dialog, "cancel", "Cancel");
                                  adw_message_dialog_add_response(dialog, "clear", "Clear");
                                  adw_message_dialog_set_response_appearance(dialog, "clear",
                                                                              ADW_RESPONSE_DESTRUCTIVE);
                                  g_signal_connect(
                                      dialog, "response",
                                      G_CALLBACK(+[](AdwMessageDialog *d, const char *response, gpointer data) {
                                          auto *self2 = static_cast<SettingsWindow *>(data);
                                          if (std::string(response) == "clear") {
                                              save_history({});
                                              self2->populate_history();
                                          }
                                          gtk_window_destroy(GTK_WINDOW(d));
                                      }),
                                      self);
                                  gtk_window_present(GTK_WINDOW(dialog));
                              }),
                              this);
    adw_preferences_group_set_header_suffix(hist_group_, clear_btn);

    populate_history();
}

void SettingsWindow::refresh_excl_list() {
    clear_listbox(excl_listbox_);

    struct RemoveCtx {
        SettingsWindow *self;
        int index;
    };

    for (std::size_t i = 0; i < settings_.excluded_paths.size(); ++i) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), settings_.excluded_paths[i].c_str());
        GtkWidget *del_btn = gtk_button_new_from_icon_name("list-remove-symbolic");
        gtk_widget_add_css_class(del_btn, "flat");
        gtk_widget_add_css_class(del_btn, "destructive-action");
        gtk_widget_set_valign(del_btn, GTK_ALIGN_CENTER);
        auto *ctx = new RemoveCtx{this, static_cast<int>(i)};
        g_signal_connect_data(
            del_btn, "clicked",
            G_CALLBACK(+[](GtkButton *, gpointer data) {
                auto *c = static_cast<RemoveCtx *>(data);
                c->self->remove_excl_path(c->index);
            }),
            ctx, (GClosureNotify) + [](gpointer data, GClosure *) { delete static_cast<RemoveCtx *>(data); },
            (GConnectFlags)0);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), del_btn);
        gtk_list_box_append(GTK_LIST_BOX(excl_listbox_), row);
    }

    if (settings_.excluded_paths.empty()) {
        GtkWidget *empty_row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(empty_row), "No preservation paths configured");
        gtk_widget_set_sensitive(empty_row, FALSE);
        gtk_list_box_append(GTK_LIST_BOX(excl_listbox_), empty_row);
    }
}

void SettingsWindow::refresh_custom_list() {
    clear_listbox(custom_listbox_);

    struct RemoveCtx {
        SettingsWindow *self;
        int index;
    };

    for (std::size_t i = 0; i < settings_.custom_scan_paths.size(); ++i) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), settings_.custom_scan_paths[i].c_str());
        GtkWidget *del_btn = gtk_button_new_from_icon_name("list-remove-symbolic");
        gtk_widget_add_css_class(del_btn, "flat");
        gtk_widget_add_css_class(del_btn, "destructive-action");
        gtk_widget_set_valign(del_btn, GTK_ALIGN_CENTER);
        auto *ctx = new RemoveCtx{this, static_cast<int>(i)};
        g_signal_connect_data(
            del_btn, "clicked",
            G_CALLBACK(+[](GtkButton *, gpointer data) {
                auto *c = static_cast<RemoveCtx *>(data);
                c->self->remove_custom_path(c->index);
            }),
            ctx, (GClosureNotify) + [](gpointer data, GClosure *) { delete static_cast<RemoveCtx *>(data); },
            (GConnectFlags)0);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), del_btn);
        gtk_list_box_append(GTK_LIST_BOX(custom_listbox_), row);
    }

    if (settings_.custom_scan_paths.empty()) {
        GtkWidget *empty_row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(empty_row), "No custom targets defined");
        gtk_widget_set_sensitive(empty_row, FALSE);
        gtk_list_box_append(GTK_LIST_BOX(custom_listbox_), empty_row);
    }
}

void SettingsWindow::add_excl_path(const std::string &path) {
    if (!path.empty() &&
        std::find(settings_.excluded_paths.begin(), settings_.excluded_paths.end(), path) ==
            settings_.excluded_paths.end()) {
        settings_.excluded_paths.push_back(path);
        refresh_excl_list();
    }
}

void SettingsWindow::remove_excl_path(int index) {
    if (index >= 0 && static_cast<std::size_t>(index) < settings_.excluded_paths.size())
        settings_.excluded_paths.erase(settings_.excluded_paths.begin() + index);
    refresh_excl_list();
}

void SettingsWindow::add_custom_path(const std::string &path) {
    if (!path.empty() &&
        std::find(settings_.custom_scan_paths.begin(), settings_.custom_scan_paths.end(), path) ==
            settings_.custom_scan_paths.end()) {
        settings_.custom_scan_paths.push_back(path);
        refresh_custom_list();
    }
}

void SettingsWindow::remove_custom_path(int index) {
    if (index >= 0 && static_cast<std::size_t>(index) < settings_.custom_scan_paths.size())
        settings_.custom_scan_paths.erase(settings_.custom_scan_paths.begin() + index);
    refresh_custom_list();
}

void SettingsWindow::populate_history() {
    for (GtkWidget *row : hist_rows_) adw_preferences_group_remove(hist_group_, row);
    hist_rows_.clear();

    auto entries = load_history();
    if (entries.empty()) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "No execution records indexed");
        gtk_widget_set_sensitive(row, FALSE);
        adw_preferences_group_add(hist_group_, row);
        hist_rows_.push_back(row);
        return;
    }

    std::size_t limit = std::min<std::size_t>(entries.size(), 50);
    for (std::size_t i = 0; i < limit; ++i) {
        const auto &entry = entries[i];
        std::string mode_str = entry.mode == "quick"   ? "Quick scan"
                                : entry.mode == "deep"  ? "Deep scan"
                                : entry.mode == "folder" ? "Folder scan"
                                                          : capitalize(entry.mode);

        GtkWidget *row = adw_action_row_new();
        std::string title = mode_str + " — " + entry.total_found_fmt() + " mapped";
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title.c_str());

        std::string deleted_str;
        if (entry.deleted_count > 0)
            deleted_str = " · Cleared " + std::to_string(entry.deleted_count) + " paths (" +
                          entry.total_deleted_fmt() + ")";
        std::string subtitle = entry.date_fmt() + deleted_str;
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());

        GtkWidget *icon = gtk_image_new_from_icon_name("folder-saved-search-symbolic");
        gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), icon);

        adw_preferences_group_add(hist_group_, row);
        hist_rows_.push_back(row);
    }
}

void SettingsWindow::browse_for_folder(std::function<void(const std::string &)> on_chosen) {
    GtkFileDialog *dialog = gtk_file_dialog_new();

    struct BrowseCtx {
        std::function<void(const std::string &)> cb;
    };
    auto *ctx = new BrowseCtx{std::move(on_chosen)};

    gtk_file_dialog_select_folder(
        dialog, GTK_WINDOW(window_), nullptr,
        +[](GObject *source, GAsyncResult *result, gpointer user_data) {
            auto *c = static_cast<BrowseCtx *>(user_data);
            GError *error = nullptr;
            GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
            if (folder) {
                char *path = g_file_get_path(folder);
                if (path) {
                    c->cb(path);
                    g_free(path);
                }
                g_object_unref(folder);
            }
            if (error) g_error_free(error);
            delete c;
        },
        ctx);
}

}  // namespace fc
