// widgets/control-center/src/network_manager.c

#include "network_manager.h"
#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h> // Required for 'bool'
#include <stdint.h>  // Required for 'uint32_t'

// --- Types from Rust ---
typedef struct {
    char *ssid;
    char *object_path;
    unsigned char strength;
    bool is_secure;
    bool is_active;
    bool is_known;
    uint32_t connectivity_state;
} RsWifiNetwork;

typedef struct RsContext RsContext;

// --- Typedefs for Rust FFI ---
// Rust expects a callback taking 'bool', but GLib's NetworkOperationCallback takes 'gboolean' (int).
// We define this specific type to match the Rust side exactly.
typedef void (*RsAsyncCallback)(bool success, void *user_data);

// --- Imports from Rust Library ---
extern RsContext* nm_rust_init();
extern void nm_rust_free(RsContext *ctx);
extern bool nm_rust_is_wifi_enabled(RsContext *ctx);
extern void nm_rust_set_wifi_enabled(RsContext *ctx, bool enabled);
extern RsWifiNetwork* nm_rust_get_networks(RsContext *ctx, size_t *count);
extern void nm_rust_free_networks(RsWifiNetwork *ptr, size_t count);

// Updated externs to use the correct RsAsyncCallback type
extern void nm_rust_disconnect(RsContext *ctx, RsAsyncCallback cb, void *ud);
extern void nm_rust_activate_existing(RsContext *ctx, const char *conn, const char *ap, RsAsyncCallback cb, void *ud);
extern void nm_rust_add_and_activate(RsContext *ctx, const char *ssid, const char *ap, const char *pass, bool secure, RsAsyncCallback cb, void *ud);
extern void nm_rust_forget_connection(RsContext *ctx, const char *ssid, RsAsyncCallback cb, void *ud);

extern char* nm_rust_find_connection_path(RsContext *ctx, const char *ssid);
extern void nm_rust_free_string(char *ptr);

// --- Global Context ---
static RsContext *g_rs_ctx = NULL;

// --- Helper Struct for Callbacks ---
typedef struct {
    NetworkOperationCallback cb;
    gpointer ud;
} RustCbData;

// --- Internal Callback Bridge ---
// This function matches RsAsyncCallback (bool).
// It converts the bool to gboolean (int) and calls the actual UI callback.
static void on_rust_operation_done(bool success, void *user_data) {
    RustCbData *data = (RustCbData*)user_data;
    if (data->cb) {
        data->cb(success ? TRUE : FALSE, data->ud);
    }
    g_free(data);
}

// --- API Implementation ---

gboolean network_manager_init() {
    if (g_rs_ctx) return TRUE;
    g_rs_ctx = nm_rust_init();
    return (g_rs_ctx != NULL);
}

void network_manager_shutdown() {
    if (g_rs_ctx) {
        nm_rust_free(g_rs_ctx);
        g_rs_ctx = NULL;
    }
}

gboolean is_wifi_enabled() {
    if (!g_rs_ctx) return FALSE;
    return nm_rust_is_wifi_enabled(g_rs_ctx);
}

void set_wifi_enabled_async(gboolean enabled, NetworkOperationCallback cb, gpointer ud) {
    if (!g_rs_ctx) return;
    nm_rust_set_wifi_enabled(g_rs_ctx, (bool)enabled);
    if (cb) cb(TRUE, ud);
}

GList* get_available_wifi_networks() {
    if (!g_rs_ctx) return NULL;
    
    size_t count = 0;
    RsWifiNetwork *raw_list = nm_rust_get_networks(g_rs_ctx, &count);
    if (!raw_list) return NULL;

    GList *list = NULL;
    for (size_t i = 0; i < count; i++) {
        WifiNetwork *net = g_new0(WifiNetwork, 1);
        net->ssid = g_strdup(raw_list[i].ssid);
        net->object_path = g_strdup(raw_list[i].object_path);
        net->strength = raw_list[i].strength;
        net->is_secure = raw_list[i].is_secure;
        net->is_active = raw_list[i].is_active;
        net->is_known = raw_list[i].is_known;
        
        // Connectivity: 4=Full, >1=Limited, else=Connecting
        if (net->is_active) {
            if (raw_list[i].connectivity_state == 4) net->connectivity = WIFI_STATE_CONNECTED;
            else if (raw_list[i].connectivity_state > 1) net->connectivity = WIFI_STATE_LIMITED;
            else net->connectivity = WIFI_STATE_CONNECTING;
        } else {
            net->connectivity = WIFI_STATE_DISCONNECTED;
        }
        
        list = g_list_append(list, net);
    }

    nm_rust_free_networks(raw_list, count);
    return list;
}

void disconnect_wifi_async(NetworkOperationCallback cb, gpointer ud) {
    if (!g_rs_ctx) return;
    RustCbData *data = g_new(RustCbData, 1);
    data->cb = cb; data->ud = ud;
    // Now passing on_rust_operation_done (bool) to a param expecting RsAsyncCallback (bool). Correct.
    nm_rust_disconnect(g_rs_ctx, on_rust_operation_done, data);
}

void activate_wifi_connection_async(const gchar *connection_path, const gchar *ap_path, NetworkOperationCallback cb, gpointer ud) {
    if (!g_rs_ctx) return;
    RustCbData *data = g_new(RustCbData, 1);
    data->cb = cb; data->ud = ud;
    nm_rust_activate_existing(g_rs_ctx, connection_path, ap_path, on_rust_operation_done, data);
}

void add_and_activate_wifi_connection_async(const gchar *ssid, const gchar *ap_path, const gchar *password, gboolean is_secure, NetworkOperationCallback cb, gpointer ud) {
    if (!g_rs_ctx) return;
    RustCbData *data = g_new(RustCbData, 1);
    data->cb = cb; data->ud = ud;
    nm_rust_add_and_activate(g_rs_ctx, ssid, ap_path, password, (bool)is_secure, on_rust_operation_done, data);
}

void forget_wifi_connection_async(const gchar *ssid, NetworkOperationCallback cb, gpointer ud) {
    if (!g_rs_ctx) return;
    RustCbData *data = g_new(RustCbData, 1);
    data->cb = cb; data->ud = ud;
    nm_rust_forget_connection(g_rs_ctx, ssid, on_rust_operation_done, data);
}

gchar* find_connection_for_ssid(const gchar *ssid) {
    if (!g_rs_ctx) return NULL;
    char *path = nm_rust_find_connection_path(g_rs_ctx, ssid);
    if (!path) return NULL;
    gchar *res = g_strdup(path);
    nm_rust_free_string(path);
    return res;
}

void wifi_network_free(gpointer data) {
    if (!data) return;
    WifiNetwork *net = (WifiNetwork *)data;
    g_free(net->ssid);
    g_free(net->object_path);
    g_free(net);
}

void free_wifi_network_list(GList *list) {
    g_list_free_full(list, wifi_network_free);
}

// Stubs for functions not yet ported to Rust
void get_wifi_network_details_async(const gchar *ap_path, WifiDetailsCallback cb, gpointer ud) {
    (void)ap_path;
    if(cb) cb(NULL, ud); 
}

gboolean is_airplane_mode_active() { return FALSE; }

gboolean is_connection_forgettable(const gchar *path, gboolean secure) {
    (void)path; (void)secure;
    return TRUE;
}

void free_wifi_network_details(WifiNetworkDetails *d) {
    if(d) {
        g_free(d->ssid);
        g_free(d->security);
        g_free(d->ip_address);
        g_free(d->mac_address);
        g_free(d);
    }
}