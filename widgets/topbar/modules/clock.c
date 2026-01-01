#include <gtk/gtk.h>
#include <time.h>
#include <math.h>
#include "clock.h"

typedef struct {
    char time_str[64];
    char date_str[64];
    double time_progress;  // 0.0 - 1.0 (Time of day)
    double month_progress; // 0.0 - 1.0 (Day of month)
    guint timer_id;
    GtkWidget *drawing_area;
} ClockModule;

static void draw_clock(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);

static gboolean update_clock_data(gpointer user_data) {
    ClockModule *module = (ClockModule *)user_data;
    time_t raw_time;
    struct tm *time_info;

    time(&raw_time);
    time_info = localtime(&raw_time);

    // 1. Update Strings
    strftime(module->time_str, sizeof(module->time_str), "%I:%M %p", time_info);
    strftime(module->date_str, sizeof(module->date_str), "%b %d", time_info);

    // 2. Calculate Time Progress (Left Half)
    double current_minutes = (time_info->tm_hour * 60.0) + time_info->tm_min;
    module->time_progress = current_minutes / 1440.0;

    // 3. Calculate Month Progress (Right Half)
    GDate date;
    g_date_set_time_t(&date, raw_time);
    int days_in_month = g_date_get_days_in_month(g_date_get_month(&date), g_date_get_year(&date));
    int current_day = time_info->tm_mday;
    module->month_progress = (double)current_day / (double)days_in_month;

    if (GTK_IS_WIDGET(module->drawing_area)) {
        gtk_widget_queue_draw(module->drawing_area);
    }
    return G_SOURCE_CONTINUE;
}

static void clock_module_cleanup(gpointer data) {
    ClockModule *module = (ClockModule *)data;
    if (module->timer_id > 0) {
        g_source_remove(module->timer_id);
    }
    g_free(module);
}

static void cairo_rounded_rectangle(cairo_t *cr, double x, double y, double width, double height, double radius) {
    if (width <= 0 || height <= 0) return;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, 1.5 * G_PI);
    cairo_arc(cr, x + width - radius, y + radius, radius, 1.5 * G_PI, 2.0 * G_PI);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, 0.5 * G_PI);
    cairo_arc(cr, x + radius, y + height - radius, radius, 0.5 * G_PI, G_PI);
    cairo_close_path(cr);
}

static void draw_clock(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    ClockModule *module = (ClockModule *)user_data;
    GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(area));
    
    GdkRGBA bg_color, fg_color, accent_color;

    // --- 1. Colors Setup ---
    
    // Default fallback for background (Dark Grey/Blueish)
    gdk_rgba_parse(&bg_color, "#3E3E41");
    // Default fallback for foreground (White)
    gdk_rgba_parse(&fg_color, "#ffffff");
    // Default fallback for accent (Red/Pinkish)
    gdk_rgba_parse(&accent_color, "#e78284");

    // Look up Background Color (Priority: theme_unfocused_color -> theme_unfocused_bg_color -> window_bg_color)
    if (!gtk_style_context_lookup_color(context, "theme_unfocused_color", &bg_color)) {
        if (!gtk_style_context_lookup_color(context, "theme_unfocused_bg_color", &bg_color)) {
            gtk_style_context_lookup_color(context, "window_bg_color", &bg_color);
        }
    }

    // Look up Foreground Color
    gtk_style_context_lookup_color(context, "theme_fg_color", &fg_color);
    
    // Look up Accent Color
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    if (!gtk_style_context_lookup_color(context, "custom-accent", &accent_color)) {
        gtk_style_context_lookup_color(context, "theme_selected_bg_color", &accent_color);
    }
    #pragma GCC diagnostic pop

    // --- 2. Draw Background ---
    gdk_cairo_set_source_rgba(cr, &bg_color);
    cairo_rounded_rectangle(cr, 0, 0, width, height, 8.0);
    cairo_fill(cr);

    // --- 3. Draw Progress Bars ---
    cairo_save(cr);
    cairo_rounded_rectangle(cr, 0, 0, width, height, 8.0);
    cairo_clip(cr);

    double half_width = width / 2.0;

    // A. Time Bar (Left Half)
    // Starts at Left Edge (0), fills towards Center
    double time_bar_width = half_width * module->time_progress;
    if (time_bar_width > 0) {
        gdk_cairo_set_source_rgba(cr, &accent_color);
        cairo_rectangle(cr, 0, 0, time_bar_width, height);
        cairo_fill(cr);
    }

    // B. Date Bar (Right Half)
    // Starts at Right Edge (width), fills towards Center (leftwards)
    double date_bar_width = half_width * module->month_progress;
    if (date_bar_width > 0) {
        cairo_set_source_rgba(cr, accent_color.red, accent_color.green, accent_color.blue, 0.6);
        cairo_rectangle(cr, width - date_bar_width, 0, date_bar_width, height);
        cairo_fill(cr);
    }

    cairo_restore(cr);

    // --- 4. Draw Text ---
    PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(area), NULL);
    int text_w, text_h;

    // A. Draw Time Text (Centered in Left Half)
    g_autofree gchar *time_markup = g_strdup_printf("<b>%s</b>", module->time_str);
    pango_layout_set_markup(layout, time_markup, -1);
    pango_layout_get_pixel_size(layout, &text_w, &text_h);
    
    double time_x = (half_width - text_w) / 2.0;
    double text_y = (height - text_h) / 2.0;

    gdk_cairo_set_source_rgba(cr, &fg_color);
    cairo_move_to(cr, time_x, text_y);
    pango_cairo_show_layout(cr, layout);

    // B. Draw Date Text (Centered in Right Half)
    pango_layout_set_text(layout, module->date_str, -1);
    pango_layout_get_pixel_size(layout, &text_w, &text_h);

    double date_x = half_width + (half_width - text_w) / 2.0;

    cairo_move_to(cr, date_x, text_y);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
}

GtkWidget* create_clock_module() {
    ClockModule *module = g_new0(ClockModule, 1);
    
    module->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(module->drawing_area, 180, 28);
    
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(module->drawing_area), draw_clock, module, NULL);
    
    gtk_widget_add_css_class(module->drawing_area, "clock-module");
    gtk_widget_add_css_class(module->drawing_area, "module");

    g_object_set_data_full(G_OBJECT(module->drawing_area), "module-state", module, clock_module_cleanup);
    
    update_clock_data(module);
    module->timer_id = g_timeout_add_seconds(1, update_clock_data, module);
    
    return module->drawing_area;
}