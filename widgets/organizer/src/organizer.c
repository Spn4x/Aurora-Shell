#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <gio/gio.h>
#include <gdk/gdkkeysyms.h>
#include "calendar_widget.h"

const char* CENTER_BUS_NAME = "com.meismeric.auranotify.Center";
const char* CENTER_OBJECT_PATH = "/com/meismeric/auranotify/Center";
const char* DAEMON_BUS_NAME = "org.freedesktop.Notifications";
const char* DAEMON_OBJECT_PATH = "/org/freedesktop/Notifications";

typedef struct {
    GtkWidget *main_hbox;
    GtkBox *notification_list;
    GtkWidget *content_stack;
    guint owner_id;
    GDBusConnection *dbus_connection;
    GDBusProxy *notify_proxy;
    gulong notify_prop_changed_id;
    GtkWidget *dnd_switch;
} OrganizerState;

static void update_placeholder_visibility(OrganizerState *state);
static void on_close_clicked(GtkButton *button, gpointer user_data);
static void dbus_method_call_handler(GDBusConnection *c, const gchar *s, const gchar *o, const gchar *i, const gchar *m, GVariant *p, GDBusMethodInvocation *inv, gpointer user_data);
static void on_organizer_proxy_created(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_organizer_properties_changed(GDBusProxy *proxy, GVariant *changed_properties, const gchar *const *invalidated, gpointer user_data);

static gboolean remove_widget_from_parent(gpointer user_data) {
    GtkWidget *widget = GTK_WIDGET(user_data);
    GtkWidget *parent = gtk_widget_get_parent(widget);
    if (GTK_IS_BOX(parent)) {
        OrganizerState *state = g_object_get_data(G_OBJECT(gtk_widget_get_root(parent)), "organizer-state");
        gtk_box_remove(GTK_BOX(parent), widget);
        if (state) {
            update_placeholder_visibility(state);
        }
    }
    return G_SOURCE_REMOVE;
}

static void on_close_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWidget *revealer = GTK_WIDGET(user_data);
    guint transition_duration = gtk_revealer_get_transition_duration(GTK_REVEALER(revealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);
    g_timeout_add(transition_duration + 50, remove_widget_from_parent, revealer);
}

static GtkWidget* create_notification_widget(const gchar *summary, const gchar *body) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(card, "notification-card");
    gtk_widget_set_margin_bottom(card, 10);

    GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *summary_label = gtk_label_new(summary);
    gtk_widget_set_halign(summary_label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(summary_label, TRUE);
    gtk_label_set_wrap(GTK_LABEL(summary_label), TRUE);
    gtk_widget_add_css_class(summary_label, "summary");

    GtkWidget *close_button = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_valign(close_button, GTK_ALIGN_START);

    gtk_box_append(GTK_BOX(top_row), summary_label);
    gtk_box_append(GTK_BOX(top_row), close_button);

    GtkWidget *body_label = gtk_label_new(body);
    gtk_widget_set_halign(body_label, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(body_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(body_label), 0.0);
    gtk_widget_add_css_class(body_label, "body");

    gtk_box_append(GTK_BOX(card), top_row);
    if (body && *body) {
        gtk_box_append(GTK_BOX(card), body_label);
    }

    GtkWidget *revealer = gtk_revealer_new();
    gtk_revealer_set_child(GTK_REVEALER(revealer), card);
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), 250);

    g_signal_connect(close_button, "clicked", G_CALLBACK(on_close_clicked), revealer);
    
    return revealer;
}

static void update_placeholder_visibility(OrganizerState *state) {
    if (!state || !state->content_stack) return;
    GtkWidget *first_child = gtk_widget_get_first_child(GTK_WIDGET(state->notification_list));
    GtkWidget *footer = gtk_widget_get_last_child(gtk_widget_get_parent(state->content_stack));
    if (first_child == NULL) {
        gtk_stack_set_visible_child_name(GTK_STACK(state->content_stack), "placeholder");
        gtk_widget_set_visible(footer, FALSE);
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(state->content_stack), "list");
        gtk_widget_set_visible(footer, TRUE);
    }
}

