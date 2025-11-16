#include <gtk/gtk.h>
#include <gio/gio.h>
#include "zen.h"

const char* NOTIFY_DAEMON_BUS_NAME = "org.freedesktop.Notifications";
const char* NOTIFY_DAEMON_OBJECT_PATH = "/org/freedesktop/Notifications";
const char* NOTIFY_DAEMON_INTERFACE = "org.freedesktop.Notifications";

typedef struct {
    GtkWidget *main_button;
    GtkWidget *content_box;
    GtkWidget *glyph_label;
    GtkWidget *text_label;
    guint signal_subscription_id;
} ZenModule;

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

static void on_dnd_state_changed(GDBusConnection *connection, const gchar *sender_name, const gchar *object_path, const gchar *interface_name, const gchar *signal_name, GVariant *parameters, gpointer user_data) {
    (void)connection; (void)sender_name; (void)object_path; (void)interface_name; (void)signal_name;
    ZenModule *module = (ZenModule*)user_data;
    gboolean is_active;
    g_variant_get(parameters, "(b)", &is_active);
    update_zen_ui(module, is_active);
}

static void on_get_initial_dnd_state(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    ZenModule *module = (ZenModule*)user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source_object), res, &error);
    if (error) {
        g_warning("Zen Module: Could not get initial DND state: %s", error->message);
        return;
    }
    gboolean is_active;
    g_variant_get(result, "(b)", &is_active);
    update_zen_ui(module, is_active);
}

static void on_zen_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    g_autoptr(GDBusProxy) proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL, NOTIFY_DAEMON_BUS_NAME, NOTIFY_DAEMON_OBJECT_PATH, NOTIFY_DAEMON_INTERFACE, NULL, NULL);
    if(proxy) { g_dbus_proxy_call_sync(proxy, "ToggleDND", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL); }
}

static gboolean on_zen_scroll(GtkEventControllerScroll* controller, double dx, double dy, gpointer user_data) {
    (void)controller; (void)dx; (void)user_data;
    if (dy < 0) { on_zen_clicked(NULL, NULL); } 
    else if (dy > 0) { system("aurora-shell --toggle organizer &"); }
    return TRUE;
}

static void zen_module_cleanup(gpointer data) {
    ZenModule *module = (ZenModule *)data;
    if (module->signal_subscription_id > 0) {
        g_dbus_connection_signal_unsubscribe(g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL), module->signal_subscription_id);
    }
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

    module->glyph_label = gtk_label_new("󰂚");
    gtk_widget_add_css_class(module->glyph_label, "glyph-label");
    module->text_label = gtk_label_new(" Alert");
    gtk_box_append(GTK_BOX(module->content_box), module->glyph_label);
    gtk_box_append(GTK_BOX(module->content_box), module->text_label);

    g_signal_connect(module->main_button, "clicked", G_CALLBACK(on_zen_clicked), module);
    
    GtkEventController *scroll_controller = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll_controller, "scroll", G_CALLBACK(on_zen_scroll), module);
    gtk_widget_add_controller(module->main_button, scroll_controller);
    
    module->signal_subscription_id = g_dbus_connection_signal_subscribe(g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL), NOTIFY_DAEMON_BUS_NAME, NOTIFY_DAEMON_INTERFACE, "DNDStateChanged", NOTIFY_DAEMON_OBJECT_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_dnd_state_changed, module, NULL);
    
    g_autoptr(GDBusProxy) proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL, NOTIFY_DAEMON_BUS_NAME, NOTIFY_DAEMON_OBJECT_PATH, NOTIFY_DAEMON_INTERFACE, NULL, NULL);
    if(proxy) { g_dbus_proxy_call(proxy, "GetDNDState", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, (GAsyncReadyCallback)on_get_initial_dnd_state, module); }

    g_object_set_data_full(G_OBJECT(module->main_button), "module-state", module, zen_module_cleanup);
    return module->main_button;
}