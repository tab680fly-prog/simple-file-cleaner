#include "settings_window.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

#include "common.hpp"
#include "history.hpp"

namespace fc {

namespace {

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

bool contains_path(const std::vector<std::string> &list, const std::string &norm) {
    return std::any_of(list.begin(), list.end(),
                       [&](const std::string &existing) {
                           return path_key(path_from(expand_user_path(existing))) == path_key(path_from(norm));
                       });
}

// Fills `listbox` with one row per path, each with a remove button that
// calls `on_remove(index)`. Shows make_empty()'s row when there are none.
void fill_path_list(GtkWidget *listbox, const std::vector<std::string> &paths,
                    const std::function<GtkWidget *()> &make_empty, std::function<void(int)> on_remove) {
    clear_listbox(listbox);

    struct RemoveCtx {
        std::function<void(int)> fn;
        int index;
    };

    for (std::size_t i = 0; i < paths.size(); ++i) {
        GtkWidget *row = adw_action_row_new();
        char *escaped = g_markup_escape_text(display_path(paths[i]).c_str(), -1);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), escaped);
        g_free(escaped);

        GtkWidget *icon = gtk_image_new_from_icon_name("folder-symbolic");
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), icon);

        GtkWidget *del_btn = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_set_tooltip_text(del_btn, "Remove");
        gtk_widget_add_css_class(del_btn, "flat");
        gtk_widget_set_valign(del_btn, GTK_ALIGN_CENTER);
        auto *ctx = new RemoveCtx{on_remove, static_cast<int>(i)};
        g_signal_connect_data(
            del_btn, "clicked",
            G_CALLBACK(+[](GtkButton *, gpointer data) {
                auto *c = static_cast<RemoveCtx *>(data);
                // Copy first: fn() rebuilds the list, which destroys this
                // button and frees `c` via the closure notify below.
                auto fn = c->fn;
                int index = c->index;
                fn(index);
            }),
            ctx, (GClosureNotify) + [](gpointer data, GClosure *) { delete static_cast<RemoveCtx *>(data); },
            (GConnectFlags)0);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), del_btn);
        gtk_list_box_append(GTK_LIST_BOX(listbox), row);
    }

    if (paths.empty()) gtk_list_box_append(GTK_LIST_BOX(listbox), make_empty());
}

GtkWidget *make_empty_row(const char *title, const char *subtitle, const char *icon_name) {
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    if (icon_name) {
        GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), icon);
    }
    gtk_widget_add_css_class(row, "dim-label");
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    return row;
}

// An AdwEntryRow with "Browse…" and "Add" suffix buttons. Pressing Enter
// is equivalent to clicking Add.
GtkWidget *make_add_row(const char *title, GtkWidget **entry_out, GCallback on_add, GCallback on_browse,
                        gpointer self) {
    GtkWidget *row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    g_signal_connect_swapped(row, "entry-activated", on_add, self);

    GtkWidget *browse_btn = gtk_button_new_from_icon_name("folder-open-symbolic");
    gtk_widget_set_tooltip_text(browse_btn, "Browse…");
    gtk_widget_set_valign(browse_btn, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(browse_btn, "flat");
    g_signal_connect_swapped(browse_btn, "clicked", on_browse, self);
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(row), browse_btn);

    GtkWidget *add_btn = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(add_btn, "Add");
    gtk_widget_set_valign(add_btn, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(add_btn, "flat");
    g_signal_connect_swapped(add_btn, "clicked", on_add, self);
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(row), add_btn);

    *entry_out = row;
    return row;
}

}  // namespace

SettingsWindow::SettingsWindow(GtkWidget *parent, Settings &settings, std::function<void()> on_close)
    : parent_(parent), settings_(settings), on_close_(std::move(on_close)) {
    dialog_ = GTK_WIDGET(adw_preferences_dialog_new());
    adw_dialog_set_title(ADW_DIALOG(dialog_), "Preferences");
    adw_dialog_set_content_width(ADW_DIALOG(dialog_), 600);
    adw_dialog_set_content_height(ADW_DIALOG(dialog_), 640);
    adw_preferences_dialog_set_search_enabled(ADW_PREFERENCES_DIALOG(dialog_), FALSE);

    build();

    g_signal_connect(dialog_, "closed", G_CALLBACK(&SettingsWindow::on_closed), this);

    // The dialog owns this wrapper for the rest of its life: freed once the
    // widget itself is finalized, so callers don't need to manage it.
    g_object_set_data_full(G_OBJECT(dialog_), "fc-cpp-wrapper", this,
                            +[](gpointer p) { delete static_cast<SettingsWindow *>(p); });
}

