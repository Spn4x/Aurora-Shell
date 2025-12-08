#include <gtk/gtk.h>
#include <gio/gio.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <math.h>
#include "sysinfo.h"

typedef struct {
    GtkWidget *main_button;
    GtkWidget *drawing_area;
    GtkWidget *content_box;
    
    GtkWidget *cpu_label, *ram_label, *temp_label, *bat_label;

    int cpu_val;
    int ram_val;
    int temp_val;
    int bat_val;

    double cpu_pct;
    double ram_pct;
    double temp_pct;
    double bat_pct;

    gulong last_total, last_idle;
    guint poll_timer_id;
    
    gboolean is_hovered;

    double visual_alpha;     
    guint visual_timer_id;
    guint delay_timer_id;

    GDBusProxy *battery_proxy;
    guint upower_watcher_id;
    gchar *temp_file_path;
} SysInfoModule;

// --- Helper: Rounded Rectangle ---
static void cairo_rounded_rectangle(cairo_t *cr, double x, double y, double width, double height, double radius) {
    if (width <= 0 || height <= 0) return;
    if (radius > height / 2.0) radius = height / 2.0;
    if (radius > width / 2.0) radius = width / 2.0;

    cairo_new_sub_path(cr);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, 1.5 * G_PI);
    cairo_arc(cr, x + width - radius, y + radius, radius, 1.5 * G_PI, 2.0 * G_PI);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, 0.5 * G_PI);
    cairo_arc(cr, x + radius, y + height - radius, radius, 0.5 * G_PI, G_PI);
    cairo_close_path(cr);
}

// --- Animation Logic ---
static gboolean visual_fade_tick(gpointer user_data) {
    SysInfoModule *mod = user_data;
    mod->visual_alpha += 0.03; 
    if (mod->visual_alpha >= 1.0) {
        mod->visual_alpha = 1.0;
        mod->visual_timer_id = 0;
        gtk_widget_queue_draw(mod->drawing_area);
        return G_SOURCE_REMOVE;
    }
    gtk_widget_queue_draw(mod->drawing_area);
    return G_SOURCE_CONTINUE;
}

static gboolean start_fade_animation(gpointer user_data) {
    SysInfoModule *mod = user_data;
    mod->delay_timer_id = 0;
    if (mod->visual_timer_id > 0) g_source_remove(mod->visual_timer_id);
    mod->visual_timer_id = g_timeout_add(16, visual_fade_tick, mod);
    return G_SOURCE_REMOVE;
}

static void on_style_updated(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    SysInfoModule *mod = user_data;
    if (mod->visual_timer_id > 0) { g_source_remove(mod->visual_timer_id); mod->visual_timer_id = 0; }
    if (mod->delay_timer_id > 0) { g_source_remove(mod->delay_timer_id); mod->delay_timer_id = 0; }
    mod->visual_alpha = 0.0;
    gtk_widget_queue_draw(mod->drawing_area);
    mod->delay_timer_id = g_timeout_add(150, start_fade_animation, mod);
}

// --- Helper: Draw Glyph at Bar Tip ---
static void draw_glyph_at_tip(cairo_t *cr, PangoLayout *layout, int total_width, int total_height, double pct, const char* glyph, GdkRGBA color) {
    if (pct <= 0.01) return; // Hide if basically empty

    pango_layout_set_text(layout, glyph, -1);
    
    int text_w, text_h;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);

    // Calculate X position: The end of the bar
    double x = (total_width * pct);
    
    // Clamp so it doesn't fall off the right edge, but sits right on the tip
    // If bar is full, x = width. We want it slightly inside: width - text_w - padding
    // If bar is small, we want it just to the right? Or always inside?
    // Let's try centered on the tip line.
    x = x - (text_w / 2.0);

    // Clamp bounds
    if (x < 2) x = 2;
    if (x > total_width - text_w - 2) x = total_width - text_w - 2;

    double y = (total_height - text_h) / 2.0;

    // Draw with slight shadow for contrast against the bar
    cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
    cairo_move_to(cr, x + 1, y + 1);
    pango_cairo_show_layout(cr, layout);

    cairo_set_source_rgba(cr, color.red, color.green, color.blue, 1.0); // Full opacity glyph
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
}

