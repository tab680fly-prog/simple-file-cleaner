#include <adwaita.h>
#include <gtk/gtk.h>

#include "main_window.hpp"
#include "platform.hpp"
#include "settings.hpp"

namespace {

const char *kCss = R"CSS(
.scan-path-label { font-family: monospace; font-size: 0.85em; }

.found-pill {
    border-radius: 999px;
    padding: 4px 14px;
    font-size: 0.9em;
    font-weight: bold;
    color: @accent_color;
    background-color: alpha(@accent_bg_color, 0.15);
}

.bottom-bar-box { padding: 10px 18px 8px 18px; }

.action-btn { min-width: 140px; }

.size-label-large { color: @error_color; font-weight: bold; }

.summary-card { padding: 18px 20px; }

.summary-size {
    font-size: 2.2em;
    font-weight: 800;
    font-feature-settings: "tnum";
}

.start-row-icon { color: @accent_color; }
)CSS";

void apply_color_scheme() {
    AdwStyleManager *mgr = adw_style_manager_get_default();
    if (!fc::IS_WINDOWS && fc::running_as_admin())
        adw_style_manager_set_color_scheme(mgr, ADW_COLOR_SCHEME_FORCE_DARK);
    else
        adw_style_manager_set_color_scheme(mgr, ADW_COLOR_SCHEME_DEFAULT);
}

void on_activate(GApplication *app, gpointer) {
    // Launching the app again while it is open should raise the existing
    // window, not open a second one (and register the stylesheet twice).
    if (GtkWindow *existing = gtk_application_get_active_window(GTK_APPLICATION(app))) {
        gtk_window_present(existing);
        return;
    }

    apply_color_scheme();

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, kCss);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    gtk_window_set_default_icon_name("io.github.filecleaner");

    fc::Settings settings = fc::Settings::load();
    // Owns itself: freed when its GTK window is finalized.
    auto *window = new fc::FileCleanerWindow(ADW_APPLICATION(app), std::move(settings));
    gtk_window_present(GTK_WINDOW(window->widget()));
    window->maybe_show_first_run_warning();
}

}  // namespace

int main(int argc, char **argv) {
    AdwApplication *app =
        adw_application_new("io.github.filecleaner", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