void SettingsWindow::present(const char *page_name) {
    if (page_name) adw_preferences_dialog_set_visible_page_name(ADW_PREFERENCES_DIALOG(dialog_), page_name);
    adw_dialog_present(ADW_DIALOG(dialog_), parent_);
}

void SettingsWindow::on_closed(AdwDialog *, gpointer user_data) {
    auto *self = static_cast<SettingsWindow *>(user_data);
    self->settings_.save();
    if (self->on_close_) self->on_close_();
}

void SettingsWindow::toast(const std::string &text) {
    adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(dialog_), adw_toast_new(text.c_str()));
}

void SettingsWindow::build() {
    AdwPreferencesDialog *dlg = ADW_PREFERENCES_DIALOG(dialog_);

    // -------------------------------------------------------------- Scanning
    AdwPreferencesPage *scan_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(scan_page, "Scanning");
    adw_preferences_page_set_name(scan_page, "scanning");
    adw_preferences_page_set_icon_name(scan_page, "system-search-symbolic");
    adw_preferences_dialog_add(dlg, scan_page);

    AdwPreferencesGroup *cat_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(cat_group, "Categories");
    adw_preferences_group_set_description(cat_group, "Select the types of files that scans should look for.");
    adw_preferences_page_add(scan_page, cat_group);

    struct SwitchCtx {
        SettingsWindow *self;
        std::string key;
    };
    for (const auto &[key, label] : SCAN_CATEGORIES) {
        GtkWidget *row = adw_switch_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
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

    // ------------------------------------------------------ Custom locations
    AdwPreferencesPage *custom_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(custom_page, "Locations");
    adw_preferences_page_set_name(custom_page, "custom");
    adw_preferences_page_set_icon_name(custom_page, "folder-saved-search-symbolic");
    adw_preferences_dialog_add(dlg, custom_page);

    AdwPreferencesGroup *custom_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(custom_group, "Additional Scan Locations");
    adw_preferences_group_set_description(
        custom_group,
        "Files and folders added here are included in every quick and deep scan, and their entire "
        "contents are offered for deletion.");
    adw_preferences_page_add(custom_page, custom_group);

    custom_listbox_ = gtk_list_box_new();
    gtk_widget_add_css_class(custom_listbox_, "boxed-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(custom_listbox_), GTK_SELECTION_NONE);
    adw_preferences_group_add(custom_group, custom_listbox_);
    refresh_custom_list();

    AdwPreferencesGroup *custom_add_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_add(
        custom_add_group,
        make_add_row(IS_WINDOWS ? "Add a location (for example, %LOCALAPPDATA%\\Example\\Cache)"
                                : "Add a location (for example, ~/.cache/example)", &custom_entry_,
                     G_CALLBACK(+[](SettingsWindow *self) {
                         std::string text = gtk_editable_get_text(GTK_EDITABLE(self->custom_entry_));
                         self->add_custom_path(text);
                     }),
                     G_CALLBACK(+[](SettingsWindow *self) {
                         self->browse_for_folder([self](const std::string &p) { self->add_custom_path(p); });
                     }),
                     this));
    adw_preferences_page_add(custom_page, custom_add_group);

    // ------------------------------------------------------------ Exclusions
    AdwPreferencesPage *excl_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(excl_page, "Exclusions");
    adw_preferences_page_set_name(excl_page, "exclusions");
    adw_preferences_page_set_icon_name(excl_page, "changes-prevent-symbolic");
    adw_preferences_dialog_add(dlg, excl_page);

    AdwPreferencesGroup *excl_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(excl_group, "Protected Locations");
    adw_preferences_group_set_description(
        excl_group,
        "Files and folders listed here, including everything inside them, are never scanned or deleted. "
        "Folders that contain a protected location are also left untouched.");
    adw_preferences_page_add(excl_page, excl_group);

    excl_listbox_ = gtk_list_box_new();
    gtk_widget_add_css_class(excl_listbox_, "boxed-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(excl_listbox_), GTK_SELECTION_NONE);
    adw_preferences_group_add(excl_group, excl_listbox_);
    refresh_excl_list();

    AdwPreferencesGroup *excl_add_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_add(
        excl_add_group,
        make_add_row(IS_WINDOWS ? "Add a location (for example, ~\\Documents)"
                                : "Add a location (for example, ~/Documents)", &excl_entry_,
                     G_CALLBACK(+[](SettingsWindow *self) {
                         std::string text = gtk_editable_get_text(GTK_EDITABLE(self->excl_entry_));
                         self->add_excl_path(text);
                     }),
                     G_CALLBACK(+[](SettingsWindow *self) {
                         self->browse_for_folder([self](const std::string &p) { self->add_excl_path(p); });
                     }),
                     this));
    adw_preferences_page_add(excl_page, excl_add_group);

    // ------------------------------------------------------------ Appearance
    AdwPreferencesPage *appear_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(appear_page, "Appearance");
    adw_preferences_page_set_name(appear_page, "appearance");
    adw_preferences_page_set_icon_name(appear_page, "applications-graphics-symbolic");
    adw_preferences_dialog_add(dlg, appear_page);

    AdwPreferencesGroup *anim_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(anim_group, "Scan Animation");
    adw_preferences_group_set_description(anim_group, "Select the animation displayed while a scan is in progress.");
    adw_preferences_page_add(appear_page, anim_group);

    GtkWidget *mag_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(mag_row), "Magnifying glass");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(mag_row), "Default · A magnifying glass that circles the indicator");
    GtkWidget *mag_radio = gtk_check_button_new();
    gtk_widget_set_valign(mag_radio, GTK_ALIGN_CENTER);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(mag_radio), settings_.animation != "spinner");
    adw_action_row_add_prefix(ADW_ACTION_ROW(mag_row), mag_radio);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(mag_row), mag_radio);
    adw_preferences_group_add(anim_group, mag_row);

    GtkWidget *spin_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(spin_row), "Spinner");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(spin_row), "A minimal rotating arc");
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

    AdwPreferencesGroup *behavior_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(behavior_group, "Behavior");
    adw_preferences_page_add(appear_page, behavior_group);

    GtkWidget *rescan_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(rescan_row), "Scan again after deleting");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(rescan_row),
                                 "Automatically repeat the previous scan once the selected files have been deleted");
    adw_switch_row_set_active(ADW_SWITCH_ROW(rescan_row), settings_.auto_rescan);
    g_signal_connect_swapped(rescan_row, "notify::active",
                              G_CALLBACK(+[](SettingsWindow *self, GParamSpec *, GObject *obj) {
                                  self->settings_.auto_rescan = adw_switch_row_get_active(ADW_SWITCH_ROW(obj));
                              }),
                              this);
    adw_preferences_group_add(behavior_group, rescan_row);

    // --------------------------------------------------------------- History
    AdwPreferencesPage *history_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(history_page, "History");
    adw_preferences_page_set_name(history_page, "history");
    adw_preferences_page_set_icon_name(history_page, "document-open-recent-symbolic");
    adw_preferences_dialog_add(dlg, history_page);

    hist_group_ = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(hist_group_, "Scan History");
    adw_preferences_group_set_description(hist_group_, "The 50 most recent scans and the space recovered by each.");
    adw_preferences_page_add(history_page, hist_group_);

    GtkWidget *clear_btn = gtk_button_new_with_label("Clear History");
    gtk_widget_add_css_class(clear_btn, "flat");
    gtk_widget_add_css_class(clear_btn, "error");
    gtk_widget_set_valign(clear_btn, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(
        clear_btn, "clicked", G_CALLBACK(+[](SettingsWindow *self) {
            AdwAlertDialog *alert = ADW_ALERT_DIALOG(
                adw_alert_dialog_new("Clear Scan History?", "All scan records will be permanently deleted."));
            adw_alert_dialog_add_response(alert, "cancel", "Cancel");
            adw_alert_dialog_add_response(alert, "clear", "Clear History");
            adw_alert_dialog_set_response_appearance(alert, "clear", ADW_RESPONSE_DESTRUCTIVE);
            adw_alert_dialog_set_default_response(alert, "cancel");
            adw_alert_dialog_set_close_response(alert, "cancel");
            g_signal_connect(alert, "response",
                             G_CALLBACK(+[](AdwAlertDialog *, const char *response, gpointer data) {
                                 auto *self2 = static_cast<SettingsWindow *>(data);
                                 if (std::string(response) == "clear") {
                                     save_history({});
                                     self2->populate_history();
                                     self2->toast("Scan history cleared");
                                 }
                             }),
                             self);
            adw_dialog_present(ADW_DIALOG(alert), self->dialog_);
        }),
        this);
    adw_preferences_group_set_header_suffix(hist_group_, clear_btn);

    populate_history();
}