static void notify_daemon_of_dnd_state(OrganizerState *state, gboolean is_dnd_active) {
    if (!state->notify_proxy) {
        g_warning("Organizer: DND toggled but notify_proxy is not available.");
        return;
    }
    g_print("Organizer: Notifying daemon, DND active: %s\n", is_dnd_active ? "TRUE" : "FALSE");
    g_dbus_proxy_call(state->notify_proxy, "SetDND", g_variant_new("(b)", is_dnd_active),
                      G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void on_dnd_toggled(GtkSwitch *widget, gboolean state, gpointer user_data) {
    (void)widget;
    OrganizerState *state_ptr = user_data;
    notify_daemon_of_dnd_state(state_ptr, state);
}

static void on_clear_all_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    OrganizerState *state = (OrganizerState *)user_data;
    if (!state->notification_list) return;
    
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(state->notification_list));
    while (child) {
        GtkWidget *next_sibling = gtk_widget_get_next_sibling(child);
        GtkWidget* card = gtk_revealer_get_child(GTK_REVEALER(child));
        GtkWidget* top_row = gtk_widget_get_first_child(card);
        GtkWidget* close_button = gtk_widget_get_last_child(top_row);

        if (GTK_IS_BUTTON(close_button)) {
            on_close_clicked(GTK_BUTTON(close_button), child);
        }
        child = next_sibling;
    }
}

static void on_organizer_properties_changed(GDBusProxy *proxy, GVariant *changed_properties, const gchar *const *invalidated, gpointer user_data) {
    (void)proxy; (void)invalidated;
    OrganizerState *state = user_data;
    if (!state->dnd_switch) return;
    
    g_autoptr(GVariant) dnd_variant = g_variant_lookup_value(changed_properties, "DND", G_VARIANT_TYPE_BOOLEAN);
    if (dnd_variant) {
        g_signal_handlers_block_by_func(state->dnd_switch, G_CALLBACK(on_dnd_toggled), state);
        gtk_switch_set_active(GTK_SWITCH(state->dnd_switch), g_variant_get_boolean(dnd_variant));
        g_signal_handlers_unblock_by_func(state->dnd_switch, G_CALLBACK(on_dnd_toggled), state);
    }
}

static void on_organizer_proxy_created(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    OrganizerState *state = user_data;
    g_autoptr(GError) error = NULL;
    state->notify_proxy = g_dbus_proxy_new_for_bus_finish(res, &error);
    if (error) {
        g_warning("Organizer: Failed to create proxy for notify daemon: %s", error->message);
        return;
    }
    
    g_autoptr(GVariant) dnd_variant = g_dbus_proxy_get_cached_property(state->notify_proxy, "DND");
    if (dnd_variant) {
        gtk_switch_set_active(GTK_SWITCH(state->dnd_switch), g_variant_get_boolean(dnd_variant));
    }

    state->notify_prop_changed_id = g_signal_connect(state->notify_proxy, "g-properties-changed", G_CALLBACK(on_organizer_properties_changed), state);
}

static void handle_add_notification_dbus(GVariant *p, OrganizerState *state) {
    if (!state || !state->main_hbox) return;
    gchar *icon, *summary, *body;
    g_variant_get(p, "(&s&s&s)", &icon, &summary, &body);
    GtkWidget *notification_revealer = create_notification_widget(summary, body);
    gtk_box_prepend(state->notification_list, notification_revealer);
    update_placeholder_visibility(state);
    gtk_revealer_set_reveal_child(GTK_REVEALER(notification_revealer), TRUE);
}

static void dbus_method_call_handler(GDBusConnection *c, const gchar *s, const gchar *o, const gchar *i, const gchar *m, GVariant *p, GDBusMethodInvocation *inv, gpointer user_data) {
    (void)c; (void)s; (void)o; (void)i;
    OrganizerState *state = (OrganizerState *)user_data;
    if (g_strcmp0(m, "AddNotification") == 0) {
        handle_add_notification_dbus(p, state);
    } else {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method %s", m);
        return;
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}

static const GDBusInterfaceVTable interface_vtable = { .method_call = dbus_method_call_handler };

static void on_bus_acquired(GDBusConnection *c, const gchar *n, gpointer user_data) {
    (void)n;
    OrganizerState *state = (OrganizerState *)user_data;
    const gchar* xml = "<node><interface name='com.meismeric.auranotify.Center'><method name='AddNotification'><arg type='s' name='icon' direction='in'/><arg type='s' name='summary' direction='in'/><arg type='s' name='body' direction='in'/></method></interface></node>";
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, NULL);
    g_dbus_connection_register_object(c, CENTER_OBJECT_PATH, node->interfaces[0], &interface_vtable, state, NULL, NULL);
    g_dbus_node_info_unref(node);
    g_print("Organizer Plugin: D-Bus Service is running.\n");
}

