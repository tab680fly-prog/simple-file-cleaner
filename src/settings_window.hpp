#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

#include "settings.hpp"

namespace fc {

// Preferences dialog with five pages: Scanning, Custom Locations,
// Exclusions, Appearance and History. Settings are saved to disk when the
// dialog closes.
class SettingsWindow {
   public:
    // Page names accepted by present(): "scanning", "custom", "exclusions",
    // "appearance", "history".
    SettingsWindow(GtkWidget *parent, Settings &settings, std::function<void()> on_close);

    void present(const char *page_name = nullptr);

   private:
    GtkWidget *parent_;
    GtkWidget *dialog_;
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

    void toast(const std::string &text);
    void add_excl_path(const std::string &path);
    void remove_excl_path(int index);
    void add_custom_path(const std::string &path);
    void remove_custom_path(int index);
    void browse_for_folder(std::function<void(const std::string &)> on_chosen);

    static void on_closed(AdwDialog *dialog, gpointer user_data);
};

}  // namespace fc
