// FILE: ./scenes/system/main.c
#include <gtk/gtk.h>
#include <stdint.h>
#include <stdlib.h> 
#include <gio/gio.h>

// Rust Interface
typedef struct {
    double cpu_usage;
    double ram_usage;
    double temp_c;
    uint64_t new_total;
    uint64_t new_idle;
} SystemStats;

extern void rust_get_system_stats(uint64_t prev_total, uint64_t prev_idle, SystemStats *out);

typedef struct {
    GtkWidget *cpu_bar; GtkWidget *cpu_lbl;
    GtkWidget *ram_bar; GtkWidget *ram_lbl;
    GtkWidget *temp_bar; GtkWidget *temp_lbl;
    GtkWidget *bat_bar; GtkWidget *bat_lbl;
    
    uint64_t last_total;
    uint64_t last_idle;
    guint poll_timer_id;
    GDBusProxy *battery_proxy;
} SysModule;

static gboolean on_poll(gpointer data) {
    SysModule *mod = data;
    SystemStats stats;
    
    rust_get_system_stats(mod->last_total, mod->last_idle, &stats);
    
    mod->last_total = stats.new_total;
    mod->last_idle = stats.new_idle;
    
    if (mod->cpu_bar) gtk_level_bar_set_value(GTK_LEVEL_BAR(mod->cpu_bar), stats.cpu_usage);
    if (mod->cpu_lbl) {
        char buf[32]; snprintf(buf, 32, "%.0f%%", stats.cpu_usage * 100);
        gtk_label_set_text(GTK_LABEL(mod->cpu_lbl), buf);
    }
    
    if (mod->ram_bar) gtk_level_bar_set_value(GTK_LEVEL_BAR(mod->ram_bar), stats.ram_usage);
    if (mod->ram_lbl) {
        char buf[32]; snprintf(buf, 32, "%.0f%%", stats.ram_usage * 100);
        gtk_label_set_text(GTK_LABEL(mod->ram_lbl), buf);
    }

    if (mod->temp_bar) gtk_level_bar_set_value(GTK_LEVEL_BAR(mod->temp_bar), stats.temp_c / 100.0);
    if (mod->temp_lbl) {
        char buf[32]; snprintf(buf, 32, "%.0f°C", stats.temp_c);
        gtk_label_set_text(GTK_LABEL(mod->temp_lbl), buf);
    }

    return G_SOURCE_CONTINUE;
}

static void on_bat_change(GDBusProxy *proxy, GVariant *chg, const gchar *const *inv, gpointer user_data) {
    (void)chg; (void)inv;
    SysModule *mod = user_data;
    g_autoptr(GVariant) v = g_dbus_proxy_get_cached_property(proxy, "Percentage");
    if (v) {
        double pct = g_variant_get_double(v);
        if (mod->bat_bar) gtk_level_bar_set_value(GTK_LEVEL_BAR(mod->bat_bar), pct / 100.0);
        if (mod->bat_lbl) {
            char buf[32]; snprintf(buf, 32, "%.0f%%", pct);
            gtk_label_set_text(GTK_LABEL(mod->bat_lbl), buf);
        }
    }
}

static void on_bat_ready(GObject *s, GAsyncResult *r, gpointer d) {
    SysModule *mod = d; g_autoptr(GError) e = NULL;
    mod->battery_proxy = g_dbus_proxy_new_for_bus_finish(r, &e);
    if (mod->battery_proxy) {
        g_signal_connect(mod->battery_proxy, "g-properties-changed", G_CALLBACK(on_bat_change), mod);
        on_bat_change(mod->battery_proxy, NULL, NULL, mod);
    }
}

static void on_upower_ready(GDBusConnection *c, const gchar *n, const gchar *o, gpointer d) {
    (void)c; (void)n; (void)o;
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, 
        "org.freedesktop.UPower", "/org/freedesktop/UPower/devices/DisplayDevice", 
        "org.freedesktop.UPower.Device", NULL, (GAsyncReadyCallback)on_bat_ready, d);
}

static void on_destroy(GtkWidget *w, gpointer data) {
    SysModule *mod = data;
    if (mod->poll_timer_id) g_source_remove(mod->poll_timer_id);
    if (mod->battery_proxy) g_object_unref(mod->battery_proxy);
    g_free(mod);
}

static void init_system_module(SysModule *mod, GtkWidget *wrapper) {
    g_bus_watch_name(G_BUS_TYPE_SYSTEM, "org.freedesktop.UPower", G_BUS_NAME_WATCHER_FLAGS_NONE, on_upower_ready, NULL, mod, NULL);
    on_poll(mod);
    mod->poll_timer_id = g_timeout_add_seconds(2, on_poll, mod);
    g_signal_connect(wrapper, "destroy", G_CALLBACK(on_destroy), mod);
}

static const char *ICONS[] = { "system-run-symbolic", "drive-harddisk-symbolic", "weather-severe-alert-symbolic", "battery-level-50-symbolic" };

