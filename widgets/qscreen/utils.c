#include "utils.h"
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <string.h>

typedef struct {
    gchar *temp_path;
    gpointer original_user_data;
} CaptureData;

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
        g_warning("Failed to create subprocess: %s", error->message);
        return;
    }
    g_subprocess_communicate_utf8(subprocess, input, NULL, NULL, NULL, &error);
}

void process_precomposited_screenshot(const char *source_path, gboolean save_to_disk, QScreenState *state) {
    (void)state;
    g_autofree char *output_path = NULL;
    g_autofree char *command = NULL;

    if (save_to_disk) {
        g_autofree char *pictures_dir = g_strdup(g_get_user_special_dir(G_USER_DIRECTORY_PICTURES));
        g_autoptr(GDateTime) now = g_date_time_new_now_local();
        g_autofree char *timestamp = g_date_time_format(now, "%Y-%m-%d_%H-%M-%S");
        g_autofree char *filename = g_strdup_printf("screenshot-%s.png", timestamp);
        output_path = g_build_filename(pictures_dir, filename, NULL);
        
        command = g_strdup_printf("cp \"%s\" \"%s\" && wl-copy -t image/png < \"%s\"", source_path, output_path, output_path);
    } else {
        command = g_strdup_printf("wl-copy -t image/png < \"%s\"", source_path);
    }

    g_autoptr(GError) error = NULL;
    gint exit_status = 0;
    gboolean success = g_spawn_sync(NULL, (gchar*[]){ "sh", "-c", command, NULL }, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &exit_status, &error);
    
    if (success && exit_status == 0 && !error) {
        const char* msg = save_to_disk ? "Annotated screenshot saved and copied." : "Annotated image is on your clipboard.";
        g_autofree char* notify_cmd = g_strdup_printf("notify-send 'Screenshot Captured' '%s'", msg);
        g_spawn_command_line_async(notify_cmd, NULL);
    } else {
        g_spawn_command_line_async("notify-send -u critical 'Screenshot Failed' 'Could not process the image.'", NULL);
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
        command = g_strdup_printf("grim \"%s\" && wl-copy -t image/png < \"%s\"", output_path, output_path);
    } else {
        command = g_strdup("grim - | wl-copy -t image/png");
    }

    g_autoptr(GError) error = NULL;
    gint exit_status = 0;
    gboolean success = g_spawn_sync(NULL, (gchar*[]){ "sh", "-c", command, NULL }, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &exit_status, &error);

    if (success && exit_status == 0 && !error) {
        const char* msg = state->save_on_launch ? "Screenshot saved and copied." : "Image is on your clipboard.";
        g_autofree char* notify_cmd = g_strdup_printf("notify-send 'Screenshot Captured' '%s'", msg);
        run_command_async(notify_cmd, NULL, NULL);
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
    g_string_printf(command_str, "magick \"%s\" -crop %s \"%s\" && wl-copy -t image/png < \"%s\"", source_path, crop_geom, output_path, output_path);

    if (!save_to_disk) g_string_append_printf(command_str, " && rm \"%s\"", output_path);

    g_autofree char *command = g_string_free(command_str, FALSE);
    g_autoptr(GError) error = NULL;
    gint exit_status = 0;
    gboolean success = g_spawn_sync(NULL, (gchar*[]){ "sh", "-c", command, NULL }, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &exit_status, &error);
    
    if (success && exit_status == 0 && !error) {
        const char* msg = save_to_disk ? "Screenshot saved and copied." : "Image is on your clipboard.";
        g_autofree char* notify_cmd = g_strdup_printf("notify-send 'Screenshot Captured' '%s'", msg);
        run_command_async(notify_cmd, NULL, NULL);
    }
}

void capture_fullscreen_for_overlay(GChildWatchFunc on_captured, gpointer user_data) {
    CaptureData *capture_data = g_new(CaptureData, 1);
    capture_data->temp_path = g_build_filename(g_get_tmp_dir(), "qscreen_overlay.png", NULL);
    capture_data->original_user_data = user_data;

    g_autofree char *grim_command = g_strdup_printf("grim \"%s\"", capture_data->temp_path);

    GPid pid;
    g_autoptr(GError) error = NULL;
    g_spawn_async(NULL, (gchar*[]){ "sh", "-c", grim_command, NULL }, NULL,
                  G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
                  NULL, NULL, &pid, &error);

    if (error) {
        g_free(capture_data->temp_path);
        g_free(capture_data);
        return;
    }
    g_child_watch_add(pid, on_captured, capture_data);
}

// --- NIRI WINDOW DETAILS ACQUISITION ---

void niri_window_info_free(gpointer data) {
    if (!data) return;
    NiriWindowInfo *info = (NiriWindowInfo*)data;
    g_free(info->title);
    g_free(info->app_id);
    g_free(info);
}

gchar* run_niri_ipc(const char *cmd) {
    g_autofree gchar *command = g_strdup_printf("niri msg -j %s", cmd);
    FILE *fp = popen(command, "r");
    if (!fp) return NULL;
    char buffer[4096];
    GString *out = g_string_new("");
    while (fgets(buffer, sizeof(buffer), fp)) g_string_append(out, buffer);
    pclose(fp);
    return g_string_free(out, FALSE);
}

GList* get_niri_windows_info(QScreenState *state) {
    (void)state;
    
    // 1. Get active workspace ID
    g_autofree gchar *ws_reply = run_niri_ipc("workspaces");
    if (!ws_reply) return NULL;

    guint64 active_ws_id = 0;

    g_autoptr(JsonParser) ws_parser = json_parser_new();
    if (json_parser_load_from_data(ws_parser, ws_reply, -1, NULL)) {
        JsonNode *root = json_parser_get_root(ws_parser);
        if (JSON_NODE_HOLDS_ARRAY(root)) {
            JsonArray *arr = json_node_get_array(root);
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *ws = json_array_get_object_element(arr, i);
                gboolean is_active = json_object_has_member(ws, "is_active") && json_object_get_boolean_member(ws, "is_active");
                gboolean is_focused = json_object_has_member(ws, "is_focused") && json_object_get_boolean_member(ws, "is_focused");
                if (is_active || is_focused) {
                    active_ws_id = json_object_get_int_member(ws, "id");
                    break;
                }
            }
        }
    }

    if (active_ws_id == 0) return NULL;

    // 2. Get windows safely (Width and Height only)
    g_autofree gchar *win_reply = run_niri_ipc("windows");
    if (!win_reply) return NULL;

    GList *result = NULL;

    g_autoptr(JsonParser) win_parser = json_parser_new();
    if (json_parser_load_from_data(win_parser, win_reply, -1, NULL)) {
        JsonNode *root = json_parser_get_root(win_parser);
        if (JSON_NODE_HOLDS_ARRAY(root)) {
            JsonArray *arr = json_node_get_array(root);
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *win = json_array_get_object_element(arr, i);
                
                guint64 ws_id = json_object_get_int_member(win, "workspace_id");
                gboolean is_focused = json_object_has_member(win, "is_focused") && json_object_get_boolean_member(win, "is_focused");

                if (ws_id == active_ws_id) {
                    JsonObject *layout = json_object_get_object_member(win, "layout");
                    if (layout && json_object_has_member(layout, "window_size")) {
                        JsonArray *size_arr = json_object_get_array_member(layout, "window_size");
                        
                        NiriWindowInfo *info = g_new0(NiriWindowInfo, 1);
                        info->id = json_object_get_int_member(win, "id");
                        info->title = g_strdup(json_object_get_string_member_with_default(win, "title", "Unknown"));
                        info->app_id = g_strdup(json_object_get_string_member_with_default(win, "app_id", "application-x-executable"));
                        
                        info->geometry.width = json_array_get_int_element(size_arr, 0);
                        info->geometry.height = json_array_get_int_element(size_arr, 1);
                        info->geometry.x = 0;
                        info->geometry.y = 0;
                        info->is_visible = is_focused;

                        result = g_list_prepend(result, info);
                    }
                }
            }
        }
    }
    return g_list_reverse(result);
}

gboolean check_dependencies(void) { return TRUE; }