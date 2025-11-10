// ===== widgets/control-center/src/network_manager.h (COMPLETE) =====
#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <glib.h>

// --- Init and Shutdown ---
gboolean network_manager_init();
void network_manager_shutdown();

// --- Type Definitions ---
typedef void (*NetworkOperationCallback)(gboolean success, gpointer user_data);

// --- ADD THIS NEW ENUM ---
typedef enum {
    WIFI_STATE_DISCONNECTED,      // Not connected, but might be known/saved
    WIFI_STATE_CONNECTING,        // In the process of connecting
    WIFI_STATE_LIMITED,           // Connected to router, but no internet access
    WIFI_STATE_CONNECTED          // Fully connected with internet access
} WifiConnectivityState;
// --- END NEW ENUM ---

typedef struct {
    gchar *ssid;
    gchar *object_path; // D-Bus object path of the Access Point
    guint8 strength;
    gboolean is_secure;
    gboolean is_active;
    gboolean is_known; // Is this a saved network profile?
    
    // --- ADD THIS NEW FIELD ---
    WifiConnectivityState connectivity;

} WifiNetwork;

typedef struct {
    gchar *ssid;
    gchar *security;
    guint8 strength;
    gchar *ip_address;
    gchar *mac_address;
} WifiNetworkDetails;

// NEW: Callback for the async details function
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

void add_and_activate_wifi_connection_async(const gchar *ssid,
                                            const gchar *ap_path,
                                            const gchar *password,
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