GtkWidget* scene_sys_stacked(void) {
    SysModule *mod = g_new0(SysModule, 1);
    GtkWidget *wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(wrapper, "card");
    
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_valign(content, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(content, 15);
    gtk_widget_set_margin_end(content, 15);
    gtk_widget_set_margin_top(content, 15);
    gtk_widget_set_margin_bottom(content, 15);

    GtkWidget **bars[] = {&mod->cpu_bar, &mod->ram_bar, &mod->temp_bar, &mod->bat_bar};
    GtkWidget **lbls[] = {&mod->cpu_lbl, &mod->ram_lbl, &mod->temp_lbl, &mod->bat_lbl};

    for(int i=0; i<4; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        
        GtkWidget *icon = gtk_image_new_from_icon_name(ICONS[i]);
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
        gtk_widget_set_opacity(icon, 0.7);
        gtk_box_append(GTK_BOX(row), icon);
        
        GtkWidget *bar = gtk_level_bar_new();
        gtk_widget_set_hexpand(bar, TRUE);
        gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), bar);
        *bars[i] = bar;

        GtkWidget *val = gtk_label_new("--");
        gtk_widget_add_css_class(val, "numeric");
        gtk_widget_set_name(val, "sys-text");
        gtk_widget_set_size_request(val, 45, -1);
        gtk_label_set_xalign(GTK_LABEL(val), 1.0);
        gtk_box_append(GTK_BOX(row), val);
        *lbls[i] = val;

        gtk_box_append(GTK_BOX(content), row);
    }
    
    gtk_box_append(GTK_BOX(wrapper), content);
    init_system_module(mod, wrapper);
    return wrapper;
}

GtkWidget* scene_sys_vertical(void) {
    SysModule *mod = g_new0(SysModule, 1);
    GtkWidget *wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(wrapper, "card");

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(content, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(content, GTK_ALIGN_FILL);
    gtk_widget_set_margin_top(content, 15);
    gtk_widget_set_margin_bottom(content, 15);

    GtkWidget **bars[] = {&mod->cpu_bar, &mod->ram_bar, &mod->temp_bar, &mod->bat_bar};

    for(int i=0; i<4; i++) {
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_valign(col, GTK_ALIGN_FILL);
        
        GtkWidget *bar = gtk_level_bar_new();
        gtk_orientable_set_orientation(GTK_ORIENTABLE(bar), GTK_ORIENTATION_VERTICAL);
        gtk_level_bar_set_inverted(GTK_LEVEL_BAR(bar), TRUE);
        gtk_widget_set_vexpand(bar, TRUE);
        gtk_widget_set_size_request(bar, 8, -1);
        gtk_box_append(GTK_BOX(col), bar);
        *bars[i] = bar;

        GtkWidget *icon = gtk_image_new_from_icon_name(ICONS[i]);
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 14);
        gtk_widget_set_opacity(icon, 0.7);
        gtk_box_append(GTK_BOX(col), icon);

        gtk_box_append(GTK_BOX(content), col);
    }

    gtk_box_append(GTK_BOX(wrapper), content);
    init_system_module(mod, wrapper);
    return wrapper;
}

GtkWidget* scene_sys_dashboard(void) {
    SysModule *mod = g_new0(SysModule, 1);
    GtkWidget *wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(wrapper, "card");

    GtkWidget *grid = gtk_grid_new();
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(grid, GTK_ALIGN_CENTER);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 15);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 30);
    gtk_widget_set_margin_top(grid, 20);
    gtk_widget_set_margin_bottom(grid, 20);

    struct { const char *title; GtkWidget **lbl; GtkWidget **bar; int x; int y; } items[] = {
        { "CPU", &mod->cpu_lbl, &mod->cpu_bar, 0, 0 },
        { "RAM", &mod->ram_lbl, &mod->ram_bar, 1, 0 },
        { "TMP", &mod->temp_lbl, &mod->temp_bar, 0, 1 },
        { "BAT", &mod->bat_lbl, &mod->bat_bar, 1, 1 }
    };

    for (int i=0; i<4; i++) {
        GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        
        GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *icon = gtk_image_new_from_icon_name(ICONS[i]);
        GtkWidget *title = gtk_label_new(items[i].title);
        gtk_widget_add_css_class(title, "heading");
        gtk_box_append(GTK_BOX(top), icon);
        gtk_box_append(GTK_BOX(top), title);
        gtk_box_append(GTK_BOX(cell), top);

        GtkWidget *val = gtk_label_new("--");
        gtk_widget_add_css_class(val, "title-2");
        gtk_widget_set_name(val, "sys-text");
        gtk_widget_set_halign(val, GTK_ALIGN_START);
        *items[i].lbl = val;
        gtk_box_append(GTK_BOX(cell), val);

        GtkWidget *bar = gtk_level_bar_new();
        gtk_widget_set_size_request(bar, 100, 4);
        *items[i].bar = bar;
        gtk_box_append(GTK_BOX(cell), bar);

        gtk_grid_attach(GTK_GRID(grid), cell, items[i].x, items[i].y, 1, 1);
    }

    gtk_box_append(GTK_BOX(wrapper), grid);
    init_system_module(mod, wrapper);
    return wrapper;
}

GtkWidget* scene_sys_text(void) {
    SysModule *mod = g_new0(SysModule, 1);
    GtkWidget *wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(wrapper, "card");
    
    GtkWidget *grid = gtk_grid_new();
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(grid, GTK_ALIGN_CENTER);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 20);

    GtkWidget **lbls[] = {&mod->cpu_lbl, &mod->ram_lbl, &mod->temp_lbl, &mod->bat_lbl};

    for(int i=0; i<4; i++) {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *icon = gtk_image_new_from_icon_name(ICONS[i]);
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 18);
        
        GtkWidget *val = gtk_label_new("--");
        gtk_widget_add_css_class(val, "title-3");
        gtk_widget_set_name(val, "sys-text");
        *lbls[i] = val;

        gtk_box_append(GTK_BOX(box), icon);
        gtk_box_append(GTK_BOX(box), val);
        
        int row = i / 2;
        int col = i % 2;
        gtk_grid_attach(GTK_GRID(grid), box, col, row, 1, 1);
    }

    gtk_box_append(GTK_BOX(wrapper), grid);
    init_system_module(mod, wrapper);
    return wrapper;
}