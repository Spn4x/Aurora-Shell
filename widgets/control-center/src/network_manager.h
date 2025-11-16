
#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <glib.h>

// --- Init and Shutdown ---
gboolean network_manager_init();
void network_manager_shutdown();

// --- Type Definitions ---
typedef void (*NetworkOperationCallback)(gboolean success, gpointer user_data);

typedef enum {
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_LIMITED,
    WIFI_STATE_CONNECTED
} WifiConnectivityState;

typedef struct {
    gchar *ssid;
    gchar *object_path;
    guint8 strength;
    gboolean is_secure;
    gboolean is_active;
    gboolean is_known;
    WifiConnectivityState connectivity;
} WifiNetwork;

typedef struct {
    gchar *ssid;
    gchar *security;
    guint8 strength;
    gchar *ip_address;
    gchar *mac_address;
} WifiNetworkDetails;

typedef void (*WifiDetailsCallback)(WifiNetworkDetails *details, gpointer user_data);

// --- Radio Control ---
gboolean is_wifi_enabled();
void set_wifi_enabled_async(gboolean enabled, NetworkOperationCallback callback, gpointer user_data);
gboolean is_airplane_mode_active();

// --- Network Operations ---
GList* get_available_wifi_networks();

void get_wifi_network_details_async(const gchar *ap_path, WifiDetailsCallback callback, gpointer user_data);

gchar* find_connection_for_ssid(const gchar *ssid);
gboolean is_connection_forgettable(const gchar *connection_path, gboolean is_network_secure);

void activate_wifi_connection_async(const gchar *connection_path,
                                    const gchar *ap_path,
                                    NetworkOperationCallback callback,
                                    gpointer user_data);

// This function now correctly handles prompting for a password when `password` is NULL.
void add_and_activate_wifi_connection_async(const gchar *ssid,
                                            const gchar *ap_path,
                                            const gchar *password, // Can be NULL to trigger a prompt
                                            gboolean is_secure,
                                            NetworkOperationCallback callback,
                                            gpointer user_data);
                                   
void forget_wifi_connection_async(const gchar *ssid,
                                  NetworkOperationCallback callback,
                                  gpointer user_data);

void disconnect_wifi_async(NetworkOperationCallback callback,
                           gpointer user_data);

// --- Memory Management ---
void wifi_network_free(gpointer data);
void free_wifi_network_list(GList *list);
void free_wifi_network_details(WifiNetworkDetails *details);

#endif // NETWORK_MANAGER_H