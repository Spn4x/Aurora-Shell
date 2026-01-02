#include "audio_manager.h"
#include "utils.h" // For run_command
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>

// --- Data Structures for Async Tasks ---
typedef struct {
    guint volume;
    gchar *sink_name_target; // Store string name for switching
} AudioTaskData;

typedef struct {
    AudioOperationCallback user_callback;
    gpointer user_data;
} AudioFinishData;

// --- Freeing Functions ---

void audio_sink_free(gpointer data) {
    if (!data) return;
    AudioSink *sink = (AudioSink*)data;
    g_free(sink->name);
    g_free(sink->description);
    g_free(sink);
}

void free_audio_sink_list(GList *list) {
    g_list_free_full(list, audio_sink_free);
}

// Helper to deep copy an AudioSink (Prevents double-free issues in main.c)
AudioSink* audio_sink_copy(const AudioSink *src) {
    if (!src) return NULL;
    AudioSink *dest = g_new0(AudioSink, 1);
    dest->id = src->id;
    dest->name = g_strdup(src->name);
    dest->description = g_strdup(src->description);
    dest->is_default = src->is_default;
    return dest;
}

// --- Syncrhonous "Get" Functions ---

AudioSinkState* get_default_sink_state() {
    // wpctl get-volume is still reliable
    g_autofree gchar *output = run_command("wpctl get-volume @DEFAULT_SINK@");
    if (!output) return NULL;

    AudioSinkState *state = g_new0(AudioSinkState, 1);
    state->volume = 0;
    state->is_muted = FALSE;

    if (strstr(output, "[MUTED]")) {
        state->is_muted = TRUE;
    }

    const char *vol_str = strstr(output, "Volume: ");
    if (vol_str) {
        double vol_float = atof(vol_str + 8);
        state->volume = (gint)(vol_float * 100);
    }
    return state;
}

// FIXED: Parse raw 'pactl list sinks' output without using shell pipes
GList* get_audio_sinks() {
    // 1. Get the current default sink name
    g_autofree gchar *default_sink = run_command("pactl get-default-sink");
    if (default_sink) g_strstrip(default_sink);

    // 2. Run pactl WITHOUT pipes
    g_autofree gchar *output = run_command("pactl list sinks");
    if (!output) return NULL;

    GList *sinks = NULL;
    gchar **lines = g_strsplit(output, "\n", -1);

    // Temp variables to hold data as we parse the block
    gchar *pending_name = NULL;
    gchar *pending_desc = NULL;

    for (int i = 0; lines[i] != NULL; i++) {
        gchar *line = g_strstrip(lines[i]); // Remove indentation
        
        // If we hit the start of a new Sink block (or end of file), 
        // save the previous one if we found both fields.
        if (g_str_has_prefix(line, "Sink #")) {
            // (Optional cleanup if the previous block was incomplete, 
            // though pactl usually provides both)
            g_clear_pointer(&pending_name, g_free);
            g_clear_pointer(&pending_desc, g_free);
            continue;
        }

        // Parse Name
        if (g_str_has_prefix(line, "Name: ")) {
            g_free(pending_name); 
            pending_name = g_strdup(line + 6); // Skip "Name: "
        }
        // Parse Description
        else if (g_str_has_prefix(line, "Description: ")) {
            g_free(pending_desc);
            pending_desc = g_strdup(line + 13); // Skip "Description: "
        }

        // If we have both, create the sink and reset
        // (pactl lists Name before Description usually, so we trigger on Description)
        if (pending_name && pending_desc) {
            AudioSink *sink = g_new0(AudioSink, 1);
            sink->name = g_strdup(pending_name);
            sink->description = g_strdup(pending_desc);
            sink->is_default = (default_sink && g_strcmp0(pending_name, default_sink) == 0);
            sink->id = 0;

            sinks = g_list_append(sinks, sink);

            // Reset for next sink
            g_clear_pointer(&pending_name, g_free);
            g_clear_pointer(&pending_desc, g_free);
        }
    }

    g_free(pending_name);
    g_free(pending_desc);
    g_strfreev(lines);
    return sinks;
}

// --- Asynchronous "Set" Functions (using GTask) ---

static void on_async_operation_finished(GObject *s, GAsyncResult *res, gpointer d) {
    (void)s;
    AudioFinishData *finish_data = d;
    gboolean success = g_task_propagate_boolean(G_TASK(res), NULL);
    if (finish_data->user_callback) {
        finish_data->user_callback(success, finish_data->user_data);
    }
    g_free(finish_data);
}

static void free_task_data(gpointer data) {
    AudioTaskData *td = (AudioTaskData*)data;
    g_free(td->sink_name_target);
    g_free(td);
}

static void set_volume_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)d; (void)c;
    AudioTaskData *data = g_task_get_task_data(task);
    gchar *cmd = g_strdup_printf("wpctl set-volume @DEFAULT_SINK@ %u%%", data->volume);
    gint status;
    g_spawn_command_line_sync(cmd, NULL, NULL, &status, NULL);
    g_free(cmd);
    g_task_return_boolean(task, status == 0);
}

void set_default_sink_volume_async(gint volume, AudioOperationCallback cb, gpointer ud) {
    AudioTaskData *task_data = g_new0(AudioTaskData, 1);
    task_data->volume = (guint)volume;
    
    AudioFinishData *finish_data = g_new0(AudioFinishData, 1);
    finish_data->user_callback = cb;
    finish_data->user_data = ud;
    
    GTask *task = g_task_new(NULL, NULL, on_async_operation_finished, finish_data);
    g_task_set_task_data(task, task_data, free_task_data);
    g_task_run_in_thread(task, set_volume_thread_func);
    g_object_unref(task);
}

// FIXED: Uses pactl set-default-sink with string name
static void set_sink_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)d; (void)c;
    AudioTaskData *data = g_task_get_task_data(task);
    
    // Command changed to pactl
    gchar *cmd = g_strdup_printf("pactl set-default-sink '%s'", data->sink_name_target);
    
    gint status;
    g_spawn_command_line_sync(cmd, NULL, NULL, &status, NULL);
    g_free(cmd);
    g_task_return_boolean(task, status == 0);
}

void set_default_sink_async(const char *sink_name, AudioOperationCallback cb, gpointer ud) {
    AudioTaskData *task_data = g_new0(AudioTaskData, 1);
    // Copy string for thread safety
    task_data->sink_name_target = g_strdup(sink_name);
    
    AudioFinishData *finish_data = g_new0(AudioFinishData, 1);
    finish_data->user_callback = cb;
    finish_data->user_data = ud;
    
    GTask *task = g_task_new(NULL, NULL, on_async_operation_finished, finish_data);
    g_task_set_task_data(task, task_data, free_task_data);
    g_task_run_in_thread(task, set_sink_thread_func);
    g_object_unref(task);
}