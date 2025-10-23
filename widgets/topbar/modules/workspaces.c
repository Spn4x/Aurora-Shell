#include <gtk/gtk.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include "workspaces.h"

typedef struct {
    GtkWidget *box;
    GHashTable *workspace_buttons;
    gint current_active_id;
    GSocketConnection *event_connection;
    GDataInputStream *event_stream;
    GCancellable *cancellable; // THE FIX: Add a cancellable object.
} WorkspacesModule;

// --- Forward Declarations ---
static void add_workspace_button(WorkspacesModule *module, gint id);
static void remove_workspace_button(WorkspacesModule *module, gint id);
static void on_socket_connected(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_hyprland_event(GObject *source, GAsyncResult *res, gpointer user_data);
static void workspaces_module_cleanup(gpointer data);

static void update_active_workspace_ui(WorkspacesModule *module, gint new_active_id) {
    if (module->current_active_id == new_active_id) return;
    GtkWidget *old_button = g_hash_table_lookup(module->workspace_buttons, GINT_TO_POINTER(module->current_active_id));
    if (old_button) gtk_widget_remove_css_class(old_button, "active-ws");
    GtkWidget *new_button = g_hash_table_lookup(module->workspace_buttons, GINT_TO_POINTER(new_active_id));
    if (new_button) gtk_widget_add_css_class(new_button, "active-ws");
    module->current_active_id = new_active_id;
}

static void on_workspace_button_clicked(GtkButton *button, gpointer data) {
    (void)button;
    gint id = GPOINTER_TO_INT(data);
    g_autofree gchar *command = g_strdup_printf("hyprctl dispatch workspace %d", id);
    system(command);
}

static void add_workspace_button(WorkspacesModule *module, gint id) {
    if (g_hash_table_lookup(module->workspace_buttons, GINT_TO_POINTER(id))) return;

    g_autofree gchar *label = g_strdup_printf("%d", id);
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(button, "workspace-button");
    gtk_widget_add_css_class(button, "flat");
    
    g_object_set_data(G_OBJECT(button), "ws-id", GINT_TO_POINTER(id));
    g_signal_connect(button, "clicked", G_CALLBACK(on_workspace_button_clicked), GINT_TO_POINTER(id));
    
    gtk_box_append(GTK_BOX(module->box), button);
    g_hash_table_insert(module->workspace_buttons, GINT_TO_POINTER(id), button);

    GtkWidget *place_after = NULL;
    gint max_lower_id = -1;
    for (GtkWidget *child = gtk_widget_get_first_child(module->box); child != NULL; child = gtk_widget_get_next_sibling(child)) {
        if (child == button) continue;
        gint child_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "ws-id"));
        if (child_id < id && child_id > max_lower_id) {
            max_lower_id = child_id;
            place_after = child;
        }
    }
    gtk_box_reorder_child_after(GTK_BOX(module->box), button, place_after);
}

static void remove_workspace_button(WorkspacesModule *module, gint id) {
    GtkWidget *button = g_hash_table_lookup(module->workspace_buttons, GINT_TO_POINTER(id));
    if (button) {
        g_hash_table_remove(module->workspace_buttons, GINT_TO_POINTER(id));
        gtk_widget_unparent(button);
    }
}

static void on_hyprland_event(GObject *source, GAsyncResult *res, gpointer user_data) {
    WorkspacesModule *module = (WorkspacesModule *)user_data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *line = g_data_input_stream_read_line_finish(module->event_stream, res, NULL, &error);
    
    // THE FIX: Handle cancellation and errors gracefully.
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_info("Hyprland event listener cancelled. Exiting cleanly.");
        return; // Stop processing
    }
    if (error || !line) {
        g_warning("Error reading from Hyprland event socket: %s", error ? error->message : "No line received");
        return; // Stop trying to listen
    }
    
    if (g_str_has_prefix(line, "workspacev2>>")) {
        gchar **parts = g_strsplit(line, ">>", 2);
        if (parts[1]) {
            gchar **data_parts = g_strsplit(parts[1], ",", 2);
            if (data_parts[0]) update_active_workspace_ui(module, atoi(data_parts[0]));
            g_strfreev(data_parts);
        }
        g_strfreev(parts);
    } 
    else if (g_str_has_prefix(line, "createworkspacev2>>")) {
        gchar **parts = g_strsplit(line, ">>", 2);
        if (parts[1]) {
            gchar **data_parts = g_strsplit(parts[1], ",", 2);
            if (data_parts[0]) add_workspace_button(module, atoi(data_parts[0]));
            g_strfreev(data_parts);
        }
        g_strfreev(parts);
    }
    else if (g_str_has_prefix(line, "destroyworkspacev2>>")) {
        gchar **parts = g_strsplit(line, ">>", 2);
        if (parts[1]) {
            gchar **data_parts = g_strsplit(parts[1], ",", 2);
            if (data_parts[0]) remove_workspace_button(module, atoi(data_parts[0]));
            g_strfreev(data_parts);
        }
        g_strfreev(parts);
    }
    
    // THE FIX: Pass the cancellable to the next async read.
    g_data_input_stream_read_line_async(module->event_stream, G_PRIORITY_DEFAULT, module->cancellable, on_hyprland_event, module);
}

