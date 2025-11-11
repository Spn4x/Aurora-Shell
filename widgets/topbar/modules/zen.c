#include <gtk/gtk.h>
#include <gio/gio.h>
#include "zen.h"

// D-Bus constants for our own notification daemon
const char* NOTIFY_DAEMON_BUS_NAME = "org.freedesktop.Notifications";
const char* NOTIFY_DAEMON_OBJECT_PATH = "/org/freedesktop/Notifications";

typedef struct {
    GtkWidget *main_button;
    GtkWidget *content_box;
    GtkWidget *glyph_label;
    GtkWidget *text_label;
    GDBusProxy *notify_proxy; // Proxy to our auroranotifyd
} ZenModule;


// --- Forward Declarations ---
static void update_zen_ui(ZenModule *module, gboolean is_dnd);
static void update_from_proxy(ZenModule *module);
static void on_zen_clicked(GtkButton *button, gpointer user_data);
static gboolean on_zen_scroll(GtkEventControllerScroll* controller, double dx, double dy, gpointer user_data);
static void on_proxy_created(GObject *source_object, GAsyncResult *res, gpointer user_data);
static void on_properties_changed(GDBusProxy *proxy, GVariant *changed_properties, const gchar * const *invalidated_properties, gpointer user_data);
static void zen_module_cleanup(gpointer data);

static void update_zen_ui(ZenModule *module, gboolean is_dnd) {
    if (is_dnd) {
        gtk_label_set_text(GTK_LABEL(module->glyph_label), "󰂛");
        gtk_label_set_text(GTK_LABEL(module->text_label), " Zen");
        gtk_widget_add_css_class(module->main_button, "zen-active");
    } else {
        gtk_label_set_text(GTK_LABEL(module->glyph_label), "󰂚");
        gtk_label_set_text(GTK_LABEL(module->text_label), " Alert");
        gtk_widget_remove_css_class(module->main_button, "zen-active");
    }
}

static void update_from_proxy(ZenModule *module) {
    if (!module->notify_proxy) return;

    g_autoptr(GVariant) dnd_variant = g_dbus_proxy_get_cached_property(module->notify_proxy, "DND");
    if (dnd_variant) {
        update_zen_ui(module, g_variant_get_boolean(dnd_variant));
    } else {
        update_zen_ui(module, FALSE);
    }
}

// --- D-Bus Callbacks ---

static void on_proxy_created(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object;
    ZenModule *module = (ZenModule*)user_data;
    g_autoptr(GError) error = NULL;

    module->notify_proxy = g_dbus_proxy_new_for_bus_finish(res, &error);
    if (error) {
        g_warning("Zen Module: Failed to create proxy for notify daemon: %s", error->message);
        gtk_label_set_text(GTK_LABEL(module->text_label), " Error");
        return;
    }

    g_signal_connect(module->notify_proxy, "g-properties-changed", G_CALLBACK(on_properties_changed), module);
    update_from_proxy(module);
}

static void on_properties_changed(GDBusProxy *proxy, GVariant *changed_properties, const gchar * const *invalidated_properties, gpointer user_data) {
    (void)proxy; (void)invalidated_properties;
    ZenModule *module = (ZenModule*)user_data;
    
    if (g_variant_lookup_value(changed_properties, "DND", NULL)) {
        g_print("Zen Module: Received DND state change from daemon. Updating UI.\n");
        update_from_proxy(module);
    }
}

// --- UI Action Callbacks ---

// MODIFIED: This is now much simpler. It just calls ToggleDND.
static void on_zen_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    ZenModule *module = (ZenModule*)user_data;
    if (!module->notify_proxy) return;

    // Just tell the daemon to toggle. No need to know the current state.
    g_dbus_proxy_call(module->notify_proxy, "ToggleDND", NULL,
                      G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static gboolean on_zen_scroll(GtkEventControllerScroll* controller, double dx, double dy, gpointer user_data) {
    (void)controller; (void)dx;
    ZenModule *module = (ZenModule*)user_data;

    if (dy < 0) { // Scroll up toggles DND
        on_zen_clicked(NULL, module);
    } else if (dy > 0) { // Scroll down toggles the organizer
        system("aurora-shell --toggle organizer &");
    }
    return G_SOURCE_CONTINUE;
}

// --- Lifecycle ---

static void zen_module_cleanup(gpointer data) {
    ZenModule *module = (ZenModule *)data;
    g_clear_object(&module->notify_proxy);
    g_free(module);
}

GtkWidget* create_zen_module() {
    ZenModule *module = g_new0(ZenModule, 1);
    module->main_button = gtk_button_new();
    gtk_widget_add_css_class(module->main_button, "zen-module");
    gtk_widget_add_css_class(module->main_button, "module");
    gtk_widget_add_css_class(module->main_button, "flat");
    gtk_widget_add_css_class(module->main_button, "group-start");

    module->content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_button_set_child(GTK_BUTTON(module->main_button), module->content_box);

    module->glyph_label = gtk_label_new("?");
    gtk_widget_add_css_class(module->glyph_label, "glyph-label");
    module->text_label = gtk_label_new("...");

    gtk_box_append(GTK_BOX(module->content_box), module->glyph_label);
    gtk_box_append(GTK_BOX(module->content_box), module->text_label);

    g_signal_connect(module->main_button, "clicked", G_CALLBACK(on_zen_clicked), module);
    
    GtkEventController *scroll_controller = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll_controller, "scroll", G_CALLBACK(on_zen_scroll), module);
    gtk_widget_add_controller(module->main_button, scroll_controller);
    
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
                             NOTIFY_DAEMON_BUS_NAME, NOTIFY_DAEMON_OBJECT_PATH,
                             NOTIFY_DAEMON_BUS_NAME, NULL,
                             (GAsyncReadyCallback)on_proxy_created, module);

    g_object_set_data_full(G_OBJECT(module->main_button), "module-state", module, zen_module_cleanup);
    return module->main_button;
}