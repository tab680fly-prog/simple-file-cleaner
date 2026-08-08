#include <unistd.h>

#include <adwaita.h>
#include <gtk/gtk.h>

#include "main_window.hpp"
#include "settings.hpp"

namespace {

const char *kCss = R"CSS(
.scan-path-label { font-family: monospace; font-size: 11px; }

.found-pill {
    border-radius: 999px;
    padding: 2px 12px;
    font-size: 11px;
    font-weight: bold;
}

.bottom-bar-box {
    padding: 12px 18px;
    border-top: 1px solid rgba(127,127,127,0.13);
}

.action-btn { min-width: 130px; }

.size-label-large { color: #e5534b; }

.cat-group-header {
    font-size: 12px;
    font-weight: 600;
    opacity: 0.75;
    padding: 2px 0;
}

.summary-bar {
    padding: 10px 16px;
    border-bottom: 1px solid rgba(127,127,127,0.13);
}

.summary-total {
    font-size: 13px;
    font-weight: 600;
}

.result-group-fading {
    transition: opacity 200ms ease;
}
)CSS";

void apply_color_scheme() {
    AdwStyleManager *mgr = adw_style_manager_get_default();
    if (geteuid() == 0)
        adw_style_manager_set_color_scheme(mgr, ADW_COLOR_SCHEME_FORCE_DARK);
    else
        adw_style_manager_set_color_scheme(mgr, ADW_COLOR_SCHEME_DEFAULT);
}

void on_activate(GApplication *app, gpointer) {
    apply_color_scheme();

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, kCss);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    fc::Settings settings = fc::Settings::load();
    // Owns itself: freed when its GTK window is finalized.
    auto *window = new fc::FileCleanerWindow(ADW_APPLICATION(app), std::move(settings));
    gtk_window_present(GTK_WINDOW(window->widget()));
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