static void on_socket_connected(GObject *source, GAsyncResult *res, gpointer user_data) {
    WorkspacesModule *module = (WorkspacesModule *)user_data;
    g_autoptr(GError) error = NULL;
    module->event_connection = g_socket_client_connect_finish(G_SOCKET_CLIENT(source), res, &error);
    if (error) { g_warning("Failed to connect to event socket: %s", error->message); return; }
    g_print("Successfully connected to Hyprland event socket.\n");
    g_autoptr(GInputStream) istream = g_io_stream_get_input_stream(G_IO_STREAM(module->event_connection));
    module->event_stream = g_data_input_stream_new(istream);
    
    // THE FIX: Pass the cancellable to the first async read.
    g_data_input_stream_read_line_async(module->event_stream, G_PRIORITY_DEFAULT, module->cancellable, on_hyprland_event, module);
}

static void connect_to_event_socket(WorkspacesModule *module) {
    const gchar *instance_signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const gchar *xdg_runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!instance_signature || !xdg_runtime_dir) { g_warning("Hyprland environment variables not set."); return; }
    g_autofree gchar *socket_path = g_build_filename(xdg_runtime_dir, "hypr", instance_signature, ".socket2.sock", NULL);
    g_autoptr(GSocketClient) client = g_socket_client_new();
    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(socket_path);
    // THE FIX: Pass the cancellable when connecting.
    g_socket_client_connect_async(client, G_SOCKET_CONNECTABLE(address), module->cancellable, on_socket_connected, module);
}

static gchar* run_command_and_get_output(const char* command) {
    FILE *fp = popen(command, "r");
    if (!fp) return NULL;
    gchar buffer[256]; GString *output_str = g_string_new("");
    while (fgets(buffer, sizeof(buffer), fp) != NULL) g_string_append(output_str, buffer);
    pclose(fp);
    return g_string_free(output_str, FALSE);
}

static void populate_initial_workspaces(WorkspacesModule *module) {
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *workspaces_json = run_command_and_get_output("hyprctl -j workspaces");
    if (!workspaces_json || !json_parser_load_from_data(parser, workspaces_json, -1, NULL)) return;
    JsonArray *workspaces_array = json_node_get_array(json_parser_get_root(parser));
    for (guint i = 0; i < json_array_get_length(workspaces_array); i++) {
        JsonObject *ws = json_array_get_object_element(workspaces_array, i);
        gint id = json_object_get_int_member(ws, "id");
        if (id > 0) add_workspace_button(module, id);
    }
    g_autofree gchar *active_json = run_command_and_get_output("hyprctl -j activeworkspace");
    if (!active_json || !json_parser_load_from_data(parser, active_json, -1, NULL)) return;
    JsonObject *active_ws = json_node_get_object(json_parser_get_root(parser));
    gint active_id = json_object_get_int_member(active_ws, "id");
    if (active_id != -1) update_active_workspace_ui(module, active_id);
}

static void workspaces_module_cleanup(gpointer data) {
    WorkspacesModule *module = (WorkspacesModule *)data;
    
    // THE FIX: Cancel the background tasks BEFORE freeing anything.
    if (module->cancellable) {
        g_cancellable_cancel(module->cancellable);
    }

    g_hash_table_destroy(module->workspace_buttons);
    if (module->event_stream) g_object_unref(module->event_stream);
    if (module->event_connection) g_object_unref(module->event_connection);
    
    // THE FIX: Unref the cancellable itself.
    if (module->cancellable) {
        g_object_unref(module->cancellable);
    }
    
    g_free(module);
}

GtkWidget* create_workspaces_module() {
    WorkspacesModule *module = g_new0(WorkspacesModule, 1);
    // THE FIX: Create the cancellable object.
    module->cancellable = g_cancellable_new();
    
    module->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(module->box, "workspace-module");
    gtk_widget_add_css_class(module->box, "module");
    module->workspace_buttons = g_hash_table_new(NULL, NULL);
    g_object_set_data_full(G_OBJECT(module->box), "module-state", module, workspaces_module_cleanup);
    populate_initial_workspaces(module);
    connect_to_event_socket(module);
    return module->box;
}