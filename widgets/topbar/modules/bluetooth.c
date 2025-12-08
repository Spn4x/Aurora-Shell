#include <gtk/gtk.h>
#include <gio/gio.h>
#include "bluetooth.h"

typedef struct {
    GtkWidget *drawing_area;
    GDBusObjectManager *bluez_manager;
    
    // State for drawing
    gboolean is_connected;
    gboolean is_powered;
    gchar *device_name;
    int battery_percentage; // -1 if unknown
} BluetoothModule;

// --- Helper: Rounded Rectangle ---
static void cairo_rounded_rectangle(cairo_t *cr, double x, double y, double width, double height, double radius) {
    if (width <= 0 || height <= 0) return;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, 1.5 * G_PI);
    cairo_arc(cr, x + width - radius, y + radius, radius, 1.5 * G_PI, 2.0 * G_PI);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, 0.5 * G_PI);
    cairo_arc(cr, x + radius, y + height - radius, radius, 0.5 * G_PI, G_PI);
    cairo_close_path(cr);
}

// --- Draw Function ---
static void draw_bluetooth(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    BluetoothModule *module = (BluetoothModule *)user_data;
    GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(area));
    
    GdkRGBA bg_color, fg_color, accent_color;
    
    // 1. Defaults
    gdk_rgba_parse(&bg_color, "rgba(30, 30, 46, 0.5)"); 
    gdk_rgba_parse(&fg_color, "#ffffff");
    gdk_rgba_parse(&accent_color, "#e78284"); // Fallback Red
    
    // 2. Theme Lookup
    if (!gtk_style_context_lookup_color(context, "theme_unfocused_bg_color", &bg_color)) {
        gtk_style_context_lookup_color(context, "window_bg_color", &bg_color);
    }
    gtk_style_context_lookup_color(context, "theme_fg_color", &fg_color);
    
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    gboolean found_accent = FALSE;
    if (gtk_style_context_lookup_color(context, "custom-accent", &accent_color)) {
        found_accent = TRUE;
    } else if (gtk_style_context_lookup_color(context, "theme_selected_bg_color", &accent_color)) {
        found_accent = TRUE;
    }
    #pragma GCC diagnostic pop

    // Safety: If accent is fully transparent, fallback to red
    if (found_accent && accent_color.alpha < 0.1) {
        gdk_rgba_parse(&accent_color, "#e78284");
        accent_color.alpha = 1.0;
    }

    // 3. Draw Background
    gdk_cairo_set_source_rgba(cr, &bg_color);
    cairo_rounded_rectangle(cr, 0, 0, width, height, 8.0);
    cairo_fill(cr);

    // 4. Draw Battery Progress Bar
    if (module->is_connected && module->battery_percentage >= 0) {
        // Debug output to check if logic is reached
        // g_print("BT Draw: %d%%, Width: %d\n", module->battery_percentage, width);

        cairo_save(cr);
        
        // Clip to rounded shape
        cairo_rounded_rectangle(cr, 0, 0, width, height, 8.0);
        cairo_clip(cr);

        double progress = (double)module->battery_percentage / 100.0;
        double bar_width = width * progress;

        // Use Accent Color
        gdk_cairo_set_source_rgba(cr, &accent_color);
        cairo_rectangle(cr, 0, 0, bar_width, height);
        cairo_fill(cr);
        
        cairo_restore(cr);
    }

    // 5. Draw Text
    PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(area), NULL);
    gchar *text = NULL;
    
    if (!module->is_powered) {
        text = g_strdup("󰂲  Bluetooth Off");
    } else if (module->is_connected) {
        if (module->battery_percentage >= 0) {
            text = g_strdup_printf("󰋋  %d%% %s", module->battery_percentage, module->device_name);
        } else {
            text = g_strdup_printf("󰋋  %s", module->device_name);
        }
    } else {
        text = g_strdup("󰂯  Disconnected");
    }

    pango_layout_set_text(layout, text, -1);
    g_free(text);

    int text_w, text_h;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);
    
    double x = (width - text_w) / 2.0;
    double y = (height - text_h) / 2.0;

    gdk_cairo_set_source_rgba(cr, &fg_color);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

