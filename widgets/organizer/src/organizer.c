#include <gtk/gtk.h>
#include <adwaita.h>
#include <gtk4-layer-shell.h>
#include <gio/gio.h>
#include <gdk/gdkkeysyms.h>
#include "calendar_widget.h"
#include "mpris_widget.h" // Added

const char* CENTER_BUS_NAME = "com.meismeric.auranotify.Center";
const char* CENTER_OBJECT_PATH = "/com/meismeric/auranotify/Center";
const char* DAEMON_BUS_NAME = "org.freedesktop.Notifications";
const char* DAEMON_OBJECT_PATH = "/org/freedesktop/Notifications";
const char* DAEMON_INTERFACE_NAME = "org.freedesktop.Notifications";

typedef struct {
    GtkWidget *main_hbox;
    GtkBox *notification_list;
    GtkWidget *content_stack;
    guint owner_id;
    GDBusConnection *dbus_connection;
    guint dnd_signal_id;
    GtkWidget *dnd_switch;
} OrganizerState;

// Data attached to each Group Widget
typedef struct {
    GtkWidget *header_btn;
    GtkWidget *count_label;
    GtkWidget *chevron_icon;
    GtkWidget *latest_box;      // Holds the 1 newest card
    GtkWidget *history_revealer; // Animates the older cards
    GtkWidget *history_box;     // Holds cards 2..N
    char *app_name;
} GroupWidgets;

static void update_placeholder_visibility(OrganizerState *state);
static void on_close_clicked(GtkButton *button, gpointer user_data);
static void dbus_method_call_handler(GDBusConnection *c, const gchar *s, const gchar *o, const gchar *i, const gchar *m, GVariant *p, GDBusMethodInvocation *inv, gpointer user_data);

// --- Group Logic ---

static void update_group_header(GroupWidgets *g) {
    int latest_count = 0;
    for (GtkWidget *c = gtk_widget_get_first_child(g->latest_box); c; c = gtk_widget_get_next_sibling(c)) latest_count++;
    
    int history_count = 0;
    for (GtkWidget *c = gtk_widget_get_first_child(g->history_box); c; c = gtk_widget_get_next_sibling(c)) history_count++;

    int total = latest_count + history_count;
    
    // Update Count Label
    g_autofree gchar *text = g_strdup_printf("%d", total);
    gtk_label_set_text(GTK_LABEL(g->count_label), text);
    
    // Only show chevron and enable toggle if we have history
    if (history_count > 0) {
        gtk_widget_set_visible(g->chevron_icon, TRUE);
        gtk_widget_set_visible(g->count_label, TRUE);
        gtk_widget_set_sensitive(g->header_btn, TRUE);
    } else {
        gtk_widget_set_visible(g->chevron_icon, FALSE);
        gtk_widget_set_visible(g->count_label, total > 1);
        gtk_widget_set_sensitive(g->header_btn, FALSE);
        gtk_revealer_set_reveal_child(GTK_REVEALER(g->history_revealer), FALSE);
    }
}

static void on_group_header_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GroupWidgets *g = (GroupWidgets *)user_data;
    
    gboolean current = gtk_revealer_get_reveal_child(GTK_REVEALER(g->history_revealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(g->history_revealer), !current);
    
    gtk_image_set_from_icon_name(GTK_IMAGE(g->chevron_icon), 
        !current ? "pan-down-symbolic" : "pan-end-symbolic");
}

static void destroy_group_data(gpointer data) {
    GroupWidgets *g = (GroupWidgets *)data;
    g_free(g->app_name);
    g_free(g);
}

