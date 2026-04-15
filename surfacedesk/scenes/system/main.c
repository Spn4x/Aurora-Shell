// FILE: ./scenes/system/main.c
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <stdlib.h> 

typedef struct {
    GtkWidget *cpu_bar; GtkWidget *cpu_lbl;
    GtkWidget *ram_bar; GtkWidget *ram_lbl;
    GtkWidget *temp_bar; GtkWidget *temp_lbl;
    GtkWidget *bat_bar; GtkWidget *bat_lbl;
    
    GDBusProxy *stats_proxy;
} SysModule;

static void update_ui(SysModule *mod) {
    if (!mod->stats_proxy) return;

    g_autoptr(GVariant) cpu_v = g_dbus_proxy_get_cached_property(mod->stats_proxy, "CpuUsage");
    g_autoptr(GVariant) ram_v = g_dbus_proxy_get_cached_property(mod->stats_proxy, "RamUsage");
    g_autoptr(GVariant) temp_v = g_dbus_proxy_get_cached_property(mod->stats_proxy, "TempC");
    g_autoptr(GVariant) bat_v = g_dbus_proxy_get_cached_property(mod->stats_proxy, "BatteryPercent");

    if (cpu_v) {
        double cpu = g_variant_get_double(cpu_v);
        if (mod->cpu_bar) gtk_level_bar_set_value(GTK_LEVEL_BAR(mod->cpu_bar), cpu);
        if (mod->cpu_lbl) {
            char buf[32]; snprintf(buf, 32, "%.0f%%", cpu * 100);
            gtk_label_set_text(GTK_LABEL(mod->cpu_lbl), buf);
        }
    }

    if (ram_v) {
        double ram = g_variant_get_double(ram_v);
        if (mod->ram_bar) gtk_level_bar_set_value(GTK_LEVEL_BAR(mod->ram_bar), ram);
        if (mod->ram_lbl) {
            char buf[32]; snprintf(buf, 32, "%.0f%%", ram * 100);
            gtk_label_set_text(GTK_LABEL(mod->ram_lbl), buf);
        }
    }

    if (temp_v) {
        double temp = g_variant_get_double(temp_v);
        if (mod->temp_bar) gtk_level_bar_set_value(GTK_LEVEL_BAR(mod->temp_bar), temp / 100.0);
        if (mod->temp_lbl) {
            char buf[32]; snprintf(buf, 32, "%.0f°C", temp);
            gtk_label_set_text(GTK_LABEL(mod->temp_lbl), buf);
        }
    }

    if (bat_v) {
        double bat = g_variant_get_double(bat_v);
        if (mod->bat_bar) gtk_level_bar_set_value(GTK_LEVEL_BAR(mod->bat_bar), bat / 100.0);
        if (mod->bat_lbl) {
            char buf[32]; snprintf(buf, 32, "%.0f%%", bat);
            gtk_label_set_text(GTK_LABEL(mod->bat_lbl), buf);
        }
    }
}

static void on_stats_changed(GDBusProxy *proxy, GVariant *chg, const gchar *const *inv, gpointer user_data) {
    (void)proxy; (void)chg; (void)inv;
    update_ui((SysModule*)user_data);
}

static void on_name_owner_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    (void)object; (void)pspec;
    SysModule *mod = (SysModule*)user_data;
    g_autofree gchar *owner = g_dbus_proxy_get_name_owner(mod->stats_proxy);
    if (owner) update_ui(mod);
}

static void on_proxy_ready(GObject *source, GAsyncResult *res, gpointer user_data) {
    SysModule *mod = user_data;
    GError *error = NULL;
    mod->stats_proxy = g_dbus_proxy_new_for_bus_finish(res, &error);
    
    if (mod->stats_proxy) {
        g_signal_connect(mod->stats_proxy, "g-properties-changed", G_CALLBACK(on_stats_changed), mod);
        g_signal_connect(mod->stats_proxy, "notify::g-name-owner", G_CALLBACK(on_name_owner_changed), mod);
        
        g_autofree gchar *owner = g_dbus_proxy_get_name_owner(mod->stats_proxy);
        if (owner) update_ui(mod); // Initial paint
    } else {
        g_warning("System Widget failed to connect to Daemon: %s", error->message);
        g_error_free(error);
    }
}

static void on_destroy(GtkWidget *w, gpointer data) {
    (void)w;
    SysModule *mod = data;
    if (mod->stats_proxy) g_object_unref(mod->stats_proxy);
    g_free(mod);
}

static void init_system_module(SysModule *mod, GtkWidget *wrapper) {
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "com.meismeric.aurora.Stats", "/com/meismeric/aurora/Stats", "com.meismeric.aurora.Stats",
        NULL, on_proxy_ready, mod);
    
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