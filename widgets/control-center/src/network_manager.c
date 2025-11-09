// ===== src/network_manager.c =====
#include "network_manager.h"
#include <gio/gio.h>
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

// --- Context and Data Structures (Reverted to simple version) ---
typedef struct {
    GDBusProxy *nm_proxy;
    GDBusProxy *settings_proxy;
} NetworkManagerContext;
static NetworkManagerContext *g_context = NULL;

typedef struct { NetworkOperationCallback user_callback; gpointer user_data; } OperationFinishData;
typedef struct { gchar *ssid; gchar *ap_path; gchar *password; gboolean is_secure; } AddConnectionTaskData;
typedef struct { gchar *connection_path; gchar *ap_path; } ActivateConnectionTaskData;
typedef struct { gchar *ssid; } ForgetTaskData;
typedef struct { gboolean enabled; } SetEnabledTaskData;

// --- Forward Declarations for GTask functions ---
static void on_operation_finished(GObject *s, GAsyncResult *res, gpointer user_data);
static void forget_task_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c);
static void disconnect_task_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c);
static void add_and_activate_connection_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c);
static void activate_connection_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c);
static void set_enabled_task_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c);

// --- Memory management for task data ---
static void add_connection_task_data_free(gpointer data) {
    AddConnectionTaskData *d = data; if (!d) return; g_free(d->ssid); g_free(d->ap_path); g_free(d->password); g_free(d);
}
static void activate_connection_task_data_free(gpointer data) {
    ActivateConnectionTaskData *d = data; if (!d) return; g_free(d->connection_path); g_free(d->ap_path); g_free(d);
}
static void forget_task_data_free(gpointer data) {
    ForgetTaskData *d = data; if (!d) return; g_free(d->ssid); g_free(d);
}

// --- Helper Functions ---
static gchar* find_wifi_device_path() {
    g_return_val_if_fail(g_context && g_context->nm_proxy, NULL);
    g_autoptr(GVariant) devices_variant = g_dbus_proxy_get_cached_property(g_context->nm_proxy, "AllDevices");
    if (!devices_variant) { g_warning("Could not get AllDevices property"); return NULL; }
    const gchar **device_paths = g_variant_get_objv(devices_variant, NULL);
    gchar *wifi_device_path = NULL;
    for (int i = 0; device_paths && device_paths[i] != NULL; ++i) {
        g_autoptr(GDBusProxy) device_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE, device_paths[i], NM_DEVICE_INTERFACE, NULL, NULL);
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

// --- Detailed Network Info Functions ---
void free_wifi_network_details(WifiNetworkDetails *details) {
    if (!details) return;
    g_free(details->ssid);
    g_free(details->security);
    g_free(details->ip_address);
    g_free(details->mac_address);
    g_free(details);
}

// --- Async Task Data for Get Details ---
typedef struct {
    gchar *ap_path;
} GetDetailsTaskData;

typedef struct {
    WifiDetailsCallback user_callback;
    gpointer user_data;
} GetDetailsFinishData;


// Thread function: This runs in the background
static void get_details_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)d; (void)c;
    GetDetailsTaskData *data = g_task_get_task_data(task);
    
    // This is the exact same logic from your old blocking function
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusProxy) ap_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE, data->ap_path, NM_AP_INTERFACE, NULL, &error);
    if (error) { 
        g_warning("Failed to create AP proxy for details: %s", error->message); 
        g_task_return_pointer(task, NULL, NULL);
        return;
    }

    WifiNetworkDetails *details = g_new0(WifiNetworkDetails, 1);
    // ... (The rest of this function body is identical to your old get_wifi_network_details) ...
    g_autoptr(GVariant) ssid_v = g_dbus_proxy_get_cached_property(ap_proxy, "Ssid");
    if (ssid_v) {
        gsize len;
        const guint8 *ssid_bytes = g_variant_get_fixed_array(ssid_v, &len, sizeof(guint8));
        details->ssid = g_strndup((const gchar*)ssid_bytes, len);
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
                const gchar* active_conn_path = active_conn_v ? g_variant_get_string(active_conn_v, NULL) : NULL;
                if(active_conn_path) {
                    g_autoptr(GDBusProxy) conn_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE, active_conn_path, "org.freedesktop.NetworkManager.Connection.Active", NULL, NULL);
                    g_autoptr(GVariant) ip4_config_v = g_dbus_proxy_get_cached_property(conn_proxy, "Ip4Config");
                    const gchar* ip4_config_path = ip4_config_v ? g_variant_get_string(ip4_config_v, NULL) : NULL;
                    if(ip4_config_path && g_strcmp0(ip4_config_path, "/") != 0) {
                         g_autoptr(GDBusProxy) ip4_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE, ip4_config_path, "org.freedesktop.NetworkManager.IP4Config", NULL, NULL);
                         g_autoptr(GVariant) addr_data_v = g_dbus_proxy_get_cached_property(ip4_proxy, "AddressData");
                         if (addr_data_v) {
                            GVariantIter iter; GVariant *addr_dict_variant;
                            g_variant_iter_init(&iter, addr_data_v);
                            if (g_variant_iter_next(&iter, "a{sv}", &addr_dict_variant)) {
                                GVariantIter dict_iter; gchar *key; GVariant *value;
                                g_variant_iter_init(&dict_iter, addr_dict_variant);
                                while (g_variant_iter_next(&dict_iter, "{sv}", &key, &value)) {
                                    if (g_strcmp0(key, "address") == 0) details->ip_address = g_variant_dup_string(value, NULL);
                                    g_free(key); g_variant_unref(value);
                                    if(details->ip_address) break;
                                }
                                g_variant_unref(addr_dict_variant);
                            }
                         }
                    }
                }
            }
        }
    }
    
    // Return the result to the main thread
    g_task_return_pointer(task, details, (GDestroyNotify)free_wifi_network_details);
}

