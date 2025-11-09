#include "utils.h"
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <string.h>

// --- THE FIX: PART 1 ---
// Helper struct to pass multiple pieces of data to our async callback.
// This allows us to send both the new path AND the original UI state.
typedef struct {
    gchar *temp_path;
    gpointer original_user_data;
} CaptureData;
// --- END FIX ---

static void run_command_async(const gchar *command, GChildWatchFunc exit_callback, gpointer user_data) {
    g_autoptr(GError) error = NULL;
    GPid child_pid;

    g_spawn_async(
        NULL, (gchar*[]){ "sh", "-c", (gchar*)command, NULL }, NULL,
        G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
        NULL, NULL, &child_pid, &error
    );

    if (error) {
        g_warning("Failed to start async command '%s': %s", command, error->message);
        return;
    }
    
    if (exit_callback) {
        g_child_watch_add(child_pid, exit_callback, user_data);
    } else {
        g_spawn_close_pid(child_pid);
    }
}

void run_command_with_stdin_sync(const gchar *command, const gchar *input) {
    if (!command) return;

    g_autoptr(GError) error = NULL;
    g_autoptr(GSubprocess) subprocess = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDIN_PIPE,
        &error,
        "sh", "-c", command, NULL);

    if (error) {
        g_warning("Failed to create subprocess for command '%s': %s", command, error->message);
        return;
    }

    g_subprocess_communicate_utf8(subprocess, input, NULL, NULL, NULL, &error);
    
    if (error) {
        g_warning("Failed to communicate with subprocess for command '%s': %s", command, error->message);
    }
}

void process_fullscreen_screenshot(QScreenState *state) {
    g_autofree char* command = NULL;
    if (state->save_on_launch) {
        g_autofree char *pictures_dir = g_strdup(g_get_user_special_dir(G_USER_DIRECTORY_PICTURES));
        g_autoptr(GDateTime) now = g_date_time_new_now_local();
        g_autofree char *timestamp = g_date_time_format(now, "%Y-%m-%d_%H-%M-%S");
        g_autofree char *filename = g_strdup_printf("screenshot-%s.png", timestamp);
        g_autofree char *output_path = g_build_filename(pictures_dir, filename, NULL);
        command = g_strdup_printf("grim \"%s\" && wl-copy < \"%s\"", output_path, output_path);
    } else {
        command = g_strdup("grim - | wl-copy");
    }

    g_print("Running direct command (sync): %s\n", command);
    
    g_autoptr(GError) error = NULL;
    gint exit_status = 0;
    gboolean success = g_spawn_sync(NULL, (gchar*[]){ "sh", "-c", command, NULL }, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &exit_status, &error);

    if (success && exit_status == 0 && !error) {
        const char* msg = state->save_on_launch 
            ? "Screenshot saved and copied." 
            : "Image is on your clipboard.";
        g_autofree char* notify_cmd = g_strdup_printf("notify-send 'Screenshot Captured' '%s'", msg);
        run_command_async(notify_cmd, NULL, NULL);
    } else {
        g_warning("Fullscreen screenshot command failed: %s", error ? error->message : "Unknown error");
        run_command_async("notify-send -u critical 'Screenshot Failed' 'Could not capture the screen.'", NULL, NULL);
    }
}

void process_final_screenshot(const char *source_path, GdkRectangle *geometry, gboolean save_to_disk, QScreenState *state) {
    g_autofree char *output_path = NULL;

    if (save_to_disk) {
        g_autofree char *pictures_dir = g_strdup(g_get_user_special_dir(G_USER_DIRECTORY_PICTURES));
        g_autoptr(GDateTime) now = g_date_time_new_now_local();
        g_autofree char *timestamp = g_date_time_format(now, "%Y-%m-%d_%H-%M-%S");
        g_autofree char *filename = g_strdup_printf("screenshot-%s.png", timestamp);
        output_path = g_build_filename(pictures_dir, filename, NULL);
    } else {
        output_path = g_build_filename(g_get_tmp_dir(), "qscreen_final.png", NULL);
    }
    
    g_autofree char* crop_geom = g_strdup_printf("%dx%d+%d+%d", geometry->width, geometry->height, geometry->x, geometry->y);

    GString *command_str = g_string_new("");
    
    g_string_printf(command_str, "magick \"%s\" -crop %s \"%s\" && wl-copy < \"%s\"",
                    source_path, crop_geom, output_path, output_path);

    if (!save_to_disk) {
        g_string_append_printf(command_str, " && rm \"%s\"", output_path);
    }

    g_autofree char *command = g_string_free(command_str, FALSE);
    g_print("Running final command (sync): %s\n", command);

    g_autoptr(GError) error = NULL;
    gint exit_status = 0;
    gboolean success = g_spawn_sync(NULL, (gchar*[]){ "sh", "-c", command, NULL }, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &exit_status, &error);
    
    if (success && exit_status == 0 && !error) {
        const char* msg = save_to_disk 
            ? "Screenshot saved and copied." 
            : "Image is on your clipboard.";
        g_autofree char* notify_cmd = g_strdup_printf("notify-send 'Screenshot Captured' '%s'", msg);
        run_command_async(notify_cmd, NULL, NULL);
    } else {
        g_warning("Final screenshot command failed: %s", error ? error->message : "Unknown error");
        run_command_async("notify-send -u critical 'Screenshot Failed' 'Could not process the image.'", NULL, NULL);
    }

    // In the plugin version, the cleanup of the temp file is handled
    // by the UIState struct's destructor.
}

