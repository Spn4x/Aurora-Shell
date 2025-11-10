// ===== widgets/control-center/src/bluetooth_manager.c (WITH DEBUG LOGGING) =====
#include "bluetooth_manager.h"
#include <gio/gio.h>
#include <string.h>

// D-Bus constants for BlueZ
#define BLUEZ_DBUS_SERVICE "org.bluez"
#define BLUEZ_DBUS_PATH "/"
#define BLUEZ_ADAPTER_INTERFACE "org.bluez.Adapter1"
#define BLUEZ_DEVICE_INTERFACE "org.bluez.Device1"

// --- Manager State ---
typedef struct {
    GDBusObjectManager *object_manager;
    BluetoothDeviceUpdateCallback update_callback;
    gpointer user_data;
    guint object_added_handler_id;
    guint object_removed_handler_id;
    guint properties_changed_handler_id;
} BluetoothManagerContext;

static BluetoothManagerContext *g_bt_context = NULL;

// --- Async Task Data ---
typedef struct { BluetoothOperationCallback cb; gpointer ud; } FinishData;
typedef struct { gchar *address; gboolean powered; } OperationData;

static void operation_data_free(gpointer data) {
    if (!data) return;
    OperationData *d = data;
    g_free(d->address);
    g_free(d);
}

// --- Forward Declarations ---
static void refresh_and_notify();

// --- Utility Functions ---
static gchar* find_adapter_path() {
    g_return_val_if_fail(g_bt_context && g_bt_context->object_manager, NULL);
    g_autoptr(GList) objects = g_dbus_object_manager_get_objects(g_bt_context->object_manager);
    for (GList *l = objects; l != NULL; l = l->next) {
        if (g_dbus_object_get_interface(l->data, BLUEZ_ADAPTER_INTERFACE)) {
            return g_strdup(g_dbus_object_get_object_path(l->data));
        }
    }
    return NULL;
}

static gchar* find_device_path_for_address(const gchar *address) {
    g_return_val_if_fail(g_bt_context && g_bt_context->object_manager, NULL);
    g_autoptr(GList) objects = g_dbus_object_manager_get_objects(g_bt_context->object_manager);
    for (GList *l = objects; l != NULL; l = l->next) {
        g_autoptr(GDBusInterface) iface = g_dbus_object_get_interface(l->data, BLUEZ_DEVICE_INTERFACE);
        if (iface) {
            g_autoptr(GVariant) addr_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(iface), "Address");
            if (addr_var && g_strcmp0(g_variant_get_string(addr_var, NULL), address) == 0) {
                return g_strdup(g_dbus_object_get_object_path(l->data));
            }
        }
    }
    return NULL;
}

// --- D-Bus Signal Handlers ---
static void on_object_added_or_removed(GDBusObjectManager *m, GDBusObject *o, gpointer d) { (void)m; (void)o; (void)d; refresh_and_notify(); }
static void on_properties_changed(GDBusObjectManagerClient *m, GDBusObjectProxy *o, GDBusProxy *i, GVariant *v, const gchar *const *iv, gpointer d) { (void)m; (void)o; (void)i; (void)v; (void)iv; (void)d; refresh_and_notify(); }
static void refresh_and_notify() { if (g_bt_context && g_bt_context->update_callback) { GList *devices = get_available_bluetooth_devices(); g_bt_context->update_callback(devices, g_bt_context->user_data); } }

// --- Lifecycle ---
gboolean bluetooth_manager_init(BluetoothDeviceUpdateCallback callback, gpointer user_data) {
    if (g_bt_context) return TRUE;
    g_bt_context = g_new0(BluetoothManagerContext, 1);
    g_bt_context->update_callback = callback; g_bt_context->user_data = user_data;
    g_autoptr(GError) error = NULL;
    g_bt_context->object_manager = g_dbus_object_manager_client_new_for_bus_sync( G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, BLUEZ_DBUS_SERVICE, BLUEZ_DBUS_PATH, NULL, NULL, NULL, NULL, &error);
    if (error) { g_warning("Failed to create BlueZ D-Bus object manager: %s", error->message); bluetooth_manager_shutdown(); return FALSE; }
    g_bt_context->object_added_handler_id = g_signal_connect(g_bt_context->object_manager, "object-added", G_CALLBACK(on_object_added_or_removed), NULL);
    g_bt_context->object_removed_handler_id = g_signal_connect(g_bt_context->object_manager, "object-removed", G_CALLBACK(on_object_added_or_removed), NULL);
    g_bt_context->properties_changed_handler_id = g_signal_connect(g_bt_context->object_manager, "interface-proxy-properties-changed", G_CALLBACK(on_properties_changed), NULL);
    refresh_and_notify(); return TRUE;
}