// Finish function: This runs on the main thread after the background work is done
static void on_get_details_finished(GObject *s, GAsyncResult *res, gpointer user_data) {
    (void)s;
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
        if (details) free_wifi_network_details(details);
    }

    g_free(finish_data);
}

// Public API function
void get_wifi_network_details_async(const gchar *ap_path, WifiDetailsCallback cb, gpointer ud) {
    GetDetailsTaskData *task_data = g_new0(GetDetailsTaskData, 1);
    task_data->ap_path = g_strdup(ap_path);

    GetDetailsFinishData *finish_data = g_new0(GetDetailsFinishData, 1);
    finish_data->user_callback = cb;
    finish_data->user_data = ud;

    GTask *task = g_task_new(NULL, NULL, on_get_details_finished, finish_data);
    g_task_set_task_data(task, task_data, (GDestroyNotify)g_free);
    g_task_run_in_thread(task, get_details_thread_func);
    g_object_unref(task);
}

gboolean is_connection_forgettable(const gchar *connection_path, gboolean is_network_secure) {
    g_return_val_if_fail(connection_path != NULL, FALSE);
    if (!is_network_secure) return TRUE;
    g_autoptr(GDBusProxy) conn_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE, connection_path, NM_SETTINGS_CONNECTION_INTERFACE, NULL, NULL);
    if (!conn_proxy) return FALSE;
    g_autoptr(GVariant) settings_variant = g_dbus_proxy_call_sync(conn_proxy, "GetSettings", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if (!settings_variant) return FALSE;
    g_autoptr(GVariant) settings_dict = g_variant_get_child_value(settings_variant, 0);
    g_autoptr(GVariant) security_settings = g_variant_lookup_value(settings_dict, "802-11-wireless-security", G_VARIANT_TYPE("a{sv}"));
    if (security_settings == NULL) return FALSE;
    return (g_variant_lookup_value(security_settings, "psk", G_VARIANT_TYPE_STRING) != NULL);
}

