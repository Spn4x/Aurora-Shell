#include <gtk/gtk.h>
#include <gio/gio.h>
#include <stdlib.h> // For system()
#include "audio.h"

// Struct to hold info about a single audio sink
typedef struct {
    uint32_t id;
    gchar *description;
} AudioSink;

typedef struct {
    GtkWidget *main_button;
    GtkWidget *content_box;
    GtkWidget *glyph_label;
    GtkWidget *text_label;
    GDBusObjectManager *bluez_manager;
    GtkWidget *popover;
    GtkWidget *sink_list_box;
} AudioModule;

// --- Forward Declarations ---
static void on_module_clicked(GtkButton *button, gpointer user_data);
static void update_bluetooth_status(AudioModule *module);
static void on_object_manager_created(GObject *source, GAsyncResult *res, gpointer user_data);
static void audio_module_cleanup(gpointer data);

static void audio_sink_free(gpointer data) {
    AudioSink *sink = (AudioSink*)data;
    g_free(sink->description);
    g_free(sink);
}


// =======================================================================
// NEW: The Glyph Helper Function
// This function inspects the sink description and returns a glyph.
// =======================================================================
static const char* get_glyph_for_sink(const gchar* description) {
    if (!description) return "󰗟"; // audio-description-symbolic (fallback)

    // Case-insensitive search is more robust
    g_autofree gchar *lower_desc = g_ascii_strdown(description, -1);

    // Bluetooth Devices (Headphones, Headsets, Earbuds)
    if (strstr(lower_desc, "bluez") || strstr(lower_desc, "earphone") || 
        strstr(lower_desc, "headphone") || strstr(lower_desc, "headset") ||
        strstr(lower_desc, "soundcore") || strstr(lower_desc, "r50i")) {
        return "󰋋"; // headphones-symbolic
    }
    // Phone
    if (strstr(lower_desc, "phone")) {
        return "󰄄"; // cellphone-symbolic
    }
    // Speakers / Built-in Audio
    if (strstr(lower_desc, "speaker") || strstr(lower_desc, "built-in") || 
        strstr(lower_desc, "analog")) {
        return "󰕾"; // audio-volume-high-symbolic
    }
    // Loopback devices
    if (strstr(lower_desc, "loopback")) {
        return "󰑪"; // object-rotate-right-symbolic (sync/loop icon)
    }

    // Default fallback
    return "󰗟";
}


// --- UI Functions ---

static void on_sink_button_clicked(GtkButton *button, gpointer data) {
    AudioSink *sink = (AudioSink*)data;
    g_autofree gchar *command = g_strdup_printf("wpctl set-default %u", sink->id);
    system(command);
    
    GtkPopover* popover = GTK_POPOVER(gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER));
    if (popover) gtk_popover_popdown(popover);
}

// UPGRADED: This function now builds the richer list item with a glyph
static void update_sink_list_ui(AudioModule *module) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(module->sink_list_box))) {
        gtk_box_remove(GTK_BOX(module->sink_list_box), child);
    }

    FILE *fp = popen("wpctl status | awk '/Sinks:/, /Sources:/' | grep -E '[0-9]+\\.' | sed -e 's/[│]//g' -e 's/ \\[[^]]*\\]//'", "r");
    if (!fp) return;

    gint current_sink_id = -1;
    FILE *fp_default = popen("wpctl status | awk '/Default Sink:/ {print $3}'", "r");
    if (fp_default) { char id_buf[16]; if (fgets(id_buf, sizeof(id_buf), fp_default)) current_sink_id = atoi(id_buf); pclose(fp_default); }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        gboolean is_default = FALSE;
        char* current_pos = line;
        while (g_ascii_isspace(*current_pos)) current_pos++;
        if (*current_pos == '*') { is_default = TRUE; current_pos++; while (g_ascii_isspace(*current_pos)) current_pos++; }
        
        char *id_str = strtok(current_pos, ".");
        char *desc_str = strtok(NULL, "");

        if (id_str && desc_str) {
            g_strstrip(desc_str);
            
            AudioSink *sink = g_new0(AudioSink, 1);
            sink->id = atoi(id_str);
            sink->description = g_strdup(desc_str);
            
            // --- NEW: Create a richer button ---
            GtkWidget *button = gtk_button_new();
            gtk_widget_add_css_class(button, "sink-button");
            gtk_widget_add_css_class(button, "flat");

            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_button_set_child(GTK_BUTTON(button), box);

            const char* glyph = get_glyph_for_sink(sink->description);
            GtkWidget *glyph_label = gtk_label_new(glyph);
            gtk_widget_add_css_class(glyph_label, "glyph-label");
            
            GtkWidget *desc_label = gtk_label_new(sink->description);
            gtk_label_set_xalign(GTK_LABEL(desc_label), 0.0);
            gtk_widget_set_hexpand(desc_label, TRUE);

            gtk_box_append(GTK_BOX(box), glyph_label);
            gtk_box_append(GTK_BOX(box), desc_label);
            // --- End of new button creation ---

            if (is_default) gtk_widget_add_css_class(button, "active-sink");

            g_object_set_data_full(G_OBJECT(button), "sink-data", sink, audio_sink_free);
            g_signal_connect(button, "clicked", G_CALLBACK(on_sink_button_clicked), sink);
            gtk_box_append(GTK_BOX(module->sink_list_box), button);
        }
    }
    pclose(fp);
}