void bluetooth_manager_shutdown() {
    if (!g_bt_context) return;
    if (g_bt_context->object_manager) {
        g_signal_handler_disconnect(g_bt_context->object_manager, g_bt_context->object_added_handler_id);
        g_signal_handler_disconnect(g_bt_context->object_manager, g_bt_context->object_removed_handler_id);
        g_signal_handler_disconnect(g_bt_context->object_manager, g_bt_context->properties_changed_handler_id);
        g_object_unref(g_bt_context->object_manager);
    }
    g_free(g_bt_context); g_bt_context = NULL;
}

// --- Discovery & Power ---
void bluetooth_manager_start_discovery() { g_autofree gchar *p = find_adapter_path(); if (!p) return; g_autoptr(GDBusProxy) proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, BLUEZ_DBUS_SERVICE, p, BLUEZ_ADAPTER_INTERFACE, NULL, NULL); if (proxy) g_dbus_proxy_call(proxy, "StartDiscovery", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL); }
void bluetooth_manager_stop_discovery() { g_autofree gchar *p = find_adapter_path(); if (!p) return; g_autoptr(GDBusProxy) proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, BLUEZ_DBUS_SERVICE, p, BLUEZ_ADAPTER_INTERFACE, NULL, NULL); if (proxy) g_dbus_proxy_call(proxy, "StopDiscovery", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL); }
gboolean is_bluetooth_powered() { g_autofree gchar *p = find_adapter_path(); if (!p) return FALSE; g_autoptr(GDBusProxy) proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, BLUEZ_DBUS_SERVICE, p, BLUEZ_ADAPTER_INTERFACE, NULL, NULL); if (!proxy) return FALSE; g_autoptr(GVariant) prop = g_dbus_proxy_get_cached_property(proxy, "Powered"); return prop ? g_variant_get_boolean(prop) : FALSE; }
static void on_async_op_finished(GObject *s, GAsyncResult *res, gpointer d) { (void)s; FinishData *fd = d; gboolean success = g_task_propagate_boolean(G_TASK(res), NULL); g_print("[BT DEBUG] on_async_op_finished called. Success: %s\n", success ? "TRUE" : "FALSE"); if (fd->cb) fd->cb(success, fd->ud); g_free(fd); }
static void set_powered_thread(GTask *t, gpointer s, gpointer d, GCancellable *c) { (void)s; (void)c; (void)d; OperationData *data = g_task_get_task_data(t); g_autofree gchar *p = find_adapter_path(); if (!p) { g_task_return_boolean(t, FALSE); return; } g_autoptr(GDBusProxy) proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, BLUEZ_DBUS_SERVICE, p, "org.freedesktop.DBus.Properties", NULL, NULL); if (!proxy) { g_task_return_boolean(t, FALSE); return; } g_autoptr(GError) error = NULL; g_dbus_proxy_call_sync(proxy, "Set", g_variant_new("(ssv)", BLUEZ_ADAPTER_INTERFACE, "Powered", g_variant_new_boolean(data->powered)), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error); g_task_return_boolean(t, error == NULL); }
void set_bluetooth_powered_async(gboolean powered, BluetoothOperationCallback cb, gpointer ud) { OperationData *td = g_new0(OperationData, 1); td->powered = powered; FinishData *fd = g_new0(FinishData, 1); fd->cb = cb; fd->ud = ud; GTask *t = g_task_new(NULL, NULL, on_async_op_finished, fd); g_task_set_task_data(t, td, operation_data_free); g_task_run_in_thread(t, set_powered_thread); g_object_unref(t); }

// --- Get Devices (from cache) ---
static gint sort_devices(gconstpointer a, gconstpointer b) { const BluetoothDevice *da=a, *db=b; if (da->is_connected && !db->is_connected) return -1; if (!da->is_connected && db->is_connected) return 1; return g_strcmp0(da->name, db->name); }
GList* get_available_bluetooth_devices() { if (!g_bt_context || !g_bt_context->object_manager) return NULL; GList *devices = NULL; g_autoptr(GList) objects = g_dbus_object_manager_get_objects(g_bt_context->object_manager); for (GList *l = objects; l != NULL; l = l->next) { g_autoptr(GDBusInterface) iface = g_dbus_object_get_interface(l->data, BLUEZ_DEVICE_INTERFACE); if (iface) { BluetoothDevice *dev=g_new0(BluetoothDevice,1); dev->object_path=g_strdup(g_dbus_object_get_object_path(l->data)); g_autoptr(GVariant) name=g_dbus_proxy_get_cached_property(G_DBUS_PROXY(iface),"Name"), addr=g_dbus_proxy_get_cached_property(G_DBUS_PROXY(iface),"Address"), conn=g_dbus_proxy_get_cached_property(G_DBUS_PROXY(iface),"Connected"); dev->name = name ? g_variant_dup_string(name,NULL) : g_strdup("Unknown"); dev->address = addr ? g_variant_dup_string(addr,NULL) : g_strdup("??:"); dev->is_connected = conn ? g_variant_get_boolean(conn) : FALSE; devices = g_list_prepend(devices,dev); } } return g_list_sort(devices, sort_devices); }