void SettingsWindow::refresh_excl_list() {
    fill_path_list(excl_listbox_, settings_.excluded_paths,
                   [] {
                       return make_empty_row("No locations are protected",
                                             "Adding folders that contain important files is strongly recommended.",
                                             "dialog-warning-symbolic");
                   },
                   [this](int i) { remove_excl_path(i); });
}

void SettingsWindow::refresh_custom_list() {
    fill_path_list(custom_listbox_, settings_.custom_scan_paths,
                   [] { return make_empty_row("No additional locations have been added", nullptr, nullptr); },
                   [this](int i) { remove_custom_path(i); });
}

void SettingsWindow::add_excl_path(const std::string &path) {
    std::string norm = expand_user_path(path);
    if (norm.empty()) return;
    if (!path_from(norm).is_absolute()) {
        toast(IS_WINDOWS ? "Enter a full path, such as C:\\Users\\Name\\Documents"
                         : "Enter a full path, such as ~/Documents");
        return;
    }
    if (contains_path(settings_.excluded_paths, norm)) {
        toast("This location is already protected");
        return;
    }
    settings_.excluded_paths.push_back(norm);
    gtk_editable_set_text(GTK_EDITABLE(excl_entry_), "");
    refresh_excl_list();
}

void SettingsWindow::remove_excl_path(int index) {
    if (index >= 0 && static_cast<std::size_t>(index) < settings_.excluded_paths.size())
        settings_.excluded_paths.erase(settings_.excluded_paths.begin() + index);
    refresh_excl_list();
}

