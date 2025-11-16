#include "network_manager.h"
#include <gio/gio.h>
#include <glib-object.h>
#include <string.h>

// D-Bus constants for NetworkManager
#define NM_DBUS_SERVICE "org.freedesktop.NetworkManager"
#define NM_DBUS_PATH "/org/freedesktop/NetworkManager"
#define NM_DBUS_INTERFACE "org.freedesktop.NetworkManager"
#define NM_DEVICE_INTERFACE "org.freedesktop.NetworkManager.Device"
#define NM_WIRELESS_DEVICE_INTERFACE "org.freedesktop.NetworkManager.Device.Wireless"
#define NM_AP_INTERFACE "org.freedesktop.NetworkManager.AccessPoint"
#define NM_SETTINGS_PATH "/org/freedesktop/NetworkManager/Settings"
#define NM_SETTINGS_INTERFACE "org.freedesktop.NetworkManager.Settings"
#define NM_SETTINGS_CONNECTION_INTERFACE "org.freedesktop.NetworkManager.Settings.Connection"
#define NM_DEVICE_TYPE_WIFI 2
#define NM_STATE_ASLEEP 10
#define NM_CONNECTIVITY_UNKNOWN 0
#define NM_CONNECTIVITY_NONE 1
#define NM_CONNECTIVITY_PORTAL 2
#define NM_CONNECTIVITY_LIMITED 3
#define NM_CONNECTIVITY_FULL 4

/* --- Internal Structures and Context --- */

typedef struct {
    GDBusProxy *nm_proxy;
    GDBusProxy *settings_proxy;
} NetworkManagerContext;

typedef struct {
    NetworkOperationCallback user_callback;
    gpointer user_data;
} OperationFinishData;

typedef struct {
    WifiDetailsCallback user_callback;
    gpointer user_data;
} GetDetailsFinishData;

typedef struct {
    gchar *ssid;
    gchar *ap_path;
    gchar *password;
    gboolean is_secure;
} AddConnectionTaskData;

typedef struct {
    gchar *connection_path;
    gchar *ap_path;
} ActivateConnectionTaskData;

typedef struct {
    gchar *ssid;
} ForgetTaskData;

typedef struct {
    gboolean enabled;
} SetEnabledTaskData;

typedef struct {
    gchar *ap_path;
} GetDetailsTaskData;

typedef struct {
    GMainLoop *loop;
    gboolean success;
    gchar *active_conn_path; // We need to know which object to watch for
    guint sub_properties;
    guint sub_object_removed;
    guint sub_timeout;
} ActivationState;

static NetworkManagerContext *g_context = NULL;

/* --- Forward Declarations of Static Functions --- */

