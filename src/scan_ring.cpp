#include "scan_ring.hpp"

#include <cmath>

namespace fc {

ScanRing::ScanRing(std::string style) : style_(std::move(style)) {
    widget_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(widget_, SIZE, SIZE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(widget_), &ScanRing::draw_func, this, nullptr);
    g_signal_connect(widget_, "destroy", G_CALLBACK(&ScanRing::on_destroy), this);
}

ScanRing::~ScanRing() { cancel_timers(); }

void ScanRing::cancel_timers() {
    if (timer_id_) {
        g_source_remove(timer_id_);
        timer_id_ = 0;
    }
    if (done_timer_) {
        g_source_remove(done_timer_);
        done_timer_ = 0;
    }
}

void ScanRing::on_destroy(GtkWidget *, gpointer user_data) {
    auto *self = static_cast<ScanRing *>(user_data);
    self->cancel_timers();
}

void ScanRing::start() {
    cancel_timers();
    running_ = true;
    done_ok_ = false;
    done_progress_ = 0.0;
    timer_id_ = g_timeout_add(16, &ScanRing::tick_cb, this);
}

void ScanRing::stop(bool success) {
    running_ = false;
    done_ok_ = success;
    if (timer_id_) {
        g_source_remove(timer_id_);
        timer_id_ = 0;
    }
    if (success) {
        done_timer_ = g_timeout_add(16, &ScanRing::done_tick_cb, this);
    } else {
        gtk_widget_queue_draw(widget_);
    }
}

gboolean ScanRing::tick_cb(gpointer user_data) {
    auto *self = static_cast<ScanRing *>(user_data);
    if (!self->running_) {
        self->timer_id_ = 0;
        return G_SOURCE_REMOVE;
    }
    self->angle_ = std::fmod(self->angle_ + 3.0, 360.0);
    self->refresh_accent();
    gtk_widget_queue_draw(self->widget_);
    return G_SOURCE_CONTINUE;
}

gboolean ScanRing::done_tick_cb(gpointer user_data) {
    auto *self = static_cast<ScanRing *>(user_data);
    self->done_progress_ = std::min(1.0, self->done_progress_ + 0.05);
    gtk_widget_queue_draw(self->widget_);
    if (self->done_progress_ >= 1.0) {
        self->done_timer_ = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
void ScanRing::refresh_accent() {
    GdkRGBA c;
    if (gtk_style_context_lookup_color(gtk_widget_get_style_context(widget_), "accent_color", &c)) {
        r_ = c.red;
        g_ = c.green;
        b_ = c.blue;
    }
}
G_GNUC_END_IGNORE_DEPRECATIONS

void ScanRing::draw_func(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer user_data) {
    static_cast<ScanRing *>(user_data)->draw(cr, w, h);
}

void ScanRing::draw(cairo_t *cr, int w, int h) {
    if (style_ == "spinner")
        draw_spinner(cr, w, h);
    else
        draw_magnifier_style(cr, w, h);
}

void ScanRing::draw_magnifier_style(cairo_t *cr, int w, int h) {
    double cx = w / 2.0, cy = h / 2.0;
    double r = r_, g = g_, b = b_;
    const double circle_r = 58, inner_orb = 24;

    if (done_ok_) {
        double p = done_progress_;
        double fr = r + (0.20 - r) * p;
        double fg = g + (0.78 - g) * p;
        double fb = b + (0.35 - b) * p;
        cairo_set_source_rgba(cr, fr, fg, fb, 1.0);
    } else {
        cairo_set_source_rgba(cr, r, g, b, 1.0);
    }
    cairo_arc(cr, cx, cy, circle_r, 0, 2 * M_PI);
    cairo_fill(cr);

    if (running_) {
        double rad = (angle_ - 90) * M_PI / 180.0;
        double mx = cx + inner_orb * std::cos(rad);
        double my = cy + inner_orb * std::sin(rad);
        draw_magnifier(cr, mx, my);
    } else if (done_ok_) {
        draw_checkmark_vector(cr, cx, cy, done_progress_, true);
    }
}

void ScanRing::draw_magnifier(cairo_t *cr, double mx, double my) {
    const double lens_r = 17.0, handle_l = 14.0, lw = 3.0;
    cairo_save(cr);
    cairo_translate(cr, mx, my);

    cairo_arc(cr, 0, 0, lens_r, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.20);
    cairo_fill(cr);

    cairo_arc(cr, 0, 0, lens_r, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    cairo_set_line_width(cr, lw);
    cairo_stroke(cr);

    cairo_arc(cr, 0, 0, lens_r - 5, 210 * M_PI / 180.0, 285 * M_PI / 180.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.50);
    cairo_set_line_width(cr, lw - 1.0);
    cairo_stroke(cr);

    double hx = lens_r * std::cos(45 * M_PI / 180.0);
    double hy = lens_r * std::sin(45 * M_PI / 180.0);
    cairo_move_to(cr, hx, hy);
    cairo_line_to(cr, hx + handle_l, hy + handle_l);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    cairo_set_line_width(cr, lw + 1.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void ScanRing::draw_spinner(cairo_t *cr, int w, int h) {
    double cx = w / 2.0, cy = h / 2.0;
    double r = r_, g = g_, b = b_;
    const double track_r = 52;
    const double lw = 6;

    if (running_) {
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_width(cr, lw);

        double a0 = (angle_ - 90) * M_PI / 180.0;
        double a1 = a0 + (M_PI / 2);

        cairo_arc(cr, cx, cy, track_r, a0, a1);
        cairo_set_source_rgba(cr, r, g, b, 1.0);
        cairo_stroke(cr);
    } else if (done_ok_) {
        double prog = done_progress_;

        cairo_set_line_width(cr, lw);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_arc(cr, cx, cy, track_r, -M_PI / 2, -M_PI / 2 + 2 * M_PI * prog);
        cairo_set_source_rgba(cr, 0.20, 0.78, 0.35, 1.0);
        cairo_stroke(cr);

        if (prog >= 1.0) draw_checkmark_vector(cr, cx, cy, 1.0, false);
    } else {
        cairo_set_source_rgba(cr, r, g, b, 0.12);
        cairo_set_line_width(cr, lw);
        cairo_arc(cr, cx, cy, track_r, 0, 2 * M_PI);
        cairo_stroke(cr);
    }
}

void ScanRing::draw_checkmark_vector(cairo_t *cr, double cx, double cy, double progress, bool is_inverted) {
    if (progress < 0.01) return;

    double tick_p = is_inverted ? std::max(0.0, (progress - 0.2) / 0.8) : progress;
    if (tick_p <= 0) return;

    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_set_line_width(cr, 5.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    if (is_inverted)
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    else
        cairo_set_source_rgba(cr, 0.20, 0.78, 0.35, 1.0);

    const double p1x = -14.0, p1y = -1.0;
    const double p2x = -4.0, p2y = 9.0;
    const double p3x = 12.0, p3y = -9.0;

    double seg1_p = std::min(1.0, tick_p * 2.5);
    double seg2_p = std::max(0.0, (tick_p - 0.4) * 1.66);

    cairo_new_path(cr);

    if (seg1_p > 0) {
        cairo_move_to(cr, p1x, p1y);
        cairo_line_to(cr, p1x + (p2x - p1x) * seg1_p, p1y + (p2y - p1y) * seg1_p);
    }
    if (seg2_p > 0) {
        cairo_move_to(cr, p2x, p2y);
        cairo_line_to(cr, p2x + (p3x - p2x) * seg2_p, p2y + (p3y - p2y) * seg2_p);
    }

    cairo_stroke(cr);
    cairo_restore(cr);
}

}  // namespace fc
