#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include "mpris.h"

typedef struct {
    GtkWidget *view_stack;
    MprisPopoutState *mpris_state;
    GDBusProxy *manager_proxy;
    gint width;
    gint height;
} MprisPluginState;

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

static void on_media_manager_changed(GDBusProxy *proxy, GVariant *changed_properties, const gchar *const *invalidated_properties, gpointer user_data) {
    (void)changed_properties; (void)invalidated_properties;
    MprisPluginState *state = user_data;
    
    g_autoptr(GVariant) active_var = g_dbus_proxy_get_cached_property(proxy, "ActivePlayer");
    const char *active_player = active_var ? g_variant_get_string(active_var, NULL) : "";

    if (active_player && strlen(active_player) > 0) {
        GtkWidget *old_view = gtk_stack_get_child_by_name(GTK_STACK(state->view_stack), "player-view");
        if (old_view) gtk_stack_remove(GTK_STACK(state->view_stack), old_view);
        
        GtkWidget *new_view = create_mpris_view(active_player, &state->mpris_state, state->width, state->height);
        if (new_view) {
            state->mpris_state->window = GTK_WINDOW(gtk_widget_get_root(state->view_stack));
            gtk_stack_add_named(GTK_STACK(state->view_stack), new_view, "player-view");
            gtk_stack_set_visible_child_name(GTK_STACK(state->view_stack), "player-view");
        }
    } else {
        state->mpris_state = NULL;
        gtk_stack_set_visible_child_name(GTK_STACK(state->view_stack), "default-view");
        GtkWidget *old_view = gtk_stack_get_child_by_name(GTK_STACK(state->view_stack), "player-view");
        if (old_view) gtk_stack_remove(GTK_STACK(state->view_stack), old_view);
    }
}

static void on_media_manager_ready(GObject *source, GAsyncResult *res, gpointer user_data) {
    (void)source;
    MprisPluginState *state = user_data;
    GError *error = NULL;
    state->manager_proxy = g_dbus_proxy_new_for_bus_finish(res, &error);
    if (state->manager_proxy) {
        g_signal_connect(state->manager_proxy, "g-properties-changed", G_CALLBACK(on_media_manager_changed), state);
        on_media_manager_changed(state->manager_proxy, NULL, NULL, state); // Trigger initial load
    } else {
        g_warning("Failed to connect to MediaManager: %s", error->message);
        if(error) g_error_free(error);
    }
}

static void plugin_cleanup(gpointer data) {
    MprisPluginState *state = (MprisPluginState*)data;
    if (!state) return;
    if (state->manager_proxy) g_object_unref(state->manager_proxy);
    g_free(state);
}

G_MODULE_EXPORT GtkWidget* create_widget(const char *config_string) {
    MprisPluginState *state = g_new0(MprisPluginState, 1);
    state->width = 300; state->height = 450;

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
    
    state->view_stack = gtk_stack_new();
    gtk_widget_set_name(state->view_stack, "aurora-mpris-player");
    gtk_widget_add_css_class(state->view_stack, "mpris-player-widget");
    gtk_stack_set_transition_type(GTK_STACK(state->view_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_size_request(state->view_stack, state->width, state->height);

    g_object_set_data_full(G_OBJECT(state->view_stack), "plugin-state", state, plugin_cleanup);

    gtk_stack_add_named(GTK_STACK(state->view_stack), create_default_view(), "default-view");

    // Connect to our new centralized daemon
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "com.meismeric.aurora.MediaManager", "/com/meismeric/aurora/MediaManager", "com.meismeric.aurora.MediaManager",
        NULL, on_media_manager_ready, state);

    return state->view_stack;
}