static gboolean remove_widget_from_parent(gpointer user_data) {
    GtkWidget *widget = GTK_WIDGET(user_data);
    GtkWidget *parent = gtk_widget_get_parent(widget);
    
    if (parent) {
        GtkWidget *box = parent;
        GtkWidget *group_wrapper = NULL;
        GtkWidget *grandparent = gtk_widget_get_parent(box);
        if (GTK_IS_REVEALER(grandparent)) {
            group_wrapper = gtk_widget_get_parent(grandparent);
        } else {
            group_wrapper = grandparent;
        }

        GroupWidgets *g = g_object_get_data(G_OBJECT(group_wrapper), "group-data");
        
        gtk_box_remove(GTK_BOX(box), widget);

        if (g) {
            if (box == g->latest_box) {
                GtkWidget *new_top = gtk_widget_get_first_child(g->history_box);
                if (new_top) {
                    g_object_ref(new_top);
                    gtk_box_remove(GTK_BOX(g->history_box), new_top);
                    gtk_box_append(GTK_BOX(g->latest_box), new_top);
                    g_object_unref(new_top);
                }
            }
            update_group_header(g);

            if (gtk_widget_get_first_child(g->latest_box) == NULL) {
                GtkWidget *list_container = gtk_widget_get_parent(group_wrapper);
                gtk_box_remove(GTK_BOX(list_container), group_wrapper);
                OrganizerState *state = g_object_get_data(G_OBJECT(list_container), "organizer-state");
                if (state) update_placeholder_visibility(state);
            }
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
    gtk_widget_add_css_class(close_button, "flat");
    gtk_widget_add_css_class(close_button, "circular");
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

static GtkWidget* find_app_group(GtkBox *list, const gchar *app_name) {
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(list));
    while (child) {
        GroupWidgets *g = g_object_get_data(G_OBJECT(child), "group-data");
        if (g && g_strcmp0(g->app_name, app_name) == 0) {
            return child;
        }
        child = gtk_widget_get_next_sibling(child);
    }
    return NULL;
}

static GtkWidget* create_app_group(const gchar *app_name, const gchar *icon_name) {
    GroupWidgets *g = g_new0(GroupWidgets, 1);
    g->app_name = g_strdup(app_name);

    GtkWidget *group_wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_bottom(group_wrapper, 12);
    g_object_set_data_full(G_OBJECT(group_wrapper), "group-data", g, destroy_group_data);

    // --- Header ---
    g->header_btn = gtk_button_new();
    gtk_widget_add_css_class(g->header_btn, "flat");
    gtk_widget_set_halign(g->header_btn, GTK_ALIGN_FILL);
    gtk_widget_set_margin_bottom(g->header_btn, 4);
    
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
    gtk_widget_set_opacity(icon, 0.7);

    GtkWidget *label = gtk_label_new(app_name);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(label, TRUE); 
    gtk_widget_add_css_class(label, "heading");
    gtk_widget_add_css_class(label, "dim-label");

    g->count_label = gtk_label_new("1");
    gtk_widget_add_css_class(g->count_label, "numeric");
    gtk_widget_set_opacity(g->count_label, 0.6);
    
    g->chevron_icon = gtk_image_new_from_icon_name("pan-end-symbolic");
    gtk_widget_set_opacity(g->chevron_icon, 0.5);

    gtk_box_append(GTK_BOX(header_box), icon);
    gtk_box_append(GTK_BOX(header_box), label);
    gtk_box_append(GTK_BOX(header_box), g->count_label);
    gtk_box_append(GTK_BOX(header_box), g->chevron_icon);
    
    gtk_button_set_child(GTK_BUTTON(g->header_btn), header_box);
    g_signal_connect(g->header_btn, "clicked", G_CALLBACK(on_group_header_clicked), g);

    // --- Latest Slot ---
    g->latest_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    // --- History Stack ---
    g->history_revealer = gtk_revealer_new();
    gtk_revealer_set_reveal_child(GTK_REVEALER(g->history_revealer), FALSE);
    gtk_revealer_set_transition_type(GTK_REVEALER(g->history_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    
    g->history_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    // REMOVED INDENTATION: Aligns with header now
    gtk_revealer_set_child(GTK_REVEALER(g->history_revealer), g->history_box);

    gtk_box_append(GTK_BOX(group_wrapper), g->header_btn);
    gtk_box_append(GTK_BOX(group_wrapper), g->latest_box);
    gtk_box_append(GTK_BOX(group_wrapper), g->history_revealer);

    update_group_header(g);
    return group_wrapper;
}

static void update_placeholder_visibility(OrganizerState *state) {
    if (!state || !state->content_stack) return;
    GtkWidget *first_child = gtk_widget_get_first_child(GTK_WIDGET(state->notification_list));
    gtk_stack_set_visible_child_name(GTK_STACK(state->content_stack), first_child ? "list" : "placeholder");
}

static void notify_daemon_of_dnd_state(gboolean is_dnd_active) {
    g_autoptr(GDBusProxy) proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL, DAEMON_BUS_NAME, DAEMON_OBJECT_PATH, DAEMON_INTERFACE_NAME, NULL, NULL);
    if(proxy) {
        g_dbus_proxy_call_sync(proxy, "SetDND", g_variant_new("(b)", is_dnd_active), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    }
}

static void on_dnd_toggled(GtkSwitch *widget, gboolean state, gpointer user_data) {
    (void)widget; (void)user_data;
    notify_daemon_of_dnd_state(state);
}

static void on_clear_all_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    OrganizerState *state = (OrganizerState *)user_data;
    if (!state->notification_list) return;
    
    // Clear everything
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(state->notification_list));
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        GroupWidgets *g = g_object_get_data(G_OBJECT(child), "group-data");
        
        // Helper to close all cards in a box
        GtkWidget *n = gtk_widget_get_first_child(g->latest_box);
        while(n) {
             GtkWidget *nn = gtk_widget_get_next_sibling(n);
             GtkWidget* card = gtk_revealer_get_child(GTK_REVEALER(n));
             GtkWidget* top_row = gtk_widget_get_first_child(card);
             GtkWidget* close_btn = gtk_widget_get_last_child(top_row);
             if (GTK_IS_BUTTON(close_btn)) { g_signal_emit_by_name(close_btn, "clicked"); }
             n = nn;
        }
        n = gtk_widget_get_first_child(g->history_box);
        while(n) {
             GtkWidget *nn = gtk_widget_get_next_sibling(n);
             GtkWidget* card = gtk_revealer_get_child(GTK_REVEALER(n));
             GtkWidget* top_row = gtk_widget_get_first_child(card);
             GtkWidget* close_btn = gtk_widget_get_last_child(top_row);
             if (GTK_IS_BUTTON(close_btn)) { g_signal_emit_by_name(close_btn, "clicked"); }
             n = nn;
        }
        child = next;
    }
    
    // Force check
    g_timeout_add(400, (GSourceFunc)update_placeholder_visibility, state);
}

static void on_dnd_state_changed_signal(GDBusConnection *c, const gchar *s, const gchar *o, const gchar *i, const gchar *sig, GVariant *p, gpointer user_data) {
    (void)c; (void)s; (void)o; (void)i; (void)sig;
    OrganizerState *state = user_data;
    if (!state->dnd_switch) return;
    gboolean is_active;
    g_variant_get(p, "(b)", &is_active);
    g_signal_handlers_block_by_func(state->dnd_switch, G_CALLBACK(on_dnd_toggled), state);
    gtk_switch_set_active(GTK_SWITCH(state->dnd_switch), is_active);
    g_signal_handlers_unblock_by_func(state->dnd_switch, G_CALLBACK(on_dnd_toggled), state);
}

static void on_get_initial_dnd_state(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    OrganizerState *state = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source_object), res, &error);
    if(error) { return; }
    gboolean is_active;
    g_variant_get(result, "(b)", &is_active);
    gtk_switch_set_active(GTK_SWITCH(state->dnd_switch), is_active);
}