static void plugin_cleanup(gpointer data) {
    OrganizerState *state = (OrganizerState *)data;
    if (!state) return;
    if (state->owner_id > 0) {
        g_bus_unown_name(state->owner_id);
    }
    if (state->notify_proxy && state->notify_prop_changed_id > 0) {
        g_signal_handler_disconnect(state->notify_proxy, state->notify_prop_changed_id);
    }
    g_clear_object(&state->notify_proxy);
    g_clear_object(&state->dbus_connection);
    g_free(state);
    g_print("Organizer Plugin: Cleaned up.\n");
}

G_MODULE_EXPORT GtkWidget* create_widget(const char *config_string) {
    (void)config_string;

    OrganizerState *state = g_new0(OrganizerState, 1);
    
    GtkWidget *main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_set_homogeneous(GTK_BOX(main_hbox), FALSE);
    gtk_widget_set_name(main_hbox, "organizer-widget");
    g_object_set_data_full(G_OBJECT(main_hbox), "organizer-state", state, plugin_cleanup);
    state->main_hbox = main_hbox;
    
    GtkWidget *notification_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(notification_vbox, "notification-pane");
    gtk_widget_set_hexpand(notification_vbox, TRUE);

    state->content_stack = gtk_stack_new();
    gtk_widget_set_vexpand(state->content_stack, TRUE);

    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled_window), FALSE);

    state->notification_list = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_widget_set_valign(GTK_WIDGET(state->notification_list), GTK_ALIGN_START);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), GTK_WIDGET(state->notification_list));
    gtk_stack_add_named(GTK_STACK(state->content_stack), scrolled_window, "list");

    GtkWidget *placeholder_label = gtk_label_new("No Notifications");
    gtk_widget_add_css_class(placeholder_label, "placeholder");
    gtk_widget_set_vexpand(placeholder_label, TRUE);
    gtk_stack_add_named(GTK_STACK(state->content_stack), placeholder_label, "placeholder");
    
    GtkWidget *footer_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(footer_box, "notification-footer");

    GtkWidget *dnd_label = gtk_label_new("Do Not Disturb");
    state->dnd_switch = gtk_switch_new();
    gtk_widget_add_css_class(state->dnd_switch, "dnd-switch");
    g_signal_connect(state->dnd_switch, "state-set", G_CALLBACK(on_dnd_toggled), state);

    GtkWidget *clear_button = gtk_button_new_with_label("Clear");
    gtk_widget_add_css_class(clear_button, "clear-all-button");
    gtk_widget_set_hexpand(clear_button, TRUE);
    gtk_widget_set_halign(clear_button, GTK_ALIGN_END);
    g_signal_connect(clear_button, "clicked", G_CALLBACK(on_clear_all_clicked), state);

    gtk_box_append(GTK_BOX(footer_box), dnd_label);
    gtk_box_append(GTK_BOX(footer_box), state->dnd_switch);
    gtk_box_append(GTK_BOX(footer_box), clear_button);

    gtk_box_append(GTK_BOX(notification_vbox), state->content_stack);
    gtk_box_append(GTK_BOX(notification_vbox), footer_box);

    GtkWidget *calendar = calendar_widget_new();

    gtk_box_append(GTK_BOX(main_hbox), notification_vbox);
    gtk_box_append(GTK_BOX(main_hbox), calendar);

    update_placeholder_visibility(state);

    state->dbus_connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    state->owner_id = g_bus_own_name_on_connection(state->dbus_connection, CENTER_BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                                                   on_bus_acquired, NULL, state, NULL);

    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
                             DAEMON_BUS_NAME, DAEMON_OBJECT_PATH, DAEMON_BUS_NAME,
                             NULL, (GAsyncReadyCallback)on_organizer_proxy_created, state);

    return main_hbox;
}