// --- Init and Shutdown (Reverted to simple version) ---
// --- Init and Shutdown ---
gboolean network_manager_init() {
    g_return_val_if_fail(g_context == NULL, TRUE);
    g_autoptr(GError) error = NULL;
    g_context = g_new0(NetworkManagerContext, 1);

    // --- THE NEW, MORE ROBUST METHOD ---
    g_autoptr(GDBusConnection) connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (error) {
        g_warning("Failed to get D-Bus system bus connection: %s", error->message);
        network_manager_shutdown();
        return FALSE;
    }

    // Create the main NetworkManager proxy
    g_context->nm_proxy = g_dbus_proxy_new_sync(
        connection,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,                            // GDBusInterfaceInfo
        NM_DBUS_SERVICE,                 // "org.freedesktop.NetworkManager"
        NM_DBUS_PATH,                    // "/org/freedesktop/NetworkManager" <-- The CORRECT path
        NM_DBUS_INTERFACE,               // "org.freedesktop.NetworkManager"
        NULL,                            // GCancellable
        &error
    );

    if (error || g_context->nm_proxy == NULL) {
        g_warning("Failed to create NM proxy: %s", error ? error->message : "Proxy creation returned NULL");
        g_clear_error(&error);
        network_manager_shutdown();
        return FALSE;
    }

    // Create the Settings proxy
    g_context->settings_proxy = g_dbus_proxy_new_sync(
        connection,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,
        NM_DBUS_SERVICE,
        NM_SETTINGS_PATH,                // "/org/freedesktop/NetworkManager/Settings"
        NM_SETTINGS_INTERFACE,           // "org.freedesktop.NetworkManager.Settings"
        NULL,
        &error
    );
    
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

static void on_operation_finished(GObject *s, GAsyncResult *res, gpointer user_data) {
    (void)s;
    OperationFinishData *finish_data = user_data;
    gboolean success = g_task_propagate_boolean(G_TASK(res), NULL);
    if (finish_data && finish_data->user_callback) {
        finish_data->user_callback(success, finish_data->user_data);
    }
    g_free(finish_data);
}

// --- Radio Control Implementation ---
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
void set_wifi_enabled_async(gboolean enabled, NetworkOperationCallback cb, gpointer ud) {
    SetEnabledTaskData *task_data = g_new0(SetEnabledTaskData, 1);
    task_data->enabled = enabled;
    OperationFinishData *finish_data = g_new0(OperationFinishData, 1);
    finish_data->user_callback = cb;
    finish_data->user_data = ud;
    GTask *task = g_task_new(NULL, NULL, on_operation_finished, finish_data);
    g_task_set_task_data(task, task_data, g_free);
    g_task_run_in_thread(task, set_enabled_task_thread_func);
    g_object_unref(task);
}
static void set_enabled_task_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)d; (void)c;
    SetEnabledTaskData *data = g_task_get_task_data(task);
    g_autoptr(GError) error = NULL;
    g_dbus_proxy_call_sync(g_context->nm_proxy, "org.freedesktop.DBus.Properties.Set", g_variant_new("(ssv)", NM_DBUS_INTERFACE, "WirelessEnabled", g_variant_new_boolean(data->enabled)), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    g_task_return_boolean(task, error == NULL);
}

// --- Network List & Profile Functions ---
static gint sort_networks(gconstpointer a, gconstpointer b) {
    const WifiNetwork *net_a = a; const WifiNetwork *net_b = b;
    if (net_a->is_active && !net_b->is_active) return -1;
    if (!net_a->is_active && net_b->is_active) return 1;
    if (net_a->strength > net_b->strength) return -1;
    if (net_a->strength < net_b->strength) return 1;
    return g_strcmp0(net_a->ssid, net_b->ssid);
}

GList* get_available_wifi_networks() {
    g_autofree gchar *wifi_device_path = find_wifi_device_path();
    if (!wifi_device_path) return NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusProxy) wireless_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE, wifi_device_path, NM_WIRELESS_DEVICE_INTERFACE, NULL, &error);
    if (error) { g_warning("Failed to create wireless device proxy: %s", error->message); return NULL; }

  

    g_autoptr(GVariant) aps_variant = g_dbus_proxy_call_sync(wireless_proxy, "GetAllAccessPoints", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if (!aps_variant) return NULL;
    g_autoptr(GVariant) active_ap_variant = g_dbus_proxy_get_cached_property(wireless_proxy, "ActiveAccessPoint");
    const gchar *active_ap_path = active_ap_variant ? g_variant_get_string(active_ap_variant, NULL) : NULL;
    GList *networks = NULL;
    g_autoptr(GVariantIter) iter;
    g_variant_get(aps_variant, "(ao)", &iter);
    const gchar *ap_path;
    while (g_variant_iter_loop(iter, "o", &ap_path)) {
        g_autoptr(GDBusProxy) ap_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE, ap_path, NM_AP_INTERFACE, NULL, NULL);
        if (!ap_proxy) continue;
        g_autoptr(GVariant) ssid_v = g_dbus_proxy_get_cached_property(ap_proxy, "Ssid");
        g_autoptr(GVariant) strength_v = g_dbus_proxy_get_cached_property(ap_proxy, "Strength");
        g_autoptr(GVariant) flags_v = g_dbus_proxy_get_cached_property(ap_proxy, "Flags");
        g_autoptr(GVariant) wpaflags_v = g_dbus_proxy_get_cached_property(ap_proxy, "WpaFlags");
        g_autoptr(GVariant) rsnflags_v = g_dbus_proxy_get_cached_property(ap_proxy, "RsnFlags");
        if (!ssid_v || !strength_v) continue;
        gsize n_elements;
        const guint8 *ssid_bytes = g_variant_get_fixed_array(ssid_v, &n_elements, sizeof(guint8));
        gchar *ssid_str = g_strndup((const gchar*)ssid_bytes, n_elements);
        if(strlen(ssid_str) == 0) { g_free(ssid_str); continue; }
        WifiNetwork *net = g_new0(WifiNetwork, 1);
        net->ssid = ssid_str;
        net->object_path = g_strdup(ap_path);
        net->strength = g_variant_get_byte(strength_v);
        net->is_active = (active_ap_path && g_strcmp0(ap_path, active_ap_path) == 0);
        guint32 flags = flags_v ? g_variant_get_uint32(flags_v) : 0;
        guint32 wpa_flags = wpaflags_v ? g_variant_get_uint32(wpaflags_v) : 0;
        guint32 rsn_flags = rsnflags_v ? g_variant_get_uint32(rsnflags_v) : 0;
        net->is_secure = (flags != 0 || wpa_flags != 0 || rsn_flags != 0);
        networks = g_list_prepend(networks, net);
    }
    return g_list_sort(networks, sort_networks);
}

