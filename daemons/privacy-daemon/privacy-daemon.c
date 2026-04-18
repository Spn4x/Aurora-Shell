#define _GNU_SOURCE

#include <pipewire/pipewire.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <glib.h>
#include <json-glib/json-glib.h>

const char* UI_BUS_NAME = "com.meismeric.auranotify.UI";
const char* UI_OBJECT_PATH = "/com/meismeric/auranotify/UI";
const char* UI_INTERFACE_NAME = "com.meismeric.auranotify.UI";

typedef struct {
    uint32_t id;
    uint32_t pid;
    char *name;
    int type; // 0 = mic, 1 = cam
} PwStream;

static GList *active_streams = NULL;
static GDBusConnection *session_bus = NULL;

static gboolean update_status_on_main_thread(gpointer user_data);

static void on_pw_global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version, const struct spa_dict *props) {
    (void)data; (void)permissions; (void)version;
    if (props && strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char *media_class = spa_dict_lookup(props, "media.class");
        if (!media_class) return;

        int stream_type = -1;
        if (strcmp(media_class, "Stream/Input/Audio") == 0) stream_type = 0;
        else if (strcmp(media_class, "Stream/Input/Video") == 0) stream_type = 1;

        if (stream_type != -1) {
            // Check if already tracking
            for (GList *l = active_streams; l != NULL; l = l->next) {
                if (((PwStream*)l->data)->id == id) return;
            }

            const char *app_name = spa_dict_lookup(props, "application.name");
            if (!app_name) app_name = spa_dict_lookup(props, "node.name");
            
            const char *pid_str = spa_dict_lookup(props, "application.process.id");

            PwStream *stream = g_new0(PwStream, 1);
            stream->id = id;
            stream->type = stream_type;
            stream->name = g_strdup(app_name ? app_name : "Unknown Process");
            stream->pid = pid_str ? atoi(pid_str) : 0;

            active_streams = g_list_append(active_streams, stream);
            g_idle_add(update_status_on_main_thread, NULL);
        }
    }
}

static void on_pw_global_remove(void *data, uint32_t id) {
    (void)data;
    for (GList *l = active_streams; l != NULL; l = l->next) {
        PwStream *s = (PwStream*)l->data;
        if (s->id == id) {
            g_free(s->name);
            g_free(s);
            active_streams = g_list_delete_link(active_streams, l);
            g_idle_add(update_status_on_main_thread, NULL);
            break;
        }
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
    struct pw_context *context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 0);
    struct pw_core *core = pw_context_connect(context, NULL, 0);
    struct pw_registry *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

    if (registry) {
        struct spa_hook registry_listener;
        pw_registry_add_listener(registry, &registry_listener, &registry_events, NULL);
        pw_main_loop_run(loop);
    }

    if (core) pw_core_disconnect(core);
    if (context) pw_context_destroy(context);
    pw_main_loop_destroy(loop);
    return NULL;
}

static gboolean update_status_on_main_thread(gpointer user_data) {
    (void)user_data;
    if (!session_bus) return G_SOURCE_REMOVE;

    g_autoptr(JsonBuilder) builder = json_builder_new();
    json_builder_begin_array(builder);

    for (GList *l = active_streams; l != NULL; l = l->next) {
        PwStream *s = (PwStream*)l->data;
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "pid");
        json_builder_add_int_value(builder, s->pid);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, s->name);
        json_builder_set_member_name(builder, "type");
        json_builder_add_int_value(builder, s->type);
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder);

    g_autoptr(JsonGenerator) gen = json_generator_new();
    g_autoptr(JsonNode) root = json_builder_get_root(builder);
    json_generator_set_root(gen, root);
    
    g_autofree gchar *json_str = json_generator_to_data(gen, NULL);

    g_dbus_connection_call(session_bus, UI_BUS_NAME, UI_OBJECT_PATH, UI_INTERFACE_NAME,
                           "SetPrivacyStatus",
                           g_variant_new("(s)", json_str),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);

    return G_SOURCE_REMOVE;
}

int main() {
    session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!session_bus) return 1;

    g_thread_new("pipewire-monitor", (GThreadFunc)run_pipewire_thread, NULL);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    
    g_object_unref(session_bus);
    g_main_loop_unref(loop);
    return 0;
}