#include <gtk/gtk.h>
#include <gio/gio.h>
#include "bluetooth.h"

typedef struct {
    GtkWidget *main_button;
    GtkWidget *content_box;
    GtkWidget *glyph_label;
    GtkWidget *text_label;
    GDBusObjectManager *bluez_manager;
} BluetoothModule;

// --- Forward Declarations ---
static void on_object_manager_created(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_bluez_properties_changed(GDBusObjectManagerClient *manager, GDBusObjectProxy *object, GDBusProxy *interface, GVariant *changed_properties, const gchar *const *invalidated_properties, gpointer user_data);
static void update_bluetooth_status(BluetoothModule *module);
static void bluetooth_module_cleanup(gpointer data);


// =======================================================================
// THE UPGRADED UPDATE FUNCTION
// =======================================================================
static void update_bluetooth_status(BluetoothModule *module) {
    const char *HEADPHONE_ICON = "";
    gboolean is_powered = FALSE;
    g_autofree gchar *connected_device_name = NULL;
    gint battery_level = -1;

    if (!module->bluez_manager) { gtk_label_set_text(GTK_LABEL(module->text_label), "..."); return; }

    g_autoptr(GDBusInterface) adapter_interface = g_dbus_object_manager_get_interface(module->bluez_manager, "/org/bluez/hci0", "org.bluez.Adapter1");
    if (adapter_interface) {
        g_autoptr(GVariant) powered_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(adapter_interface), "Powered");
        if (powered_var) is_powered = g_variant_get_boolean(powered_var);
    }

    if (!is_powered) {
        gtk_label_set_text(GTK_LABEL(module->glyph_label), HEADPHONE_ICON);
        gtk_label_set_text(GTK_LABEL(module->text_label), " Disconnected");
        return;
    }

    g_autoptr(GList) devices = g_dbus_object_manager_get_objects(module->bluez_manager);
    for (GList *l = devices; l != NULL; l = l->next) {
        GDBusObject *obj = G_DBUS_OBJECT(l->data);
        g_autoptr(GDBusInterface) device_interface = g_dbus_object_get_interface(obj, "org.bluez.Device1");
        if (device_interface) {
            g_autoptr(GVariant) connected_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(device_interface), "Connected");
            if (connected_var && g_variant_get_boolean(connected_var)) {
                g_autoptr(GVariant) name_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(device_interface), "Name");
                if (name_var) connected_device_name = g_variant_dup_string(name_var, NULL);

                // --- THE FIX: Check for battery in two places ---
                // 1. First, try the "BatteryPercentage" property on the Device1 interface.
                g_autoptr(GVariant) battery_var_device = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(device_interface), "BatteryPercentage");
                if (battery_var_device) {
                    battery_level = g_variant_get_byte(battery_var_device);
                } else {
                    // 2. If that fails, check for a separate "Battery1" interface on the same object.
                    g_autoptr(GDBusInterface) battery_interface = g_dbus_object_get_interface(obj, "org.bluez.Battery1");
                    if (battery_interface) {
                        g_autoptr(GVariant) battery_var_battery = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(battery_interface), "Percentage");
                        if (battery_var_battery) {
                            battery_level = g_variant_get_byte(battery_var_battery);
                        }
                    }
                }
                break;
            }
        }
    }

    if (connected_device_name) {
        g_autofree gchar *final_text = NULL;
        if (battery_level != -1) final_text = g_strdup_printf(" %d%% %s", battery_level, connected_device_name);
        else final_text = g_strdup_printf(" %s", connected_device_name);
        gtk_label_set_text(GTK_LABEL(module->glyph_label), HEADPHONE_ICON);
        gtk_label_set_text(GTK_LABEL(module->text_label), final_text);
    } else {
        gtk_label_set_text(GTK_LABEL(module->glyph_label), HEADPHONE_ICON);
        gtk_label_set_text(GTK_LABEL(module->text_label), " Disconnected");
    }
}

// (The rest of the file is included below but is unchanged)
static void on_bluez_properties_changed(GDBusObjectManagerClient *manager, GDBusObjectProxy *object, GDBusProxy *interface, GVariant *changed_properties, const gchar *const *invalidated_properties, gpointer user_data) { (void)manager; (void)object; (void)interface; (void)changed_properties; (void)invalidated_properties; update_bluetooth_status((BluetoothModule *)user_data); }
static void on_object_manager_created(GObject *source, GAsyncResult *res, gpointer user_data) { (void)source; BluetoothModule *module = (BluetoothModule *)user_data; g_autoptr(GError) error = NULL; module->bluez_manager = g_dbus_object_manager_client_new_for_bus_finish(res, &error); if (error) { g_warning("Failed to create BlueZ D-Bus object manager: %s", error->message); gtk_label_set_text(GTK_LABEL(module->text_label), " Error"); return; } update_bluetooth_status(module); g_signal_connect(module->bluez_manager, "interface-proxy-properties-changed", G_CALLBACK(on_bluez_properties_changed), module); }
static void bluetooth_module_cleanup(gpointer data) { BluetoothModule *module = (BluetoothModule *)data; if (module->bluez_manager) g_object_unref(module->bluez_manager); g_free(module); }
GtkWidget* create_bluetooth_module() { BluetoothModule *module = g_new0(BluetoothModule, 1); module->main_button = gtk_button_new(); gtk_widget_add_css_class(module->main_button, "bluetooth-module"); gtk_widget_add_css_class(module->main_button, "module"); gtk_widget_add_css_class(module->main_button, "flat"); gtk_widget_add_css_class(module->main_button, "group-start"); module->content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2); gtk_button_set_child(GTK_BUTTON(module->main_button), module->content_box); module->glyph_label = gtk_label_new(""); gtk_widget_add_css_class(module->glyph_label, "glyph-label"); module->text_label = gtk_label_new("..."); gtk_box_append(GTK_BOX(module->content_box), module->glyph_label); gtk_box_append(GTK_BOX(module->content_box), module->text_label); g_dbus_object_manager_client_new_for_bus( G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, "org.bluez", "/", NULL, NULL, NULL, NULL, (GAsyncReadyCallback)on_object_manager_created, module ); g_object_set_data_full(G_OBJECT(module->main_button), "module-state", module, bluetooth_module_cleanup); return module->main_button; }