// --- Drawing ---
static void draw_sysinfo_gauge(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    SysInfoModule *mod = (SysInfoModule *)user_data;
    if (mod->visual_alpha <= 0.01) return;

    GtkStyleContext *ctx = gtk_widget_get_style_context(GTK_WIDGET(area));
    GdkRGBA bg_col, accent_col, fg_col;
    
    if (!gtk_style_context_lookup_color(ctx, "theme_unfocused_color", &bg_col)) gdk_rgba_parse(&bg_col, "#3E3E41");
    if (!gtk_style_context_lookup_color(ctx, "custom-accent", &accent_col)) {
        if (!gtk_style_context_lookup_color(ctx, "theme_selected_bg_color", &accent_col)) gdk_rgba_parse(&accent_col, "#8aadf4");
    }
    if (!gtk_style_context_lookup_color(ctx, "theme_fg_color", &fg_col)) gdk_rgba_parse(&fg_col, "#ffffff");

    cairo_push_group(cr);

        // Background
        gdk_cairo_set_source_rgba(cr, &bg_col);
        cairo_rounded_rectangle(cr, 0, 0, width, height, 8.0);
        cairo_fill(cr);

        // --- Bars ---
        
        // 1. CPU (Back, Full Height)
        if (mod->cpu_pct > 0) {
            double w = width * mod->cpu_pct;
            cairo_set_source_rgba(cr, accent_col.red, accent_col.green, accent_col.blue, 0.2); 
            cairo_rounded_rectangle(cr, 0, 0, w, height, 8.0);
            cairo_fill(cr);
        }

        // 2. RAM (Mid-Back, 75% Height)
        if (mod->ram_pct > 0) {
            double h = height * 0.75;
            double y = (height - h) / 2.0;
            double w = width * mod->ram_pct;
            cairo_set_source_rgba(cr, accent_col.red, accent_col.green, accent_col.blue, 0.4);
            cairo_rounded_rectangle(cr, 0, y, w, h, 6.0);
            cairo_fill(cr);
        }

        // 3. Temp (Mid-Front, 50% Height)
        if (mod->temp_pct > 0) {
            double h = height * 0.50;
            double y = (height - h) / 2.0;
            double w = width * mod->temp_pct;
            cairo_set_source_rgba(cr, accent_col.red, accent_col.green, accent_col.blue, 0.6);
            cairo_rounded_rectangle(cr, 0, y, w, h, 4.0);
            cairo_fill(cr);
        }

        // 4. Battery (Front, 25% Height)
        if (mod->bat_pct > 0) {
            double h = height * 0.25;
            double y = (height - h) / 2.0;
            double w = width * mod->bat_pct;
            cairo_set_source_rgba(cr, accent_col.red, accent_col.green, accent_col.blue, 0.9);
            cairo_rounded_rectangle(cr, 0, y, w, h, 2.0);
            cairo_fill(cr);
        }

        // --- Glyphs (Only if NOT hovered) ---
        if (!mod->is_hovered) {
            PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(area), NULL);
            // Use standard FG color for glyphs so they pop
            
            // Draw order: Front to back (or back to front? Overlapping glyphs might be messy)
            // Let's draw them front to back to ensure the most opaque bars' glyphs are on top if they collide?
            // Actually, Battery (front) glyph should probably be drawn LAST so it's on top.
            
            draw_glyph_at_tip(cr, layout, width, height, mod->cpu_pct, "󰍛", fg_col);
            draw_glyph_at_tip(cr, layout, width, height, mod->ram_pct, "󰾆", fg_col);
            draw_glyph_at_tip(cr, layout, width, height, mod->temp_pct, "󰔏", fg_col);
            draw_glyph_at_tip(cr, layout, width, height, mod->bat_pct, "󰁹", fg_col);

            g_object_unref(layout);
        }

    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, mod->visual_alpha);
}

// --- Text Updating Logic ---

static void refresh_labels(SysInfoModule *mod) {
    if (mod->is_hovered) {
        // Show Content Box, Hide Glyphs (handled in draw)
        gtk_widget_set_visible(mod->content_box, TRUE);
        
        g_autofree gchar *txt_cpu = g_strdup_printf("󰍛 %d%%", mod->cpu_val);
        g_autofree gchar *txt_ram = g_strdup_printf("󰾆 %d%%", mod->ram_val);
        g_autofree gchar *txt_temp = g_strdup_printf("󰔏 %d°C", mod->temp_val);
        g_autofree gchar *txt_bat = g_strdup_printf("󰁹 %d%%", mod->bat_val);

        gtk_label_set_text(GTK_LABEL(mod->cpu_label), txt_cpu);
        gtk_label_set_text(GTK_LABEL(mod->ram_label), txt_ram);
        gtk_label_set_text(GTK_LABEL(mod->temp_label), txt_temp);
        gtk_label_set_text(GTK_LABEL(mod->bat_label), txt_bat);
    } else {
        // Hide Content Box, Show Glyphs (handled in draw)
        gtk_widget_set_visible(mod->content_box, FALSE);
    }
    gtk_widget_queue_draw(mod->drawing_area);
}