static void handle_add_notification_dbus(GVariant *p, OrganizerState *state) {
    if (!state || !state->main_hbox) return;
    gchar *icon, *app_name, *summary, *body;
    g_variant_get(p, "(&s&s&s&s)", &icon, &app_name, &summary, &body);
    
    GtkWidget *notification_revealer = create_notification_widget(summary, body);
    
    GtkWidget *group_wrapper = find_app_group(state->notification_list, app_name);
    GroupWidgets *g;

    if (!group_wrapper) {
        group_wrapper = create_app_group(app_name, icon);
        gtk_box_prepend(state->notification_list, group_wrapper);
        g = g_object_get_data(G_OBJECT(group_wrapper), "group-data");
        gtk_box_append(GTK_BOX(g->latest_box), notification_revealer);
    } else {
        g = g_object_get_data(G_OBJECT(group_wrapper), "group-data");
        GtkWidget *old_top = gtk_widget_get_first_child(g->latest_box);
        if (old_top) {
            g_object_ref(old_top);
            gtk_box_remove(GTK_BOX(g->latest_box), old_top);
            gtk_box_prepend(GTK_BOX(g->history_box), old_top);
            g_object_unref(old_top);
        }
        gtk_box_prepend(GTK_BOX(g->latest_box), notification_revealer);
    }
    
    update_group_header(g);
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
    const gchar* xml = "<node><interface name='com.meismeric.auranotify.Center'><method name='AddNotification'><arg type='s' name='icon' direction='in'/><arg type='s' name='app_name' direction='in'/><arg type='s' name='summary' direction='in'/><arg type='s' name='body' direction='in'/></method></interface></node>";
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, NULL);
    g_dbus_connection_register_object(c, CENTER_OBJECT_PATH, node->interfaces[0], &interface_vtable, state, NULL, NULL);
    g_dbus_node_info_unref(node);
}

static void plugin_cleanup(gpointer data) {
    OrganizerState *state = (OrganizerState *)data;
    if (!state) return;
    if (state->owner_id > 0) { g_bus_unown_name(state->owner_id); }
    if(state->dnd_signal_id > 0) { g_dbus_connection_signal_unsubscribe(state->dbus_connection, state->dnd_signal_id); }
    g_clear_object(&state->dbus_connection);
    g_free(state);
}