// --- Connect/Disconnect ---
static void connect_thread(GTask *t, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)c; (void)d;
    OperationData *data = g_task_get_task_data(t);
    g_print("[BT DEBUG] connect_thread started for address: %s\n", data->address);

    g_autofree gchar *device_path = find_device_path_for_address(data->address);
    if (!device_path) {
        g_warning("[BT WARNING] connect_thread: FAILED to find device path for address: %s\n", data->address);
        g_task_return_boolean(t, FALSE);
        return;
    }
    g_print("[BT DEBUG] connect_thread: Found device path: %s\n", device_path);

    g_autoptr(GDBusInterface) iface = g_dbus_object_manager_get_interface(g_bt_context->object_manager, device_path, BLUEZ_DEVICE_INTERFACE);
    if (!iface) {
        g_warning("[BT WARNING] connect_thread: FAILED to get D-Bus interface for path: %s\n", device_path);
        g_task_return_boolean(t, FALSE);
        return;
    }
    g_print("[BT DEBUG] connect_thread: Got D-Bus interface successfully.\n");

    g_autoptr(GError) error = NULL;
    g_dbus_proxy_call_sync(G_DBUS_PROXY(iface), "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

    if (error) {
        g_warning("[BT WARNING] connect_thread: D-Bus Connect() call FAILED: %s\n", error->message);
        g_task_return_boolean(t, FALSE);
    } else {
        g_print("[BT DEBUG] connect_thread: D-Bus Connect() call SUCCEEDED.\n");
        g_task_return_boolean(t, TRUE);
    }
}

static void disconnect_thread(GTask *t, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)c; (void)d;
    OperationData *data = g_task_get_task_data(t);
    g_print("[BT DEBUG] disconnect_thread started for address: %s\n", data->address);

    g_autofree gchar *device_path = find_device_path_for_address(data->address);
    if (!device_path) {
        g_warning("[BT WARNING] disconnect_thread: FAILED to find device path for address: %s\n", data->address);
        g_task_return_boolean(t, FALSE);
        return;
    }
    g_print("[BT DEBUG] disconnect_thread: Found device path: %s\n", device_path);

    g_autoptr(GDBusInterface) iface = g_dbus_object_manager_get_interface(g_bt_context->object_manager, device_path, BLUEZ_DEVICE_INTERFACE);
    if (!iface) {
        g_warning("[BT WARNING] disconnect_thread: FAILED to get D-Bus interface for path: %s\n", device_path);
        g_task_return_boolean(t, FALSE);
        return;
    }
    g_print("[BT DEBUG] disconnect_thread: Got D-Bus interface successfully.\n");

    g_autoptr(GError) error = NULL;
    g_dbus_proxy_call_sync(G_DBUS_PROXY(iface), "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    
    if (error) {
        g_warning("[BT WARNING] disconnect_thread: D-Bus Disconnect() call FAILED: %s\n", error->message);
        g_task_return_boolean(t, FALSE);
    } else {
        g_print("[BT DEBUG] disconnect_thread: D-Bus Disconnect() call SUCCEEDED.\n");
        g_task_return_boolean(t, TRUE);
    }
}

void connect_to_bluetooth_device_async(const gchar *address, BluetoothOperationCallback cb, gpointer ud) {
    g_print("[BT DEBUG] connect_to_bluetooth_device_async called for %s\n", address);
    OperationData *td = g_new0(OperationData, 1); td->address = g_strdup(address);
    FinishData *fd = g_new0(FinishData, 1); fd->cb = cb; fd->ud = ud;
    GTask *t = g_task_new(NULL, NULL, on_async_op_finished, fd);
    g_task_set_task_data(t, td, operation_data_free);
    g_task_run_in_thread(t, connect_thread);
    g_object_unref(t);
}

void disconnect_bluetooth_device_async(const gchar *address, BluetoothOperationCallback cb, gpointer ud) {
    g_print("[BT DEBUG] disconnect_bluetooth_device_async called for %s\n", address);
    OperationData *td = g_new0(OperationData, 1); td->address = g_strdup(address);
    FinishData *fd = g_new0(FinishData, 1); fd->cb = cb; fd->ud = ud;
    GTask *t = g_task_new(NULL, NULL, on_async_op_finished, fd);
    g_task_set_task_data(t, td, operation_data_free);
    g_task_run_in_thread(t, disconnect_thread);
    g_object_unref(t);
}

// --- Memory Management ---
void bluetooth_device_free(gpointer data) { if (!data) return; BluetoothDevice *dev = (BluetoothDevice*)data; g_free(dev->address); g_free(dev->name); g_free(dev->object_path); g_free(dev); }
void free_bluetooth_device_list(GList *list) { g_list_free_full(list, bluetooth_device_free); }