// --- Data Fetching ---

static void update_battery_widget(SysInfoModule *module, gdouble percentage) {
    module->bat_val = (int)percentage;
    module->bat_pct = percentage / 100.0;
    refresh_labels(module); // Triggers redraw
}

static void on_battery_changed(GDBusProxy *proxy, GVariant *chg, const gchar *const *inv, gpointer user_data) {
    (void)chg; (void)inv;
    g_autoptr(GVariant) p = g_dbus_proxy_get_cached_property(proxy, "Percentage");
    if(p) update_battery_widget((SysInfoModule*)user_data, g_variant_get_double(p));
}

static void on_battery_ready(GObject *s, GAsyncResult *r, gpointer d) {
    SysInfoModule *mod = d; g_autoptr(GError) e = NULL;
    mod->battery_proxy = g_dbus_proxy_new_for_bus_finish(r, &e);
    if(mod->battery_proxy) {
        g_signal_connect(mod->battery_proxy, "g-properties-changed", G_CALLBACK(on_battery_changed), mod);
        on_battery_changed(mod->battery_proxy,NULL,NULL,mod);
    }
}

static void on_upower_appeared(GDBusConnection *c, const gchar *n, const gchar *o, gpointer d) {
    (void)c; (void)n; (void)o;
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, 
        "org.freedesktop.UPower", "/org/freedesktop/UPower/devices/DisplayDevice", 
        "org.freedesktop.UPower.Device", NULL, (GAsyncReadyCallback)on_battery_ready, d);
}

static void update_cpu_usage(SysInfoModule *module) {
    FILE* fp = fopen("/proc/stat", "r");
    if (!fp) return;
    gulong user, nice, system, idle, iowait, irq, softirq, steal;
    if (fscanf(fp, "cpu %lu %lu %lu %lu %lu %lu %lu %lu", &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) < 8) { fclose(fp); return; }
    fclose(fp);
    gulong current_idle = idle + iowait;
    gulong current_total = user + nice + system + current_idle + irq + softirq + steal;
    
    if (module->last_total > 0) {
        gulong total_diff = current_total - module->last_total;
        gulong idle_diff = current_idle - module->last_idle;
        double usage = 100.0 * (total_diff - idle_diff) / total_diff;
        
        module->cpu_val = (int)usage;
        module->cpu_pct = usage / 100.0;
        refresh_labels(module);
    }
    module->last_total = current_total;
    module->last_idle = current_idle;
}

static void update_ram_usage(SysInfoModule *module) {
    FILE* fp = fopen("/proc/meminfo", "r");
    if (!fp) return;
    long mem_total = 0, mem_available = 0;
    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemTotal: %ld kB", &mem_total) == 1) {}
        if (sscanf(line, "MemAvailable: %ld kB", &mem_available) == 1) {}
        if (mem_total > 0 && mem_available > 0) break;
    }
    fclose(fp);
    if (mem_total > 0) {
        double usage = 100.0 * (mem_total - mem_available) / mem_total;
        module->ram_val = (int)usage;
        module->ram_pct = usage / 100.0;
        refresh_labels(module);
    }
}

static void find_and_update_temp(SysInfoModule *module) {
    long temp_mc = 0;
    
    if (module->temp_file_path) {
        FILE* fp = fopen(module->temp_file_path, "r");
        if (fp) { fscanf(fp, "%ld", &temp_mc); fclose(fp); }
    } else {
        const char *base = "/sys/class/hwmon";
        DIR *d = opendir(base);
        if (d) {
            struct dirent *entry;
            while ((entry = readdir(d))) {
                if (g_str_has_prefix(entry->d_name, "hwmon")) {
                    g_autofree gchar *p = g_build_filename(base, entry->d_name, "temp1_input", NULL);
                    if (g_file_test(p, G_FILE_TEST_EXISTS)) {
                        module->temp_file_path = g_strdup(p);
                        break; 
                    }
                }
            }
            closedir(d);
        }
    }

    double temp_c = 0.0;
    if (temp_mc > 5000) temp_c = temp_mc / 1000.0;
    else if (temp_mc > 150) temp_c = temp_mc / 10.0;
    else temp_c = (double)temp_mc;

    module->temp_val = (int)temp_c;
    module->temp_pct = temp_c / 100.0;
    if (module->temp_pct > 1.0) module->temp_pct = 1.0;

    refresh_labels(module);
}

