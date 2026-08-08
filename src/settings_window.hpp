#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>

#include <functional>
#include <vector>

#include "settings.hpp"

namespace fc {

// Preferences window: Scanning categories, Custom Rules, Exclusions,
// Appearance, History. Mirrors the original Adw.PreferencesWindow with 5
// pages. Settings are saved to disk when the window closes.
class SettingsWindow {
   public:
    SettingsWindow(GtkWindow *parent, Settings &settings, std::function<void()> on_close);

    void present();

   private:
    GtkWidget *window_;
    Settings &settings_;
    std::function<void()> on_close_;

    GtkWidget *excl_listbox_ = nullptr;
    GtkWidget *custom_listbox_ = nullptr;
    GtkWidget *excl_entry_ = nullptr;
    GtkWidget *custom_entry_ = nullptr;
    AdwPreferencesGroup *hist_group_ = nullptr;
    // Rows previously adw_preferences_group_add()-ed to hist_group_, tracked
    // explicitly so populate_history() can remove exactly what it added
    // rather than walking the group's internal widget tree (which mixes in
    // title/description/layout widgets not suitable for
    // adw_preferences_group_remove()).
    std::vector<GtkWidget *> hist_rows_;

    void build();
    void refresh_excl_list();
    void refresh_custom_list();
    void populate_history();

    void add_excl_path(const std::string &path);
    void remove_excl_path(int index);
    void add_custom_path(const std::string &path);
    void remove_custom_path(int index);
    void browse_for_folder(std::function<void(const std::string &)> on_chosen);

    static gboolean on_close_request(GtkWindow *window, gpointer user_data);
};

}  // namespace fc