gchar* find_connection_for_ssid(const gchar *ssid) {
    g_return_val_if_fail(g_context && g_context->settings_proxy, NULL);
    g_autoptr(GVariant) connections_variant = g_dbus_proxy_call_sync(g_context->settings_proxy, "ListConnections", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if (!connections_variant) return NULL;
    gchar *found_path = NULL;
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
            if (len == strlen(ssid) && memcmp(ssid_bytes, ssid, len) == 0) {
                found_path = g_strdup(conn_path);
                break;
            }
        }
    }
    return found_path;
}

void activate_wifi_connection_async(const gchar *connection_path, const gchar *ap_path, NetworkOperationCallback cb, gpointer ud) {
    ActivateConnectionTaskData *task_data = g_new0(ActivateConnectionTaskData, 1);
    task_data->connection_path = g_strdup(connection_path);
    task_data->ap_path = g_strdup(ap_path);
    OperationFinishData *finish_data = g_new0(OperationFinishData, 1);
    finish_data->user_callback = cb;
    finish_data->user_data = ud;
    GTask *task = g_task_new(NULL, NULL, on_operation_finished, finish_data);
    g_task_set_task_data(task, task_data, activate_connection_task_data_free);
    g_task_run_in_thread(task, activate_connection_thread_func);
    g_object_unref(task);
}
static void activate_connection_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)d; (void)c;
    ActivateConnectionTaskData *data = g_task_get_task_data(task);
    g_autofree gchar *device_path = find_wifi_device_path();
    if (!device_path) { g_warning("No Wi-Fi device for activation."); g_task_return_boolean(task, FALSE); return; }
    g_autoptr(GError) error = NULL;
    g_dbus_proxy_call_sync(g_context->nm_proxy, "ActivateConnection", g_variant_new("(ooo)", data->connection_path, device_path, data->ap_path), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    if (error) { g_warning("Failed to activate connection %s: %s", data->connection_path, error->message); g_task_return_boolean(task, FALSE); } else { g_task_return_boolean(task, TRUE); }
}

void add_and_activate_wifi_connection_async(const gchar *ssid, const gchar *ap_path, const gchar *password, gboolean is_secure, NetworkOperationCallback cb, gpointer ud) {
    AddConnectionTaskData *task_data = g_new0(AddConnectionTaskData, 1);
    task_data->ssid = g_strdup(ssid);
    task_data->ap_path = g_strdup(ap_path);
    task_data->password = g_strdup(password);
    task_data->is_secure = is_secure;
    OperationFinishData *finish_data = g_new0(OperationFinishData, 1);
    finish_data->user_callback = cb;
    finish_data->user_data = ud;
    GTask *task = g_task_new(NULL, NULL, on_operation_finished, finish_data);
    g_task_set_task_data(task, task_data, add_connection_task_data_free);
    g_task_run_in_thread(task, add_and_activate_connection_thread_func);
    g_object_unref(task);
}