static gboolean on_poll_timeout(gpointer user_data) {
    SysInfoModule *module = user_data;
    update_cpu_usage(module);
    update_ram_usage(module);
    find_and_update_temp(module);
    return G_SOURCE_CONTINUE;
}

// --- Hover Handlers ---

static void on_hover_enter(GtkEventControllerMotion *c, double x, double y, gpointer user_data) {
    (void)c; (void)x; (void)y;
    SysInfoModule *mod = user_data;
    if (!mod->is_hovered) {
        mod->is_hovered = TRUE;
        refresh_labels(mod);
    }
}

static void on_hover_leave(GtkEventControllerMotion *c, gpointer user_data) {
    (void)c;
    SysInfoModule *mod = user_data;
    if (mod->is_hovered) {
        mod->is_hovered = FALSE;
        refresh_labels(mod);
    }
}

static void sysinfo_module_cleanup(gpointer data) {
    SysInfoModule *module = data;
    if (module->poll_timer_id > 0) g_source_remove(module->poll_timer_id);
    if (module->visual_timer_id > 0) g_source_remove(module->visual_timer_id);
    if (module->delay_timer_id > 0) g_source_remove(module->delay_timer_id);
    if (module->upower_watcher_id > 0) g_bus_unwatch_name(module->upower_watcher_id);
    g_clear_object(&module->battery_proxy);
    g_free(module->temp_file_path);
    g_free(module);
}

static GtkWidget* create_text_label() {
    GtkWidget *l = gtk_label_new("...");
    gtk_widget_add_css_class(l, "sys-text-overlay");
    return l;
}

GtkWidget* create_sysinfo_module() {
    SysInfoModule *module = g_new0(SysInfoModule, 1);
    module->visual_alpha = 1.0; 
    
    module->main_button = gtk_button_new();
    gtk_widget_add_css_class(module->main_button, "sysinfo-module");
    gtk_widget_add_css_class(module->main_button, "module");
    gtk_widget_add_css_class(module->main_button, "flat");

    GtkWidget *overlay = gtk_overlay_new();
    gtk_button_set_child(GTK_BUTTON(module->main_button), overlay);

    module->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(module->drawing_area, 220, 28); 
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(module->drawing_area), draw_sysinfo_gauge, module, NULL);
    g_signal_connect(module->drawing_area, "style-updated", G_CALLBACK(on_style_updated), module);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), module->drawing_area);

    module->content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 15);
    gtk_widget_set_halign(module->content_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(module->content_box, GTK_ALIGN_CENTER);
    
    module->cpu_label = create_text_label();
    module->ram_label = create_text_label();
    module->temp_label = create_text_label();
    module->bat_label = create_text_label();

    gtk_box_append(GTK_BOX(module->content_box), module->cpu_label);
    gtk_box_append(GTK_BOX(module->content_box), module->ram_label);
    gtk_box_append(GTK_BOX(module->content_box), module->temp_label);
    gtk_box_append(GTK_BOX(module->content_box), module->bat_label);

    // Default: Hidden (only shows on hover)
    gtk_widget_set_visible(module->content_box, FALSE);

    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), module->content_box);

    GtkEventController *hover = gtk_event_controller_motion_new();
    g_signal_connect(hover, "enter", G_CALLBACK(on_hover_enter), module);
    g_signal_connect(hover, "leave", G_CALLBACK(on_hover_leave), module);
    gtk_widget_add_controller(module->main_button, hover);

    module->upower_watcher_id = g_bus_watch_name(G_BUS_TYPE_SYSTEM, "org.freedesktop.UPower", G_BUS_NAME_WATCHER_FLAGS_NONE, on_upower_appeared, NULL, module, NULL);
    on_poll_timeout(module);
    module->poll_timer_id = g_timeout_add_seconds(2, on_poll_timeout, module);

    g_object_set_data_full(G_OBJECT(module->main_button), "module-state", module, sysinfo_module_cleanup);
    return module->main_button;
}