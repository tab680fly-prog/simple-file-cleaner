#pragma once

#include <gtk/gtk.h>

#include <string>

namespace fc {

// Cairo-drawn scan indicator: an animated magnifying glass or spinner that
// morphs into a checkmark when a scan completes.
class ScanRing {
   public:
    static constexpr int SIZE = 160;

    explicit ScanRing(std::string style = "magnifier");
    ~ScanRing();

    ScanRing(const ScanRing &) = delete;
    ScanRing &operator=(const ScanRing &) = delete;

    GtkWidget *widget() const { return widget_; }

    void set_style(const std::string &style) { style_ = style; }
    void start();
    void stop(bool success = true);

   private:
    GtkWidget *widget_;
    double angle_ = 0.0;
    double done_progress_ = 0.0;
    guint timer_id_ = 0;
    guint done_timer_ = 0;
    bool running_ = false;
    bool done_ok_ = false;
    std::string style_;
    double r_ = 0.208, g_ = 0.518, b_ = 0.894;

    void cancel_timers();
    void refresh_accent();

    void draw(cairo_t *cr, int w, int h);
    void draw_magnifier_style(cairo_t *cr, int w, int h);
    void draw_magnifier(cairo_t *cr, double mx, double my);
    void draw_spinner(cairo_t *cr, int w, int h);
    void draw_checkmark_vector(cairo_t *cr, double cx, double cy, double progress, bool is_inverted);

    static void draw_func(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer user_data);
    static gboolean tick_cb(gpointer user_data);
    static gboolean done_tick_cb(gpointer user_data);
    static void on_destroy(GtkWidget *widget, gpointer user_data);
};

}  // namespace fc
