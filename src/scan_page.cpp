#include "scan_page.hpp"

#include <cstdlib>

#include "common.hpp"

namespace fc {

ScanPage::ScanPage(const std::string &animation_style) {
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(root_, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(root_, TRUE);

    GtkWidget *center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_halign(center, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(center, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(center, TRUE);
    gtk_widget_set_margin_top(center, 48);
    gtk_widget_set_margin_bottom(center, 32);
    gtk_box_append(GTK_BOX(root_), center);

    GtkWidget *ring_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(ring_box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(center), ring_box);

    ring_ = std::make_unique<ScanRing>(animation_style);
    gtk_box_append(GTK_BOX(ring_box), ring_->widget());

    title_ = gtk_label_new("Scanning…");
    gtk_widget_add_css_class(title_, "title-3");
    gtk_box_append(GTK_BOX(center), title_);

    path_label_ = gtk_label_new("");
    gtk_widget_add_css_class(path_label_, "scan-path-label");
    gtk_widget_add_css_class(path_label_, "dim-label");
    gtk_label_set_max_width_chars(GTK_LABEL(path_label_), 52);
    gtk_label_set_ellipsize(GTK_LABEL(path_label_), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_halign(path_label_, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(center), path_label_);

    found_label_ = gtk_label_new("");
    gtk_widget_add_css_class(found_label_, "found-pill");
    gtk_widget_add_css_class(found_label_, "accent");
    gtk_widget_set_halign(found_label_, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(found_label_, FALSE);
    gtk_box_append(GTK_BOX(center), found_label_);
}

void ScanPage::refresh_style(const std::string &animation_style) { ring_->set_style(animation_style); }

void ScanPage::start(const std::string &label) {
    ring_->start();
    gtk_label_set_label(GTK_LABEL(title_), label.c_str());
    gtk_label_set_label(GTK_LABEL(path_label_), "");
    gtk_widget_set_visible(found_label_, FALSE);
}

void ScanPage::stop() { ring_->stop(true); }

void ScanPage::set_path(const std::string &path) {
    gtk_label_set_label(GTK_LABEL(path_label_), display_path(path).c_str());
}

void ScanPage::set_found(int n, std::uint64_t total) {
    if (n == 0) {
        gtk_widget_set_visible(found_label_, FALSE);
    } else {
        std::string text = "Found " + std::to_string(n) + " categor" + (n == 1 ? "y" : "ies") + " · " +
                            fmt_size(total);
        gtk_label_set_label(GTK_LABEL(found_label_), text.c_str());
        gtk_widget_set_visible(found_label_, TRUE);
    }
}

void ScanPage::set_done(int n, std::uint64_t total) {
    stop();
    if (n == 0) {
        gtk_label_set_label(GTK_LABEL(title_), "Nothing found");
        gtk_label_set_label(GTK_LABEL(path_label_), "Your system configuration is verified clean");
    } else {
        std::string title = "Scan complete";
        gtk_label_set_label(GTK_LABEL(title_), title.c_str());
        std::string sub = std::to_string(n) + " categor" + (n == 1 ? "y" : "ies") + " · " + fmt_size(total) +
                          " queue target size";
        gtk_label_set_label(GTK_LABEL(path_label_), sub.c_str());
    }
}

void ScanPage::set_title(const std::string &text) { gtk_label_set_label(GTK_LABEL(title_), text.c_str()); }
void ScanPage::set_path_label(const std::string &text) {
    gtk_label_set_label(GTK_LABEL(path_label_), text.c_str());
}

}  // namespace fc