static void on_operation_finished(GObject *source_object, GAsyncResult *res, gpointer user_data);
static void on_get_details_finished(GObject *source_object, GAsyncResult *res, gpointer user_data);
static void forget_task_thread_func(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
static void disconnect_task_thread_func(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
static void add_and_activate_connection_thread_func(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
static void activate_connection_thread_func(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
static void set_enabled_task_thread_func(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
static void get_details_thread_func(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);

/* --- Memory Management for Task Data --- */

static void add_connection_task_data_free(gpointer data) {
    AddConnectionTaskData *d = data;
    if (!d) return;
    g_free(d->ssid);
    g_free(d->ap_path);
    g_free(d->password);
    g_free(d);
}

static void activate_connection_task_data_free(gpointer data) {
    ActivateConnectionTaskData *d = data;
    if (!d) return;
    g_free(d->connection_path);
    g_free(d->ap_path);
    g_free(d);
}

static void forget_task_data_free(gpointer data) {
    ForgetTaskData *d = data;
    if (!d) return;
    g_free(d->ssid);
    g_free(d);
}

/* --- Helper Functions --- */
static void on_active_connection_state_changed(GDBusConnection *connection,
                                               const gchar *sender_name,
                                               const gchar *object_path,
                                               const gchar *interface_name,
                                               const gchar *signal_name,
                                               GVariant *parameters,
                                               gpointer user_data)
{
    (void)connection; (void)sender_name; (void)object_path; (void)interface_name; (void)signal_name;
    ActivationState *state = user_data;
    
    // =========================================================================
    // THIS IS THE DEFINITIVE FIX.
    // The signal's parameter is a (s a{sv} as) tuple. We must unpack it safely
    // by getting the child items individually, not with a broken format string.
    // =========================================================================
    g_autoptr(GVariant) properties_changed = g_variant_get_child_value(parameters, 1);
    
    guint32 new_state;
    if (g_variant_lookup(properties_changed, "State", "u", &new_state)) {
        g_print("[NM DEBUG] ---> Activation signal received. New state: %u\n", new_state);
        // NM_ACTIVE_CONNECTION_STATE_ACTIVATED = 2
        // NM_ACTIVE_CONNECTION_STATE_DEACTIVATED = 4
        if (new_state == 2) { // ACTIVATED
            g_print("[NM DEBUG] ---> SUCCESS: Connection is fully activated.\n");
            state->success = TRUE;
            g_main_loop_quit(state->loop);
        } else if (new_state == 4) { // DEACTIVATED (This means failure)
            g_print("[NM DEBUG] ---> FAILURE: Connection deactivated (wrong password).\n");
            state->success = FALSE;
            g_main_loop_quit(state->loop);
        }
    }
}

// --- REPLACEMENT for on_object_removed ---
static void on_object_removed(GDBusConnection *connection,
                              const gchar *sender_name,
                              const gchar *object_path,
                              const gchar *interface_name,
                              const gchar *signal_name,
                              GVariant *parameters,
                              gpointer user_data)
{
    (void)connection; (void)sender_name; (void)interface_name; (void)signal_name;
    ActivationState *state = user_data;
    
    // Safely unpack the (o as) tuple
    g_autoptr(GVariant) child = g_variant_get_child_value(parameters, 0);
    g_autofree gchar *removed_path = g_variant_dup_string(child, NULL);

    // Check if the object that was removed is the one we are watching
    if (removed_path && g_strcmp0(removed_path, state->active_conn_path) == 0) {
        g_print("[NM DEBUG] ---> FAILURE: ActiveConnection object was removed (user cancelled prompt).\n");
        state->success = FALSE;
        g_main_loop_quit(state->loop);
    }
}


// --- Timeout Handler ---
static gboolean on_activation_timeout(gpointer user_data) {
    ActivationState *state = user_data;
    g_warning("[NM DEBUG] ---> TIMEOUT: Activation took too long. Assuming failure.\n");
    state->success = FALSE;
    g_main_loop_quit(state->loop);
    return G_SOURCE_REMOVE;
}


static gchar *find_wifi_device_path() {
    g_return_val_if_fail(g_context && g_context->nm_proxy, NULL);

    g_autoptr(GVariant) devices_variant = g_dbus_proxy_get_cached_property(g_context->nm_proxy, "AllDevices");
    if (!devices_variant) {
        g_warning("Could not get AllDevices property");
        return NULL;
    }

    const gchar **device_paths = g_variant_get_objv(devices_variant, NULL);
    gchar *wifi_device_path = NULL;

    for (int i = 0; device_paths && device_paths[i] != NULL; ++i) {
        g_autoptr(GDBusProxy) device_proxy = g_dbus_proxy_new_for_bus_sync(
            G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE,
            device_paths[i], NM_DEVICE_INTERFACE, NULL, NULL);

        if (device_proxy) {
            g_autoptr(GVariant) type_v = g_dbus_proxy_get_cached_property(device_proxy, "DeviceType");
            if (type_v && g_variant_get_uint32(type_v) == NM_DEVICE_TYPE_WIFI) {
                wifi_device_path = g_strdup(device_paths[i]);
                break;
            }
        }
    }
    return wifi_device_path;
}

gchar *find_connection_for_ssid(const gchar *ssid) {
    g_return_val_if_fail(g_context && g_context->settings_proxy, NULL);

    g_autoptr(GVariant) connections_variant = g_dbus_proxy_call_sync(
        g_context->settings_proxy, "ListConnections", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if (!connections_variant) {
        return NULL;
    }

    gchar *found_path = NULL;
    g_autoptr(GVariantIter) iter;
    g_variant_get(connections_variant, "(ao)", &iter);
    const gchar *conn_path;

    while (g_variant_iter_loop(iter, "o", &conn_path)) {
        g_autoptr(GDBusProxy) conn_proxy = g_dbus_proxy_new_for_bus_sync(
            G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE,
            conn_path, NM_SETTINGS_CONNECTION_INTERFACE, NULL, NULL);
        if (!conn_proxy) continue;

        g_autoptr(GVariant) settings_variant = g_dbus_proxy_call_sync(
            conn_proxy, "GetSettings", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
        if (!settings_variant) continue;

        g_autoptr(GVariant) settings_dict = g_variant_get_child_value(settings_variant, 0);
        g_autoptr(GVariant) wifi_settings = g_variant_lookup_value(settings_dict, "802-11-wireless", G_VARIANT_TYPE("a{sv}"));
        if (!wifi_settings) continue;

        g_autoptr(GVariant) ssid_variant = g_variant_lookup_value(wifi_settings, "ssid", G_VARIANT_TYPE("ay"));
        if (ssid_variant) {
            gsize len;
            const gchar *ssid_bytes = g_variant_get_fixed_array(ssid_variant, &len, sizeof(guint8));
            if (len == strlen(ssid) && memcmp(ssid_bytes, ssid, len) == 0) {
                found_path = g_strdup(conn_path);
                break;
            }
        }
    }
    return found_path;
}

static gint sort_networks(gconstpointer a, gconstpointer b) {
    const WifiNetwork *na = a;
    const WifiNetwork *nb = b;

    if (na->is_active && !nb->is_active) return -1;
    if (!na->is_active && nb->is_active) return 1;
    if (na->is_known && !nb->is_known) return -1;
    if (!na->is_known && nb->is_known) return 1;
    if (na->strength > nb->strength) return -1;
    if (na->strength < nb->strength) return 1;

    return g_strcmp0(na->ssid, nb->ssid);
}

/* --- Public API Implementation --- */

gboolean network_manager_init() {
    g_return_val_if_fail(g_context == NULL, TRUE);
    g_autoptr(GError) error = NULL;

    g_context = g_new0(NetworkManagerContext, 1);
    g_autoptr(GDBusConnection) connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);

    if (error) {
        g_warning("Failed to get D-Bus system bus connection: %s", error->message);
        network_manager_shutdown();
        return FALSE;
    }

    g_context->nm_proxy = g_dbus_proxy_new_sync(
        connection, G_DBUS_PROXY_FLAGS_NONE, NULL,
        NM_DBUS_SERVICE, NM_DBUS_PATH, NM_DBUS_INTERFACE, NULL, &error);

    if (error || g_context->nm_proxy == NULL) {
        g_warning("Failed to create NM proxy: %s", error ? error->message : "Proxy creation returned NULL");
        g_clear_error(&error);
        network_manager_shutdown();
        return FALSE;
    }

    g_context->settings_proxy = g_dbus_proxy_new_sync(
        connection, G_DBUS_PROXY_FLAGS_NONE, NULL,
        NM_DBUS_SERVICE, NM_SETTINGS_PATH, NM_SETTINGS_INTERFACE, NULL, &error);

    if (error || g_context->settings_proxy == NULL) {
        g_warning("Failed to create NM Settings proxy: %s", error ? error->message : "Proxy creation returned NULL");
        network_manager_shutdown();
        return FALSE;
    }

    g_print("NetworkManager D-Bus interface initialized successfully.\n");
    return TRUE;
}

void network_manager_shutdown() {
    if (!g_context) return;
    g_clear_object(&g_context->nm_proxy);
    g_clear_object(&g_context->settings_proxy);
    g_free(g_context);
    g_context = NULL;
    g_print("NetworkManager D-Bus interface shut down.\n");
}

gboolean is_wifi_enabled() {
    g_return_val_if_fail(g_context && g_context->nm_proxy, FALSE);
    g_autoptr(GVariant) prop = g_dbus_proxy_get_cached_property(g_context->nm_proxy, "WirelessEnabled");
    return prop ? g_variant_get_boolean(prop) : FALSE;
}

gboolean is_airplane_mode_active() {
    g_return_val_if_fail(g_context && g_context->nm_proxy, FALSE);
    g_autoptr(GVariant) prop = g_dbus_proxy_get_cached_property(g_context->nm_proxy, "State");
    return prop ? (g_variant_get_uint32(prop) == NM_STATE_ASLEEP) : FALSE;
}

gboolean is_connection_forgettable(const gchar *connection_path, gboolean is_network_secure) {
    g_return_val_if_fail(connection_path != NULL, FALSE);
    if (!is_network_secure) {
        return TRUE;
    }

    g_autoptr(GDBusProxy) conn_proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE,
        connection_path, NM_SETTINGS_CONNECTION_INTERFACE, NULL, NULL);
    if (!conn_proxy) {
        return FALSE;
    }

    g_autoptr(GVariant) settings_variant = g_dbus_proxy_call_sync(
        conn_proxy, "GetSettings", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if (!settings_variant) {
        return FALSE;
    }

    g_autoptr(GVariant) settings_dict = g_variant_get_child_value(settings_variant, 0);
    g_autoptr(GVariant) security_settings = g_variant_lookup_value(
        settings_dict, "802-11-wireless-security", G_VARIANT_TYPE("a{sv}"));

    if (security_settings == NULL) {
        return FALSE;
    }

    return (g_variant_lookup_value(security_settings, "psk", G_VARIANT_TYPE_STRING) != NULL);
}

GList *get_available_wifi_networks() {
    g_print("\n--- [WIFI DEBUG] Starting get_available_wifi_networks ---\n");
    g_autofree gchar *wifi_device_path = find_wifi_device_path();
    if (!wifi_device_path) {
        g_warning("[WIFI WARNING] No Wi-Fi device path found. Aborting scan.");
        return NULL;
    }

    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusProxy) wireless_proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE,
        wifi_device_path, NM_WIRELESS_DEVICE_INTERFACE, NULL, &error);

    if (error) {
        g_warning("[WIFI WARNING] Failed to create wireless device proxy: %s", error->message);
        return NULL;
    }

    g_autoptr(GVariant) aps_variant = g_dbus_proxy_call_sync(
        wireless_proxy, "GetAllAccessPoints", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if (!aps_variant) {
        g_warning("[WIFI WARNING] GetAllAccessPoints call returned NULL.");
        return NULL;
    }

    g_autoptr(GVariant) active_ap_variant = g_dbus_proxy_get_cached_property(wireless_proxy, "ActiveAccessPoint");
    const gchar *active_ap_path = active_ap_variant ? g_variant_get_string(active_ap_variant, NULL) : NULL;

    guint32 global_connectivity = NM_CONNECTIVITY_UNKNOWN;
    g_autoptr(GVariant) connectivity_variant = g_dbus_proxy_get_cached_property(g_context->nm_proxy, "Connectivity");
    if (connectivity_variant) {
        global_connectivity = g_variant_get_uint32(connectivity_variant);
    }

    // Build a hash table of known SSIDs
    GHashTable *saved_ssids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GVariant) connections_variant = g_dbus_proxy_call_sync(
        g_context->settings_proxy, "ListConnections", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);

    if (connections_variant) {
        g_autoptr(GVariantIter) iter;
        g_variant_get(connections_variant, "(ao)", &iter);
        const gchar *conn_path;
        while (g_variant_iter_loop(iter, "o", &conn_path)) {
            g_autoptr(GDBusProxy) conn_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE, conn_path, NM_SETTINGS_CONNECTION_INTERFACE, NULL, NULL);
            if (!conn_proxy) continue;
            g_autoptr(GVariant) settings_variant = g_dbus_proxy_call_sync(conn_proxy, "GetSettings", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
            if (!settings_variant) continue;
            g_autoptr(GVariant) settings_dict = g_variant_get_child_value(settings_variant, 0);
            g_autoptr(GVariant) wifi_settings = g_variant_lookup_value(settings_dict, "802-11-wireless", G_VARIANT_TYPE("a{sv}"));
            if (!wifi_settings) continue;
            g_autoptr(GVariant) ssid_variant = g_variant_lookup_value(wifi_settings, "ssid", G_VARIANT_TYPE("ay"));
            if (ssid_variant) {
                gsize len;
                const gchar *ssid_bytes = g_variant_get_fixed_array(ssid_variant, &len, sizeof(guint8));
                g_autofree gchar *ssid_str = g_strndup(ssid_bytes, len);
                g_hash_table_replace(saved_ssids, g_strdup(ssid_str), GINT_TO_POINTER(1));
            }
        }
    } else {
        g_warning("[WIFI WARNING] ListConnections call failed. Cannot determine saved networks.");
    }

    // Iterate through available access points
    GList *networks = NULL;
    g_autoptr(GVariantIter) ap_iter;
    g_variant_get(aps_variant, "(ao)", &ap_iter);
    const gchar *ap_path;
    while (g_variant_iter_loop(ap_iter, "o", &ap_path)) {
        g_autoptr(GDBusProxy) ap_proxy = g_dbus_proxy_new_for_bus_sync(
            G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
            NM_DBUS_SERVICE, ap_path, NM_AP_INTERFACE, NULL, NULL);
        if (!ap_proxy) continue;

        g_autoptr(GVariant) ssid_v = g_dbus_proxy_get_cached_property(ap_proxy, "Ssid");
        if (!ssid_v) continue;

        gsize n_elements;
        const guint8 *ssid_bytes = g_variant_get_fixed_array(ssid_v, &n_elements, sizeof(guint8));
        g_autofree gchar *ssid_str = g_strndup((const gchar *)ssid_bytes, n_elements);
        if (strlen(ssid_str) == 0) {
            continue;
        }

        WifiNetwork *net = g_new0(WifiNetwork, 1);
        net->ssid = g_strdup(ssid_str);
        net->object_path = g_strdup(ap_path);

        g_autoptr(GVariant) strength_v = g_dbus_proxy_get_cached_property(ap_proxy, "Strength");
        net->strength = strength_v ? g_variant_get_byte(strength_v) : 0;
        net->is_active = (active_ap_path && g_strcmp0(ap_path, active_ap_path) == 0);

        g_autoptr(GVariant) flags_v = g_dbus_proxy_get_cached_property(ap_proxy, "Flags");
        g_autoptr(GVariant) wpaflags_v = g_dbus_proxy_get_cached_property(ap_proxy, "WpaFlags");
        g_autoptr(GVariant) rsnflags_v = g_dbus_proxy_get_cached_property(ap_proxy, "RsnFlags");
        guint32 flags = flags_v ? g_variant_get_uint32(flags_v) : 0;
        guint32 wpa_flags = wpaflags_v ? g_variant_get_uint32(wpaflags_v) : 0;
        guint32 rsn_flags = rsnflags_v ? g_variant_get_uint32(rsnflags_v) : 0;
        net->is_secure = (flags != 0 || wpa_flags != 0 || rsn_flags != 0);
        net->is_known = g_hash_table_contains(saved_ssids, net->ssid);

        if (net->is_active) {
            switch (global_connectivity) {
                case NM_CONNECTIVITY_FULL:
                    net->connectivity = WIFI_STATE_CONNECTED;
                    break;
                case NM_CONNECTIVITY_LIMITED:
                case NM_CONNECTIVITY_PORTAL:
                    net->connectivity = WIFI_STATE_LIMITED;
                    break;
                default:
                    net->connectivity = WIFI_STATE_CONNECTING;
                    break;
            }
        } else {
            net->connectivity = WIFI_STATE_DISCONNECTED;
        }
        networks = g_list_prepend(networks, net);
    }
    
    g_hash_table_destroy(saved_ssids);
    return g_list_sort(networks, (GCompareFunc)sort_networks);
}


/* --- Asynchronous Operations --- */

static void on_operation_finished(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object;
    OperationFinishData *finish_data = user_data;
    gboolean success = g_task_propagate_boolean(G_TASK(res), NULL);

    if (finish_data && finish_data->user_callback) {
        finish_data->user_callback(success, finish_data->user_data);
    }
    g_free(finish_data);
}

static void on_get_details_finished(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object;
    GetDetailsFinishData *finish_data = user_data;
    g_autoptr(GError) error = NULL;
    WifiNetworkDetails *details = g_task_propagate_pointer(G_TASK(res), &error);

    if (error) {
        g_warning("Failed to get network details: %s", error->message);
        g_clear_pointer(&details, free_wifi_network_details);
    }

    if (finish_data->user_callback) {
        finish_data->user_callback(details, finish_data->user_data);
    } else {
        if (details) {
            free_wifi_network_details(details);
        }
    }
    g_free(finish_data);
}

void set_wifi_enabled_async(gboolean enabled, NetworkOperationCallback cb, gpointer ud) {
    SetEnabledTaskData *td = g_new0(SetEnabledTaskData, 1);
    td->enabled = enabled;

    OperationFinishData *fd = g_new0(OperationFinishData, 1);
    fd->user_callback = cb;
    fd->user_data = ud;

    GTask *task = g_task_new(NULL, NULL, on_operation_finished, fd);
    g_task_set_task_data(task, td, g_free);
    g_task_run_in_thread(task, set_enabled_task_thread_func);
    g_object_unref(task);
}

void activate_wifi_connection_async(const gchar *connection_path, const gchar *ap_path, NetworkOperationCallback cb, gpointer ud) {
    ActivateConnectionTaskData *td = g_new0(ActivateConnectionTaskData, 1);
    td->connection_path = g_strdup(connection_path);
    td->ap_path = g_strdup(ap_path);

    OperationFinishData *fd = g_new0(OperationFinishData, 1);
    fd->user_callback = cb;
    fd->user_data = ud;

    GTask *task = g_task_new(NULL, NULL, on_operation_finished, fd);
    g_task_set_task_data(task, td, activate_connection_task_data_free);
    g_task_run_in_thread(task, activate_connection_thread_func);
    g_object_unref(task);
}

void add_and_activate_wifi_connection_async(const gchar *ssid, const gchar *ap_path, const gchar *password, gboolean is_secure, NetworkOperationCallback cb, gpointer ud) {
    AddConnectionTaskData *td = g_new0(AddConnectionTaskData, 1);
    td->ssid = g_strdup(ssid);
    td->ap_path = g_strdup(ap_path);
    td->password = g_strdup(password);
    td->is_secure = is_secure;

    OperationFinishData *fd = g_new0(OperationFinishData, 1);
    fd->user_callback = cb;
    fd->user_data = ud;

    GTask *task = g_task_new(NULL, NULL, on_operation_finished, fd);
    g_task_set_task_data(task, td, add_connection_task_data_free);
    g_task_run_in_thread(task, add_and_activate_connection_thread_func);
    g_object_unref(task);
}

void forget_wifi_connection_async(const gchar *ssid, NetworkOperationCallback cb, gpointer ud) {
    ForgetTaskData *td = g_new0(ForgetTaskData, 1);
    td->ssid = g_strdup(ssid);

    OperationFinishData *fd = g_new0(OperationFinishData, 1);
    fd->user_callback = cb;
    fd->user_data = ud;

    GTask *task = g_task_new(NULL, NULL, on_operation_finished, fd);
    g_task_set_task_data(task, td, forget_task_data_free);
    g_task_run_in_thread(task, forget_task_thread_func);
    g_object_unref(task);
}

void disconnect_wifi_async(NetworkOperationCallback cb, gpointer ud) {
    OperationFinishData *fd = g_new0(OperationFinishData, 1);
    fd->user_callback = cb;
    fd->user_data = ud;

    GTask *task = g_task_new(NULL, NULL, on_operation_finished, fd);
    g_task_run_in_thread(task, disconnect_task_thread_func);
    g_object_unref(task);
}

void get_wifi_network_details_async(const gchar *ap_path, WifiDetailsCallback cb, gpointer ud) {
    GetDetailsTaskData *td = g_new0(GetDetailsTaskData, 1);
    td->ap_path = g_strdup(ap_path);
    
    GetDetailsFinishData *fd = g_new0(GetDetailsFinishData, 1);
    fd->user_callback = cb;
    fd->user_data = ud;
    
    GTask *task = g_task_new(NULL, NULL, on_get_details_finished, fd);
    g_task_set_task_data(task, td, (GDestroyNotify)g_free);
    g_task_run_in_thread(task, get_details_thread_func);
    g_object_unref(task);
}

/* --- GTask Thread Functions --- */

static void set_enabled_task_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)d; (void)c;
    SetEnabledTaskData *data = g_task_get_task_data(task);
    g_autoptr(GError) error = NULL;

    g_dbus_proxy_call_sync(g_context->nm_proxy, "org.freedesktop.DBus.Properties.Set",
        g_variant_new("(ssv)", NM_DBUS_INTERFACE, "WirelessEnabled", g_variant_new_boolean(data->enabled)),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

    g_task_return_boolean(task, error == NULL);
}

static void activate_connection_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)d; (void)c;
    ActivateConnectionTaskData *data = g_task_get_task_data(task);
    g_autofree gchar *device_path = find_wifi_device_path();
    if (!device_path) {
        g_warning("No Wi-Fi device for activation.");
        g_task_return_boolean(task, FALSE);
        return;
    }

    g_autoptr(GError) error = NULL;
    g_dbus_proxy_call_sync(g_context->nm_proxy, "ActivateConnection",
        g_variant_new("(ooo)", data->connection_path, device_path, data->ap_path),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

    if (error) {
        g_warning("Failed to activate connection %s: %s", data->connection_path, error->message);
        g_task_return_boolean(task, FALSE);
    } else {
        g_task_return_boolean(task, TRUE);
    }
}


static void add_and_activate_connection_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)d; (void)c;
    AddConnectionTaskData *data = g_task_get_task_data(task);
    gboolean overall_success = FALSE;
    g_autofree gchar *new_connection_path = NULL;

    g_print("\n\n--- [NM DEBUG] STARTING Add/Activate for SSID: '%s' ---\n", data->ssid);

    g_autofree gchar *device_path = find_wifi_device_path();
    if (!device_path) {
        g_warning("[NM DEBUG] ---> FAILURE: Could not find a Wi-Fi device path. Aborting.");
        g_task_return_boolean(task, FALSE);
        return;
    }
    
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariantBuilder) settings_b = g_variant_builder_new(G_VARIANT_TYPE("a{sa{sv}}"));
    
    // --- Build settings dictionary using the safest possible method ---
    g_print("[NM DEBUG] ---> Building settings dictionary...\n");
    g_variant_builder_open(settings_b, G_VARIANT_TYPE("{sa{sv}}"));
    g_variant_builder_add_value(settings_b, g_variant_new_string("connection"));
    g_variant_builder_open(settings_b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(settings_b, "{sv}", "type", g_variant_new_string("802-11-wireless"));
    g_variant_builder_add(settings_b, "{sv}", "id", g_variant_new_string(data->ssid));
    g_autofree gchar *uuid = g_uuid_string_random();
    g_variant_builder_add(settings_b, "{sv}", "uuid", g_variant_new_string(uuid));
    g_variant_builder_close(settings_b); g_variant_builder_close(settings_b);
    g_variant_builder_open(settings_b, G_VARIANT_TYPE("{sa{sv}}"));
    g_variant_builder_add_value(settings_b, g_variant_new_string("802-11-wireless"));
    g_variant_builder_open(settings_b, G_VARIANT_TYPE("a{sv}"));
    g_autoptr(GVariant) ssid_v = g_variant_new_from_data(G_VARIANT_TYPE("ay"), data->ssid, strlen(data->ssid), TRUE, NULL, NULL);
    g_variant_builder_add(settings_b, "{sv}", "ssid", ssid_v);
    g_variant_builder_close(settings_b); g_variant_builder_close(settings_b);
    g_variant_builder_open(settings_b, G_VARIANT_TYPE("{sa{sv}}"));
    g_variant_builder_add_value(settings_b, g_variant_new_string("ipv4"));
    g_variant_builder_open(settings_b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(settings_b, "{sv}", "method", g_variant_new_string("auto"));
    g_variant_builder_close(settings_b); g_variant_builder_close(settings_b);
    g_variant_builder_open(settings_b, G_VARIANT_TYPE("{sa{sv}}"));
    g_variant_builder_add_value(settings_b, g_variant_new_string("ipv6"));
    g_variant_builder_open(settings_b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(settings_b, "{sv}", "method", g_variant_new_string("auto"));
    g_variant_builder_close(settings_b); g_variant_builder_close(settings_b);
    g_variant_builder_open(settings_b, G_VARIANT_TYPE("{sa{sv}}"));
    g_variant_builder_add_value(settings_b, g_variant_new_string("802-11-wireless-security"));
    g_variant_builder_open(settings_b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(settings_b, "{sv}", "key-mgmt", g_variant_new_string("wpa-psk"));
    g_variant_builder_close(settings_b); g_variant_builder_close(settings_b);
    g_autoptr(GVariant) settings = g_variant_builder_end(settings_b);
    g_print("[NM DEBUG] ---> Settings dictionary built.\n");

    // =========================================================================
    // THIS IS THE FIX. Use g_variant_new_tuple to wrap the existing 'settings' variant.
    // =========================================================================
    g_autoptr(GVariant) params_add = g_variant_new_tuple(&settings, 1);
    g_autoptr(GVariant) add_result = g_dbus_proxy_call_sync(g_context->settings_proxy, "AddConnection", params_add, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    
    if (error) {
        g_warning("[NM DEBUG] ---> FAILURE on AddConnection: %s\n", error->message);
        g_task_return_boolean(task, FALSE); return;
    }
    g_variant_get(add_result, "(o)", &new_connection_path);
    g_print("[NM DEBUG] ---> AddConnection SUCCEEDED. New profile path: %s\n", new_connection_path);

    g_autoptr(GVariant) activate_result = g_dbus_proxy_call_sync(g_context->nm_proxy, "ActivateConnection", g_variant_new("(ooo)", new_connection_path, device_path, data->ap_path), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    if (error) {
        g_warning("[NM DEBUG] ---> IMMEDIATE FAILURE on ActivateConnection: %s.\n", error->message);
        overall_success = FALSE;
    } else {
        ActivationState state = { .loop = g_main_loop_new(NULL, FALSE), .success = FALSE };
        g_variant_get(activate_result, "(o)", &state.active_conn_path);
        g_print("[NM DEBUG] ---> ActivateConnection call sent. Monitoring object: %s\n", state.active_conn_path);
        
        GDBusConnection *bus = g_dbus_proxy_get_connection(g_context->nm_proxy);
        state.sub_properties = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.NetworkManager", "org.freedesktop.DBus.Properties", "PropertiesChanged", state.active_conn_path, "org.freedesktop.NetworkManager.Connection.Active", G_DBUS_SIGNAL_FLAGS_NONE, on_active_connection_state_changed, &state, NULL);
        state.sub_object_removed = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.NetworkManager", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", "/org/freedesktop/NetworkManager", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_object_removed, &state, NULL);
        state.sub_timeout = g_timeout_add_seconds(60, on_activation_timeout, &state);
        
        g_main_loop_run(state.loop);

        g_dbus_connection_signal_unsubscribe(bus, state.sub_properties);
        g_dbus_connection_signal_unsubscribe(bus, state.sub_object_removed);
        g_source_remove(state.sub_timeout);
        g_main_loop_unref(state.loop);
        g_free(state.active_conn_path);
        overall_success = state.success;
    }

    if (!overall_success) {
        g_print("[NM DEBUG] ---> Overall activation failed. Deleting temporary profile: %s\n", new_connection_path);
        g_autoptr(GError) delete_error = NULL;
        g_autoptr(GDBusProxy) conn_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE, new_connection_path, NM_SETTINGS_CONNECTION_INTERFACE, NULL, NULL);
        if (conn_proxy) {
            g_dbus_proxy_call_sync(conn_proxy, "Delete", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &delete_error);
            if (delete_error) g_warning("[NM DEBUG] ---> FAILED to delete temporary profile: %s\n", delete_error->message);
            else g_print("[NM DEBUG] ---> Temporary profile deleted successfully.\n");
        }
    } else {
        g_print("[NM DEBUG] ---> Overall activation succeeded. Profile will be kept.\n");
    }
    
    g_print("[NM DEBUG] Operation complete. Returning status: %s\n", overall_success ? "SUCCESS" : "FAILURE");
    g_task_return_boolean(task, overall_success);
}


static void forget_task_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)d; (void)c;
    ForgetTaskData *data = g_task_get_task_data(task);
    g_print("[FORGET DEBUG] 2. Background thread started for SSID: '%s'\n", data->ssid);
    gboolean overall_success = TRUE;

    g_autofree gchar *connection_to_forget = find_connection_for_ssid(data->ssid);
    if (connection_to_forget) {
        g_print("[FORGET DEBUG] 3. Found profile to delete at D-Bus path: %s\n", connection_to_forget);
        g_autoptr(GDBusProxy) conn_proxy = g_dbus_proxy_new_for_bus_sync(
            G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE,
            connection_to_forget, NM_SETTINGS_CONNECTION_INTERFACE, NULL, NULL);

        if (conn_proxy) {
            g_autoptr(GError) delete_error = NULL;
            g_dbus_proxy_call_sync(conn_proxy, "Delete", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &delete_error);
            if (delete_error) {
                g_warning("[FORGET DEBUG] 4. D-Bus 'Delete' call FAILED: %s", delete_error->message);
                overall_success = FALSE;
            } else {
                g_print("[FORGET DEBUG] 4. D-Bus 'Delete' call SUCCEEDED.\n");
            }
        } else {
            g_warning("[FORGET DEBUG] 4. FAILED to create D-Bus proxy for the connection.\n");
            overall_success = FALSE;
        }
    } else {
        g_print("[FORGET DEBUG] 3. No saved profile found for SSID '%s'. Nothing to delete.\n", data->ssid);
        overall_success = TRUE;
    }
    g_task_return_boolean(task, overall_success);
}

static void disconnect_task_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)d; (void)c;
    g_autofree gchar *device_path = find_wifi_device_path();
    if (!device_path) {
        g_warning("No Wi-Fi device to disconnect.");
        g_task_return_boolean(task, FALSE);
        return;
    }

    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusProxy) device_proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE,
        device_path, NM_DEVICE_INTERFACE, NULL, &error);

    if (error) {
        g_warning("Failed to create device proxy for disconnect: %s", error->message);
        g_task_return_boolean(task, FALSE);
        return;
    }

    g_dbus_proxy_call_sync(device_proxy, "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    g_task_return_boolean(task, error == NULL);
}