static void update_bluetooth_status(BluetoothModule *module) {
    if (!module->bluez_manager) return;

    module->is_connected = FALSE;
    module->is_powered = FALSE;
    module->battery_percentage = -1;
    g_free(module->device_name);
    module->device_name = g_strdup("Unknown");

    g_autoptr(GDBusInterface) adapter = g_dbus_object_manager_get_interface(module->bluez_manager, "/org/bluez/hci0", "org.bluez.Adapter1");
    if (adapter) {
        g_autoptr(GVariant) p_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(adapter), "Powered");
        if (p_var) module->is_powered = g_variant_get_boolean(p_var);
    }

    if (module->is_powered) {
        g_autoptr(GList) objects = g_dbus_object_manager_get_objects(module->bluez_manager);
        for (GList *l = objects; l != NULL; l = l->next) {
            GDBusObject *obj = G_DBUS_OBJECT(l->data);
            g_autoptr(GDBusInterface) device = g_dbus_object_get_interface(obj, "org.bluez.Device1");
            
            if (device) {
                g_autoptr(GVariant) c_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(device), "Connected");
                if (c_var && g_variant_get_boolean(c_var)) {
                    module->is_connected = TRUE;
                    
                    g_autoptr(GVariant) n_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(device), "Alias");
                    if (!n_var) n_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(device), "Name");
                    
                    if (n_var) {
                        g_free(module->device_name);
                        module->device_name = g_variant_dup_string(n_var, NULL);
                    }

                    // Try standard BatteryPercentage first
                    g_autoptr(GVariant) b_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(device), "BatteryPercentage");
                    if (b_var) {
                        module->battery_percentage = g_variant_get_byte(b_var);
                    } else {
                        // Fallback: Loop for Battery1 interface on same object path
                        // Sometimes BlueZ puts battery on a separate interface
                        g_autoptr(GDBusInterface) bat_iface = g_dbus_object_get_interface(obj, "org.bluez.Battery1");
                        if (bat_iface) {
                            g_autoptr(GVariant) b1_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(bat_iface), "Percentage");
                            if (b1_var) module->battery_percentage = g_variant_get_byte(b1_var);
                        }
                    }
                    break; 
                }
            }
        }
    }

    gtk_widget_queue_draw(module->drawing_area);
}

static void on_bluez_properties_changed(GDBusObjectManagerClient *m, GDBusObjectProxy *o, GDBusProxy *i, GVariant *c, const gchar *const *iv, gpointer d) { 
    (void)m; (void)o; (void)i; (void)c; (void)iv; 
    update_bluetooth_status((BluetoothModule *)d); 
}

static void on_object_manager_created(GObject *s, GAsyncResult *r, gpointer d) {
    (void)s; BluetoothModule *module = (BluetoothModule *)d;
    g_autoptr(GError) error = NULL;
    module->bluez_manager = g_dbus_object_manager_client_new_for_bus_finish(r, &error);
    if (error) { g_warning("BlueZ Error: %s", error->message); return; }
    
    update_bluetooth_status(module);
    g_signal_connect(module->bluez_manager, "interface-proxy-properties-changed", G_CALLBACK(on_bluez_properties_changed), module);
}

static void bluetooth_module_cleanup(gpointer data) {
    BluetoothModule *module = data;
    if (module->bluez_manager) g_object_unref(module->bluez_manager);
    g_free(module->device_name);
    g_free(module);
}

GtkWidget* create_bluetooth_module() {
    BluetoothModule *module = g_new0(BluetoothModule, 1);
    module->device_name = g_strdup("...");

    module->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(module->drawing_area, 200, 28);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(module->drawing_area), draw_bluetooth, module, NULL);
    
    gtk_widget_add_css_class(module->drawing_area, "audio-module"); 
    gtk_widget_add_css_class(module->drawing_area, "module");

    g_dbus_object_manager_client_new_for_bus(
        G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
        "org.bluez", "/", NULL, NULL, NULL, NULL,
        (GAsyncReadyCallback)on_object_manager_created, module
    );

    g_object_set_data_full(G_OBJECT(module->drawing_area), "module-state", module, bluetooth_module_cleanup);
    return module->drawing_area;
}