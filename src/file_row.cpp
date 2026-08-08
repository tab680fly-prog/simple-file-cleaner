#include "file_row.hpp"

#include <adwaita.h>
#include <glib.h>

#include <cstdlib>

namespace fc {

namespace {

void open_in_files(const fs::path &path) {
    std::error_code ec;
    fs::path parent = fs::is_directory(path, ec) ? path : path.parent_path();

    GError *error = nullptr;
    char *uri = g_filename_to_uri(parent.c_str(), nullptr, &error);
    if (uri) {
        GAppLaunchContext *ctx = nullptr;
        bool ok = g_app_info_launch_default_for_uri(uri, ctx, &error);
        g_free(uri);
        if (ok) return;
    }
    if (error) g_error_free(error);

    const char *argv[] = {"xdg-open", parent.c_str(), nullptr};
    g_spawn_async(nullptr, const_cast<char **>(argv), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr,
                  nullptr, nullptr);
}

struct RowCtx {
    FileEntry *entry;
    FileRow::ToggleCb on_toggle;
};

void on_check_toggled(GtkCheckButton *btn, gpointer user_data) {
    auto *ctx = static_cast<RowCtx *>(user_data);
    bool active = gtk_check_button_get_active(btn);
    ctx->entry->selected = active;
    if (ctx->on_toggle) ctx->on_toggle(*ctx->entry, active);
}

void on_open_clicked(GtkButton *, gpointer user_data) {
    auto *ctx = static_cast<RowCtx *>(user_data);
    open_in_files(ctx->entry->path);
}

void on_row_destroy(GtkWidget *, gpointer user_data) { delete static_cast<RowCtx *>(user_data); }

}  // namespace

FileRow::FileRow(FileEntry &entry, ToggleCb on_toggle) {
    row_ = adw_action_row_new();
    std::string short_name = entry.path.filename().string();
    if (short_name.empty()) short_name = entry.path.string();

    char *escaped_title = g_markup_escape_text(short_name.c_str(), -1);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row_), escaped_title);
    g_free(escaped_title);

    char *escaped_sub = g_markup_escape_text(display_path(entry.path).c_str(), -1);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row_), escaped_sub);
    g_free(escaped_sub);
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row_), 1);

    auto *ctx = new RowCtx{&entry, std::move(on_toggle)};
    g_signal_connect(row_, "destroy", G_CALLBACK(on_row_destroy), ctx);

    GtkWidget *check = gtk_check_button_new();
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), entry.selected);
    gtk_widget_set_valign(check, GTK_ALIGN_CENTER);
    g_signal_connect(check, "toggled", G_CALLBACK(on_check_toggled), ctx);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row_), check);

    std::error_code ec;
    const char *icon_name = fs::is_directory(entry.path, ec) ? "folder-symbolic" : "text-x-generic-symbolic";
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
    gtk_widget_add_css_class(icon, "dim-label");
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row_), icon);

    GtkWidget *size_label = gtk_label_new(fmt_size(entry.size).c_str());
    gtk_widget_set_valign(size_label, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(size_label, entry.size >= (500u << 20) ? "size-label-large" : "dim-label");
    gtk_widget_add_css_class(size_label, "caption");
    gtk_widget_add_css_class(size_label, "numeric");
    adw_action_row_add_suffix(ADW_ACTION_ROW(row_), size_label);

    GtkWidget *open_btn = gtk_button_new_from_icon_name("folder-open-symbolic");
    gtk_widget_set_valign(open_btn, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(open_btn, "flat");
    g_signal_connect(open_btn, "clicked", G_CALLBACK(on_open_clicked), ctx);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row_), open_btn);
}

}  // namespace fc