static void add_and_activate_connection_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)d; (void)c;
    AddConnectionTaskData *data = g_task_get_task_data(task);
    g_autofree gchar *device_path = find_wifi_device_path();
    if (!device_path) { g_warning("No Wi-Fi device for new connection."); g_task_return_boolean(task, FALSE); return; }
    g_autoptr(GError) error = NULL;
    GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify) g_variant_unref);
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", "type", g_variant_new_string("802-11-wireless"));
    g_variant_builder_add(&builder, "{sv}", "id", g_variant_new_string(data->ssid));
    g_autofree gchar* uuid = g_uuid_string_random();
    g_variant_builder_add(&builder, "{sv}", "uuid", g_variant_new_string(uuid));
    g_ptr_array_add(entries, g_variant_new_dict_entry(g_variant_new_string("connection"), g_variant_builder_end(&builder)));
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_autoptr(GVariant) ssid_v = g_variant_new_from_data(G_VARIANT_TYPE("ay"), data->ssid, strlen(data->ssid), TRUE, NULL, NULL);
    g_variant_builder_add(&builder, "{sv}", "ssid", ssid_v);
    g_variant_builder_add(&builder, "{sv}", "mode", g_variant_new_string("infrastructure"));
    g_ptr_array_add(entries, g_variant_new_dict_entry(g_variant_new_string("802-11-wireless"), g_variant_builder_end(&builder)));
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", "method", g_variant_new_string("auto"));
    g_ptr_array_add(entries, g_variant_new_dict_entry(g_variant_new_string("ipv4"), g_variant_builder_end(&builder)));
    if (data->is_secure) {
        g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&builder, "{sv}", "key-mgmt", g_variant_new_string("wpa-psk"));
        if (data->password && data->password[0] != '\0') {
            g_variant_builder_add(&builder, "{sv}", "psk", g_variant_new_string(data->password));
        }
        g_ptr_array_add(entries, g_variant_new_dict_entry(g_variant_new_string("802-11-wireless-security"), g_variant_builder_end(&builder)));
    }
    GVariant *settings = g_variant_new_array(G_VARIANT_TYPE_DICT_ENTRY, (GVariant **)entries->pdata, entries->len);
    g_ptr_array_free(entries, TRUE);
    g_dbus_proxy_call_sync(g_context->nm_proxy, "AddAndActivateConnection", g_variant_new("(a{sa{sv}}oo)", settings, device_path, data->ap_path), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    if (error) { g_warning("Failed to add connection: %s", error->message); g_task_return_boolean(task, FALSE); } else { g_print("D-Bus call successful.\n"); g_task_return_boolean(task, TRUE); }
}

void forget_wifi_connection_async(const gchar *ssid, NetworkOperationCallback cb, gpointer ud) {
    ForgetTaskData *task_data = g_new0(ForgetTaskData, 1);
    task_data->ssid = g_strdup(ssid);
    OperationFinishData *finish_data = g_new0(OperationFinishData, 1);
    finish_data->user_callback = cb;
    finish_data->user_data = ud;
    GTask *task = g_task_new(NULL, NULL, on_operation_finished, finish_data);
    g_task_set_task_data(task, task_data, forget_task_data_free);
    g_task_run_in_thread(task, forget_task_thread_func);
    g_object_unref(task);
}
static void forget_task_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)d; (void)c;
    ForgetTaskData *data = g_task_get_task_data(task);
    gboolean overall_success = TRUE;
    g_autofree gchar *connection_to_forget = find_connection_for_ssid(data->ssid);
    if (connection_to_forget) {
        g_print("==> Found matching profile at %s. Deleting.\n", connection_to_forget);
        g_autoptr(GDBusProxy) conn_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE, connection_to_forget, NM_SETTINGS_CONNECTION_INTERFACE, NULL, NULL);
        if (conn_proxy) {
            g_autoptr(GError) delete_error = NULL;
            g_dbus_proxy_call_sync(conn_proxy, "Delete", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &delete_error);
            if(delete_error) { g_warning("Failed to delete connection %s: %s", connection_to_forget, delete_error->message); overall_success = FALSE; }
        } else { overall_success = FALSE; }
    }
    g_task_return_boolean(task, overall_success);
}

void disconnect_wifi_async(NetworkOperationCallback cb, gpointer ud) {
    OperationFinishData *finish_data = g_new0(OperationFinishData, 1);
    finish_data->user_callback = cb;
    finish_data->user_data = ud;
    GTask *task = g_task_new(NULL, NULL, on_operation_finished, finish_data);
    g_task_run_in_thread(task, disconnect_task_thread_func);
    g_object_unref(task);
}
static void disconnect_task_thread_func(GTask *task, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)d; (void)c;
    g_autofree gchar *device_path = find_wifi_device_path();
    if (!device_path) { g_warning("No Wi-Fi device to disconnect."); g_task_return_boolean(task, FALSE); return; }
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusProxy) device_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, NM_DBUS_SERVICE, device_path, NM_DEVICE_INTERFACE, NULL, &error);
    if (error) { g_warning("Failed to create device proxy for disconnect: %s", error->message); g_task_return_boolean(task, FALSE); return; }
    g_dbus_proxy_call_sync(device_proxy, "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    g_task_return_boolean(task, error == NULL);
}

void wifi_network_free(gpointer data) {
    if (!data) return;
    WifiNetwork *net = (WifiNetwork*)data;
    g_free(net->ssid);
    g_free(net->object_path);
    g_free(net);
}
void free_wifi_network_list(GList *list) {
    g_list_free_full(list, wifi_network_free);
}