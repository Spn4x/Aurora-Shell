// In aurora-shell/widgets/mpris_player/main.c

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include "mpris.h"

// State struct remains the same, as we need width/height for our trick.
typedef struct {
    GtkWidget *view_stack;
    MprisPopoutState *mpris_state;
    GList *mpris_players;
    GDBusConnection *dbus_connection;
    guint name_watcher_id;
    gint width;
    gint height;
} MprisPluginState;

// All helper functions (update_view, etc.) are correct and remain unchanged.
static void update_view(MprisPluginState *state);
static GtkWidget* create_default_view();
static void plugin_cleanup(gpointer data);
static void on_mpris_name_appeared(const gchar *name, gpointer user_data) {
    MprisPluginState *state = user_data;
    if (g_list_find_custom(state->mpris_players, name, (GCompareFunc)g_strcmp0) == NULL) {
        state->mpris_players = g_list_append(state->mpris_players, g_strdup(name));
        update_view(state);
    }
}
static void on_mpris_name_vanished(const gchar *name, gpointer user_data) {
    MprisPluginState *state = user_data;
    GList *link = g_list_find_custom(state->mpris_players, name, (GCompareFunc)g_strcmp0);
    if (link) { g_free(link->data); state->mpris_players = g_list_delete_link(state->mpris_players, link); update_view(state); }
}
static void on_name_owner_changed(GDBusConnection *c, const gchar *s, const gchar *o, const gchar *i, const gchar *sig, GVariant *p, gpointer d) {
    (void)c; (void)s; (void)o; (void)i; (void)sig;
    const gchar *name, *old_owner, *new_owner;
    g_variant_get(p, "(sss)", &name, &old_owner, &new_owner);
    if (name && g_str_has_prefix(name, "org.mpris.MediaPlayer2.")) {
        if (new_owner && *new_owner) { on_mpris_name_appeared(name, d); }
        else { on_mpris_name_vanished(name, d); }
    }
}
static void update_view(MprisPluginState *state) {
    gboolean has_players = (state->mpris_players != NULL);
    if (has_players && state->mpris_state == NULL) {
        const gchar *bus_name = state->mpris_players->data;
        GtkWidget *old_view = gtk_stack_get_child_by_name(GTK_STACK(state->view_stack), "player-view");
        if (old_view) { gtk_stack_remove(GTK_STACK(state->view_stack), old_view); }
        GtkWidget *new_view = create_mpris_view(bus_name, &state->mpris_state, state->width, state->height);
        if (new_view) {
            state->mpris_state->window = GTK_WINDOW(gtk_widget_get_root(state->view_stack));
            gtk_stack_add_named(GTK_STACK(state->view_stack), new_view, "player-view");
            gtk_stack_set_visible_child_name(GTK_STACK(state->view_stack), "player-view");
        }
    } 
    else if (!has_players && state->mpris_state != NULL) {
        state->mpris_state = NULL;
        gtk_stack_set_visible_child_name(GTK_STACK(state->view_stack), "default-view");
        GtkWidget *old_view = gtk_stack_get_child_by_name(GTK_STACK(state->view_stack), "player-view");
        if (old_view) { gtk_stack_remove(GTK_STACK(state->view_stack), old_view); }
    }
}
static void setup_mpris_watcher(MprisPluginState *state) {
    g_autoptr(GError) error = NULL;
    state->dbus_connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (state->dbus_connection == NULL) { g_critical("MPRIS Plugin: FAILED to get D-Bus session connection. Error: %s", error ? error->message : "Unknown error"); return; }
    g_autoptr(GVariant) result = g_dbus_connection_call_sync(state->dbus_connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames", NULL, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    if (error) { g_warning("MPRIS Plugin: DBus ListNames call failed: %s", error->message); }
    else if (result) {
        g_autoptr(GVariantIter) iter;
        g_variant_get(result, "(as)", &iter);
        gchar *name;
        while (g_variant_iter_loop(iter, "s", &name)) { if (g_str_has_prefix(name, "org.mpris.MediaPlayer2.")) { on_mpris_name_appeared(name, state); } }
    }
    state->name_watcher_id = g_dbus_connection_signal_subscribe(state->dbus_connection, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_name_owner_changed, state, NULL);
}
static GtkWidget* create_default_view() {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_set_vexpand(box, TRUE);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    GtkWidget *image = gtk_image_new_from_icon_name("audio-headphones-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(image), 96);
    gtk_widget_add_css_class(image, "artist-label");
    GtkWidget *label = gtk_label_new("No active player");
    gtk_widget_add_css_class(label, "title-label");
    gtk_box_append(GTK_BOX(box), image);
    gtk_box_append(GTK_BOX(box), label);
    return box;
}
static void plugin_cleanup(gpointer data) {
    MprisPluginState *state = (MprisPluginState*)data;
    if (!state) return;
    if (state->name_watcher_id > 0) { g_dbus_connection_signal_unsubscribe(state->dbus_connection, state->name_watcher_id); }
    g_clear_object(&state->dbus_connection);
    g_list_free_full(state->mpris_players, g_free);
    g_free(state);
}

// ===================================================================
//  Plugin Entry Point (Using GtkSizeGroup to FORCE size)
// ===================================================================
G_MODULE_EXPORT GtkWidget* create_widget(const char *config_string) {
    MprisPluginState *state = g_new0(MprisPluginState, 1);
    
    state->width = 300;
    state->height = 450;

    if (config_string && *config_string) {
        g_autoptr(JsonParser) parser = json_parser_new();
        if (json_parser_load_from_data(parser, config_string, -1, NULL)) {
            JsonObject *root_obj = json_node_get_object(json_parser_get_root(parser));
            if (json_object_has_member(root_obj, "size")) {
                JsonObject *size_obj = json_object_get_object_member(root_obj, "size");
                state->width = json_object_get_int_member_with_default(size_obj, "width", state->width);
                state->height = json_object_get_int_member_with_default(size_obj, "height", state->height);
            }
        }
    }
    // This print statement is now our proof. It MUST show 700.
    g_print("MPRIS Plugin: Forcing size with GtkSizeGroup -> Width: %d, Height: %d\n", state->width, state->height);

    // 1. Create a size group that will manage both width and height.
    GtkSizeGroup *size_group = gtk_size_group_new(GTK_SIZE_GROUP_BOTH);

    // 2. Create our main UI widget, the GtkStack.
    state->view_stack = gtk_stack_new();
    gtk_widget_set_name(state->view_stack, "aurora-mpris-player");
    gtk_widget_add_css_class(state->view_stack, "mpris-player-widget");
    gtk_stack_set_transition_type(GTK_STACK(state->view_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);

    // 3. Create an invisible "dummy" widget. Its only purpose is to hold our size request.
    GtkWidget *sizing_dummy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(sizing_dummy, state->width, state->height);

    // 4. Add BOTH widgets to the size group.
    gtk_size_group_add_widget(size_group, state->view_stack);
    gtk_size_group_add_widget(size_group, sizing_dummy);

    // 5. Attach state AND the size group to the main widget for cleanup.
    g_object_set_data_full(G_OBJECT(state->view_stack), "plugin-state", state, plugin_cleanup);
    g_object_set_data_full(G_OBJECT(state->view_stack), "size-group", size_group, g_object_unref);

    // 6. Setup the rest of the widget.
    GtkWidget *default_view = create_default_view();
    gtk_stack_add_named(GTK_STACK(state->view_stack), default_view, "default-view");
    setup_mpris_watcher(state);

    // 7. Return the view_stack, which has been forcibly resized by the group.
    return state->view_stack;
}