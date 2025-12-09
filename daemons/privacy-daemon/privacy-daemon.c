#define _GNU_SOURCE

#include <pipewire/pipewire.h>
#include <stdint.h>
#include <string.h>
#include <gio/gio.h>
#include <glib.h>

const char* UI_BUS_NAME = "com.meismeric.auranotify.UI";
const char* UI_OBJECT_PATH = "/com/meismeric/auranotify/UI";
const char* UI_INTERFACE_NAME = "com.meismeric.auranotify.UI";

static GList *active_mic_streams = NULL;
static GList *active_screen_streams = NULL;
static GDBusConnection *session_bus = NULL;

static gboolean update_status_on_main_thread(gpointer user_data);
static gpointer run_pipewire_thread(gpointer user_data);

static void on_pw_global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version, const struct spa_dict *props) {
    (void)data; (void)permissions; (void)version;
    if (props && strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char *media_class = spa_dict_lookup(props, "media.class");
        if (!media_class) return;

        gboolean changed = FALSE;
        if (strcmp(media_class, "Stream/Input/Audio") == 0) {
            if (g_list_find(active_mic_streams, GUINT_TO_POINTER(id)) == NULL) {
                active_mic_streams = g_list_append(active_mic_streams, GUINT_TO_POINTER(id));
                changed = TRUE;
            }
        } else if (strcmp(media_class, "Stream/Input/Video") == 0) {
            if (g_list_find(active_screen_streams, GUINT_TO_POINTER(id)) == NULL) {
                active_screen_streams = g_list_append(active_screen_streams, GUINT_TO_POINTER(id));
                changed = TRUE;
            }
        }
        if (changed) {
            g_idle_add(update_status_on_main_thread, NULL);
        }
    }
}

static void on_pw_global_remove(void *data, uint32_t id) {
    (void)data;
    gboolean changed = FALSE;
    GList *mic_item = g_list_find(active_mic_streams, GUINT_TO_POINTER(id));
    if (mic_item) {
        active_mic_streams = g_list_remove_link(active_mic_streams, mic_item);
        g_list_free_1(mic_item);
        changed = TRUE;
    } else {
        GList *screen_item = g_list_find(active_screen_streams, GUINT_TO_POINTER(id));
        if (screen_item) {
            active_screen_streams = g_list_remove_link(active_screen_streams, screen_item);
            g_list_free_1(screen_item);
            changed = TRUE;
        }
    }
    if (changed) {
        g_idle_add(update_status_on_main_thread, NULL);
    }
}

static const struct pw_registry_events registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = on_pw_global,
    .global_remove = on_pw_global_remove,
};

static gpointer run_pipewire_thread(void *data) {
    (void)data;
    pw_init(NULL, NULL);
    struct pw_main_loop *loop = pw_main_loop_new(NULL);
    struct pw_loop *pw_l = pw_main_loop_get_loop(loop);
    struct pw_context *context = pw_context_new(pw_l, NULL, 0);
    struct pw_core *core = pw_context_connect(context, NULL, 0);
    struct pw_registry *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

    if (registry == NULL) return NULL;

    struct spa_hook registry_listener;
    pw_registry_add_listener(registry, &registry_listener, &registry_events, NULL);
    pw_main_loop_run(loop);

    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);
    return NULL;
}

static gboolean update_status_on_main_thread(gpointer user_data) {
    (void)user_data;
    if (!session_bus) return G_SOURCE_REMOVE;

    gboolean mic_active = (g_list_length(active_mic_streams) > 0);
    gboolean screen_active = (g_list_length(active_screen_streams) > 0);

    g_print("Privacy Daemon: Updating status -> Mic: %s, Screen: %s\n", mic_active ? "ON" : "OFF", screen_active ? "ON" : "OFF");

    g_dbus_connection_call(session_bus, UI_BUS_NAME, UI_OBJECT_PATH, UI_INTERFACE_NAME,
                           "SetPersistentStatus",
                           g_variant_new("(sbs)", "mic_status", mic_active, "● Microphone in use"),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);

    g_dbus_connection_call(session_bus, UI_BUS_NAME, UI_OBJECT_PATH, UI_INTERFACE_NAME,
                           "SetPersistentStatus",
                           g_variant_new("(sbs)", "screen_status", screen_active, "● Screen is being shared"),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);

    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!session_bus) {
        g_printerr("Failed to connect to D-Bus session bus.\n");
        return 1;
    }

    g_thread_new("pipewire-monitor", (GThreadFunc)run_pipewire_thread, NULL);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    g_object_unref(session_bus);
    g_main_loop_unref(loop);
    g_list_free(active_mic_streams);
    g_list_free(active_screen_streams);

    return 0;
}