// --- THE FIX: PART 2 ---
// The corrected function now uses the CaptureData struct.
void capture_fullscreen_for_overlay(GChildWatchFunc on_captured, gpointer user_data) {
    // 1. Create a bundle to hold both the path and your UIState*
    CaptureData *capture_data = g_new(CaptureData, 1);
    capture_data->temp_path = g_build_filename(g_get_tmp_dir(), "qscreen_overlay.png", NULL);
    capture_data->original_user_data = user_data; // Keep track of the original state

    g_autofree char *grim_command = g_strdup_printf("grim \"%s\"", capture_data->temp_path);
    g_print("Capturing background: %s\n", grim_command);

    GPid pid;
    g_autoptr(GError) error = NULL;
    g_spawn_async(NULL, (gchar*[]){ "sh", "-c", grim_command, NULL }, NULL,
                  G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
                  NULL, NULL, &pid, &error);

    if (error) {
        g_warning("Failed to spawn grim: %s", error->message);
        g_free(capture_data->temp_path);
        g_free(capture_data);
        return;
    }

    // 2. Pass the BUNDLE to the callback, not just the path.
    g_child_watch_add(pid, on_captured, capture_data);
}
// --- END FIX ---


static gchar* hyprland_ipc_get_reply(const char *command) {
    g_autofree char *signature = g_strdup(g_getenv("HYPRLAND_INSTANCE_SIGNATURE"));
    if (!signature) { g_warning("HYPRLAND_INSTANCE_SIGNATURE not set."); return NULL; }
    g_autofree char *socket_path = g_build_filename(g_get_user_runtime_dir(), "hypr", signature, ".socket.sock", NULL);

    g_autoptr(GError) error = NULL;
    g_autoptr(GSocketClient) client = g_socket_client_new();
    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(socket_path);
    g_autoptr(GSocketConnection) conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, &error);
    if (error) { g_warning("Failed to connect to Hyprland socket: %s", error->message); return NULL; }

    GInputStream *istream = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), command, strlen(command), NULL, NULL, NULL);

    GByteArray *bytes = g_byte_array_new();
    char buffer[4096]; gssize read;
    while ((read = g_input_stream_read(istream, buffer, sizeof(buffer), NULL, &error)) > 0) {
        g_byte_array_append(bytes, (const guint8*)buffer, read);
    }
    if (error) { g_warning("Failed to read from Hyprland socket: %s", error->message); g_byte_array_free(bytes, TRUE); return NULL; }

    g_byte_array_append(bytes, (const guint8*)"\0", 1);
    return (gchar*)g_byte_array_free(bytes, FALSE);
}

GList* get_hyprland_windows_geometry(QScreenState *state) {
    (void)state;
    g_autofree gchar *monitors_reply = hyprland_ipc_get_reply("j/monitors");
    if (!monitors_reply) return NULL;

    g_autoptr(JsonParser) monitor_parser = json_parser_new();
    json_parser_load_from_data(monitor_parser, monitors_reply, -1, NULL);
    JsonNode *root_node = json_parser_get_root(monitor_parser);
    if (!root_node) { g_warning("Failed to parse monitors JSON."); return NULL; }
    JsonArray *monitors = json_node_get_array(root_node);
    
    gint64 monitor_x = 0, monitor_y = 0;
    gint64 active_workspace_id = -1;
    for (guint i = 0; i < json_array_get_length(monitors); i++) {
        JsonObject *mon_obj = json_array_get_object_element(monitors, i);
        if (json_object_get_boolean_member(mon_obj, "focused")) {
            monitor_x = json_object_get_int_member(mon_obj, "x");
            monitor_y = json_object_get_int_member(mon_obj, "y");
            active_workspace_id = json_object_get_int_member(json_object_get_object_member(mon_obj, "activeWorkspace"), "id");
            break;
        }
    }
    
    g_autofree gchar *clients_reply = hyprland_ipc_get_reply("j/clients");
    if (!clients_reply) return NULL;

    g_autoptr(JsonParser) client_parser = json_parser_new();
    json_parser_load_from_data(client_parser, clients_reply, -1, NULL);
    root_node = json_parser_get_root(client_parser);
    if (!root_node) { g_warning("Failed to parse clients JSON."); return NULL; }
    JsonArray *clients = json_node_get_array(root_node);
    
    GList *result = NULL;
    for (guint i = 0; i < json_array_get_length(clients); i++) {
        JsonObject *obj = json_array_get_object_element(clients, i);
        if (json_object_has_member(obj, "workspace")) {
            JsonObject *workspace_obj = json_object_get_object_member(obj, "workspace");
            if (json_object_get_int_member(workspace_obj, "id") == active_workspace_id) {
                JsonArray *at = json_object_get_array_member(obj, "at");
                JsonArray *size = json_object_get_array_member(obj, "size");
                
                GdkRectangle *rect = g_new(GdkRectangle, 1);
                rect->x = json_array_get_int_element(at, 0) - monitor_x;
                rect->y = json_array_get_int_element(at, 1) - monitor_y;
                rect->width = json_array_get_int_element(size, 0);
                rect->height = json_array_get_int_element(size, 1);
                result = g_list_prepend(result, rect);
            }
        }
    }
    return g_list_reverse(result);
}

gboolean check_dependencies(void) {
    // This function is not used in the plugin version, as dependencies
    // are assumed to be handled by the user's system setup.
    // It can be removed or left here for completeness.
    return TRUE;
}