// (The rest of the file is unchanged)
static void on_module_clicked(GtkButton *button, gpointer user_data) { (void)button; AudioModule *module = (AudioModule *)user_data; update_sink_list_ui(module); gtk_popover_popup(GTK_POPOVER(module->popover)); }
static void on_bluez_properties_changed(GDBusObjectManagerClient *m, GDBusObjectProxy *o, GDBusProxy *i, GVariant *c, const gchar *const *iv, gpointer d) { (void)m; (void)o; (void)i; (void)c; (void)iv; update_bluetooth_status((AudioModule*)d); }
static void on_object_manager_created(GObject *s, GAsyncResult *r, gpointer d) { (void)s; AudioModule *m = (AudioModule*)d; g_autoptr(GError) e = NULL; m->bluez_manager = g_dbus_object_manager_client_new_for_bus_finish(r, &e); if (e) return; update_bluetooth_status(m); g_signal_connect(m->bluez_manager, "interface-proxy-properties-changed", G_CALLBACK(on_bluez_properties_changed), m); }
static void update_bluetooth_status(AudioModule *module) { const char *HEADPHONE_ICON = ""; gboolean is_powered = FALSE; g_autofree gchar *connected_device_name = NULL; gint battery_level = -1; if (!module->bluez_manager) { return; } g_autoptr(GDBusInterface) adapter_interface = g_dbus_object_manager_get_interface(module->bluez_manager, "/org/bluez/hci0", "org.bluez.Adapter1"); if (adapter_interface) { g_autoptr(GVariant) powered_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(adapter_interface), "Powered"); if (powered_var) is_powered = g_variant_get_boolean(powered_var); } if (!is_powered) { gtk_label_set_text(GTK_LABEL(module->glyph_label), HEADPHONE_ICON); gtk_label_set_text(GTK_LABEL(module->text_label), " Disconnected"); return; } g_autoptr(GList) devices = g_dbus_object_manager_get_objects(module->bluez_manager); for (GList *l = devices; l != NULL; l = l->next) { GDBusObject *obj = G_DBUS_OBJECT(l->data); g_autoptr(GDBusInterface) device_interface = g_dbus_object_get_interface(obj, "org.bluez.Device1"); if (device_interface) { g_autoptr(GVariant) connected_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(device_interface), "Connected"); if (connected_var && g_variant_get_boolean(connected_var)) { g_autoptr(GVariant) name_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(device_interface), "Name"); if (name_var) connected_device_name = g_variant_dup_string(name_var, NULL); g_autoptr(GVariant) battery_var_device = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(device_interface), "BatteryPercentage"); if (battery_var_device) { battery_level = g_variant_get_byte(battery_var_device); } else { g_autoptr(GDBusInterface) battery_interface = g_dbus_object_get_interface(obj, "org.bluez.Battery1"); if (battery_interface) { g_autoptr(GVariant) battery_var_battery = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(battery_interface), "Percentage"); if (battery_var_battery) { battery_level = g_variant_get_byte(battery_var_battery); } } } break; } } } if (connected_device_name) { g_autofree gchar *final_text = NULL; if (battery_level != -1) final_text = g_strdup_printf(" %d%% %s", battery_level, connected_device_name); else final_text = g_strdup_printf(" %s", connected_device_name); gtk_label_set_text(GTK_LABEL(module->glyph_label), HEADPHONE_ICON); gtk_label_set_text(GTK_LABEL(module->text_label), final_text); } else { gtk_label_set_text(GTK_LABEL(module->glyph_label), HEADPHONE_ICON); gtk_label_set_text(GTK_LABEL(module->text_label), " Disconnected"); } }
static void audio_module_cleanup(gpointer data) { AudioModule *module = (AudioModule *)data; if (module->bluez_manager) g_object_unref(module->bluez_manager); g_free(module); }
GtkWidget* create_audio_module() { AudioModule *module = g_new0(AudioModule, 1); module->main_button = gtk_button_new(); gtk_widget_add_css_class(module->main_button, "audio-module"); gtk_widget_add_css_class(module->main_button, "module"); gtk_widget_add_css_class(module->main_button, "flat"); module->content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2); gtk_button_set_child(GTK_BUTTON(module->main_button), module->content_box); module->glyph_label = gtk_label_new(""); gtk_widget_add_css_class(module->glyph_label, "glyph-label"); module->text_label = gtk_label_new("..."); gtk_box_append(GTK_BOX(module->content_box), module->glyph_label); gtk_box_append(GTK_BOX(module->content_box), module->text_label); module->popover = gtk_popover_new(); gtk_widget_set_parent(module->popover, module->main_button); module->sink_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4); gtk_widget_add_css_class(module->sink_list_box, "sink-list-popover"); gtk_popover_set_child(GTK_POPOVER(module->popover), module->sink_list_box); g_signal_connect(module->main_button, "clicked", G_CALLBACK(on_module_clicked), module); g_dbus_object_manager_client_new_for_bus( G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, "org.bluez", "/", NULL, NULL, NULL, NULL, (GAsyncReadyCallback)on_object_manager_created, module ); g_object_set_data_full(G_OBJECT(module->main_button), "module-state", module, audio_module_cleanup); return module->main_button; }