static void get_details_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)d; (void)c;
    GetDetailsTaskData *data = g_task_get_task_data(task);
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusProxy) ap_proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE,
        data->ap_path, NM_AP_INTERFACE, NULL, &error);

    if (error) {
        g_warning("Failed to create AP proxy for details: %s", error->message);
        g_task_return_pointer(task, NULL, NULL);
        return;
    }

    WifiNetworkDetails *details = g_new0(WifiNetworkDetails, 1);

    g_autoptr(GVariant) ssid_v = g_dbus_proxy_get_cached_property(ap_proxy, "Ssid");
    if (ssid_v) {
        gsize len;
        const guint8 *ssid_bytes = g_variant_get_fixed_array(ssid_v, &len, sizeof(guint8));
        details->ssid = g_strndup((const gchar *)ssid_bytes, len);
    }

    g_autoptr(GVariant) strength_v = g_dbus_proxy_get_cached_property(ap_proxy, "Strength");
    if (strength_v) details->strength = g_variant_get_byte(strength_v);

    g_autoptr(GVariant) mac_v = g_dbus_proxy_get_cached_property(ap_proxy, "HwAddress");
    if (mac_v) details->mac_address = g_variant_dup_string(mac_v, NULL);

    g_autoptr(GVariant) rsnflags_v = g_dbus_proxy_get_cached_property(ap_proxy, "RsnFlags");
    if (rsnflags_v && g_variant_get_uint32(rsnflags_v) != 0) {
        details->security = g_strdup("WPA2/WPA3");
    } else {
        g_autoptr(GVariant) wpaflags_v = g_dbus_proxy_get_cached_property(ap_proxy, "WpaFlags");
        if (wpaflags_v && g_variant_get_uint32(wpaflags_v) != 0) {
            details->security = g_strdup("WPA");
        } else {
            details->security = g_strdup("Open");
        }
    }

    g_autofree gchar *wifi_dev_path = find_wifi_device_path();
    if (wifi_dev_path) {
        g_autoptr(GDBusProxy) wireless_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE, wifi_dev_path, NM_WIRELESS_DEVICE_INTERFACE, NULL, NULL);
        if (wireless_proxy) {
            g_autoptr(GVariant) active_ap_v = g_dbus_proxy_get_cached_property(wireless_proxy, "ActiveAccessPoint");
            const gchar *active_ap_path = active_ap_v ? g_variant_get_string(active_ap_v, NULL) : NULL;

            if (active_ap_path && g_strcmp0(active_ap_path, data->ap_path) == 0) {
                g_autoptr(GVariant) active_conn_v = g_dbus_proxy_get_cached_property(g_context->nm_proxy, "PrimaryConnection");
                const gchar *active_conn_path = active_conn_v ? g_variant_get_string(active_conn_v, NULL) : NULL;

                if (active_conn_path) {
                    g_autoptr(GDBusProxy) conn_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE, active_conn_path, "org.freedesktop.NetworkManager.Connection.Active", NULL, NULL);
                    g_autoptr(GVariant) ip4_config_v = g_dbus_proxy_get_cached_property(conn_proxy, "Ip4Config");
                    const gchar *ip4_config_path = ip4_config_v ? g_variant_get_string(ip4_config_v, NULL) : NULL;

                    if (ip4_config_path && g_strcmp0(ip4_config_path, "/") != 0) {
                        g_autoptr(GDBusProxy) ip4_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE, ip4_config_path, "org.freedesktop.NetworkManager.IP4Config", NULL, NULL);
                        g_autoptr(GVariant) addr_data_v = g_dbus_proxy_get_cached_property(ip4_proxy, "AddressData");

                        if (addr_data_v) {
                            GVariantIter iter;
                            GVariant *addr_dict_variant;
                            g_variant_iter_init(&iter, addr_data_v);

                            if (g_variant_iter_next(&iter, "a{sv}", &addr_dict_variant)) {
                                GVariantIter dict_iter;
                                gchar *key;
                                GVariant *value;
                                g_variant_iter_init(&dict_iter, addr_dict_variant);

                                while (g_variant_iter_next(&dict_iter, "{sv}", &key, &value)) {
                                    if (g_strcmp0(key, "address") == 0) {
                                        details->ip_address = g_variant_dup_string(value, NULL);
                                    }
                                    g_free(key);
                                    g_variant_unref(value);
                                    if (details->ip_address) break;
                                }
                                g_variant_unref(addr_dict_variant);
                            }
                        }
                    }
                }
            }
        }
    }

    g_task_return_pointer(task, details, (GDestroyNotify)free_wifi_network_details);
}

/* --- Public Memory Management --- */

void wifi_network_free(gpointer data) {
    if (!data) return;
    WifiNetwork *net = (WifiNetwork *)data;
    g_free(net->ssid);
    g_free(net->object_path);
    g_free(net);
}

void free_wifi_network_details(WifiNetworkDetails *details) {
    if (!details) return;
    g_free(details->ssid);
    g_free(details->security);
    g_free(details->ip_address);
    g_free(details->mac_address);
    g_free(details);
}

void free_wifi_network_list(GList *list) {
    g_list_free_full(list, wifi_network_free);
}