#pragma once

#include <glib.h>

// Define the State struct (Volume/Mute)
typedef struct {
    gint volume;
    gboolean is_muted;
} AudioSinkState;

// Define the Sink struct
typedef struct {
    guint id;            // Legacy ID (kept for compatibility)
    gchar *name;         // PulseAudio Name (e.g. bluez_output...)
    gchar *description;  // Human Readable Name
    gboolean is_default;
} AudioSink;

// Define the Callback type
typedef void (*AudioOperationCallback)(gboolean success, gpointer user_data);

// --- Functions ---

AudioSinkState* get_default_sink_state();
GList* get_audio_sinks();

// Memory management
void free_audio_sink_list(GList *list);
void audio_sink_free(gpointer data); // Exposed so main.c can use it in signals
AudioSink* audio_sink_copy(const AudioSink *src); // Deep copy helper

// Async Operations
void set_default_sink_volume_async(gint volume, AudioOperationCallback cb, gpointer ud);

// FIXED: Takes 'const char *sink_name' instead of 'guint id'
void set_default_sink_async(const char *sink_name, AudioOperationCallback cb, gpointer ud);