void SettingsWindow::add_custom_path(const std::string &path) {
    std::string norm = expand_user_path(path);
    if (norm.empty()) return;
    if (!path_from(norm).is_absolute()) {
        toast(IS_WINDOWS ? "Enter a full path, such as %LOCALAPPDATA%\\Example\\Cache"
                         : "Enter a full path, such as ~/.cache/example");
        return;
    }
    if (is_protected_path(path_from(norm))) {
        toast("This location cannot be added because it is your home folder, a system folder or a drive");
        return;
    }
    if (contains_path(settings_.custom_scan_paths, norm)) {
        toast("This location has already been added");
        return;
    }
    settings_.custom_scan_paths.push_back(norm);
    gtk_editable_set_text(GTK_EDITABLE(custom_entry_), "");
    refresh_custom_list();

    std::error_code ec;
    if (settings_.must_preserve(path_from(norm)))
        toast("Added, but this location is protected by an exclusion and will be skipped");
    else if (!fs::exists(path_from(norm), ec))
        toast("Added, but this location does not currently exist");
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
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "No scans have been recorded");
        gtk_widget_add_css_class(row, "dim-label");
        adw_preferences_group_add(hist_group_, row);
        hist_rows_.push_back(row);
        return;
    }

    std::size_t limit = std::min<std::size_t>(entries.size(), 50);
    for (std::size_t i = 0; i < limit; ++i) {
        const auto &entry = entries[i];
        std::string mode_str = entry.mode == "quick"    ? "Quick Scan"
                               : entry.mode == "deep"   ? "Deep Scan"
                               : entry.mode == "folder" ? "Folder Scan"
                                                        : capitalize(entry.mode);

        GtkWidget *row = adw_action_row_new();
        std::string title = mode_str + " · " + entry.total_found_fmt() + " found";
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title.c_str());

        std::string deleted_str;
        if (entry.deleted_count > 0)
            deleted_str = " · " + std::to_string(entry.deleted_count) +
                          (entry.deleted_count == 1 ? " item" : " items") + " deleted (" +
                          entry.total_deleted_fmt() + ")";
        std::string subtitle = entry.date_fmt() + deleted_str;
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());

        const char *icon_name = entry.deleted_count > 0 ? "user-trash-symbolic" : "system-search-symbolic";
        GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
        gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), icon);

        adw_preferences_group_add(hist_group_, row);
        hist_rows_.push_back(row);
    }
}

void SettingsWindow::browse_for_folder(std::function<void(const std::string &)> on_chosen) {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select a Folder");

    struct BrowseCtx {
        std::function<void(const std::string &)> cb;
        GtkWidget *keep_alive;
    };
    // Hold a reference so the callback is safe even if the preferences
    // dialog is closed while the file chooser is open.
    auto *ctx = new BrowseCtx{std::move(on_chosen), GTK_WIDGET(g_object_ref(dialog_))};

    GtkRoot *root = gtk_widget_get_root(dialog_);
    gtk_file_dialog_select_folder(
        dialog, root ? GTK_WINDOW(root) : nullptr, nullptr,
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
            g_object_unref(c->keep_alive);
            delete c;
            g_object_unref(source);
        },
        ctx);
}

}  // namespace fc