G_MODULE_EXPORT GtkWidget* create_widget(const char *config_string) {
    (void)config_string;
    adw_init(); 

    OrganizerState *state = g_new0(OrganizerState, 1);
    
    GtkWidget *main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_set_homogeneous(GTK_BOX(main_hbox), FALSE);
    gtk_widget_set_name(main_hbox, "organizer-widget");
    g_object_set_data_full(G_OBJECT(main_hbox), "organizer-state", state, plugin_cleanup);
    state->main_hbox = main_hbox;
    
    GtkWidget *notification_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(notification_vbox, "notification-pane");
    gtk_widget_set_hexpand(notification_vbox, TRUE);
    gtk_widget_set_margin_top(notification_vbox, 0);

    // --- Header ---
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_bottom(header_box, 8);
    gtk_widget_set_margin_start(header_box, 12);
    gtk_widget_set_margin_end(header_box, 12);

    GtkWidget *dnd_label = gtk_label_new("Do Not Disturb");
    gtk_widget_set_halign(dnd_label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(dnd_label, TRUE);
    gtk_widget_add_css_class(dnd_label, "heading");
    
    state->dnd_switch = gtk_switch_new();
    gtk_widget_set_valign(state->dnd_switch, GTK_ALIGN_CENTER);
    g_signal_connect(state->dnd_switch, "state-set", G_CALLBACK(on_dnd_toggled), state);

    GtkWidget *clear_button = gtk_button_new_with_label("Clear All");
    gtk_widget_set_halign(clear_button, GTK_ALIGN_END);
    gtk_widget_set_valign(clear_button, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(clear_button, "destructive-action"); 
    g_signal_connect(clear_button, "clicked", G_CALLBACK(on_clear_all_clicked), state);

    gtk_box_append(GTK_BOX(header_box), dnd_label);
    gtk_box_append(GTK_BOX(header_box), state->dnd_switch);
    gtk_box_append(GTK_BOX(header_box), clear_button);

    gtk_box_append(GTK_BOX(notification_vbox), header_box);

    // --- MPRIS ---
    GtkWidget *mpris = create_mpris_widget();
    gtk_box_append(GTK_BOX(notification_vbox), mpris);

    // --- List ---
    state->content_stack = gtk_stack_new();
    gtk_widget_set_vexpand(state->content_stack, TRUE);

    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    state->notification_list = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    g_object_set_data(G_OBJECT(state->notification_list), "organizer-state", state); // Fix for cleanup
    
    gtk_widget_set_valign(GTK_WIDGET(state->notification_list), GTK_ALIGN_START);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), GTK_WIDGET(state->notification_list));
    gtk_stack_add_named(GTK_STACK(state->content_stack), scrolled_window, "list");

    // --- Placeholder ---
    GtkWidget *placeholder_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_valign(placeholder_box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(placeholder_box, GTK_ALIGN_CENTER);
    GtkWidget *ph_icon = gtk_image_new_from_icon_name("notifications-disabled-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(ph_icon), 64);
    gtk_widget_set_opacity(ph_icon, 0.3);
    GtkWidget *ph_label = gtk_label_new("No Notifications");
    gtk_widget_add_css_class(ph_label, "title-2");
    gtk_widget_set_opacity(ph_label, 0.5);
    gtk_box_append(GTK_BOX(placeholder_box), ph_icon);
    gtk_box_append(GTK_BOX(placeholder_box), ph_label);
    gtk_stack_add_named(GTK_STACK(state->content_stack), placeholder_box, "placeholder");

    gtk_box_append(GTK_BOX(notification_vbox), state->content_stack);
    
    GtkWidget *calendar = calendar_widget_new();
    gtk_box_append(GTK_BOX(main_hbox), notification_vbox);
    gtk_box_append(GTK_BOX(main_hbox), calendar);
    update_placeholder_visibility(state);

    state->dbus_connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    state->owner_id = g_bus_own_name_on_connection(state->dbus_connection, CENTER_BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, on_bus_acquired, NULL, state, NULL);
    state->dnd_signal_id = g_dbus_connection_signal_subscribe(state->dbus_connection, DAEMON_BUS_NAME, DAEMON_INTERFACE_NAME, "DNDStateChanged", DAEMON_OBJECT_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_dnd_state_changed_signal, state, NULL);
    
    g_autoptr(GDBusProxy) proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL, DAEMON_BUS_NAME, DAEMON_OBJECT_PATH, DAEMON_INTERFACE_NAME, NULL, NULL);
    if(proxy) { g_dbus_proxy_call(proxy, "GetDNDState", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, (GAsyncReadyCallback)on_get_initial_dnd_state, state); }

    return main_hbox;
}