#ifndef BLUETOOTH_MANAGER_H
#define BLUETOOTH_MANAGER_H

#include <glib.h>

// Callback prototype for async operations.
typedef void (*BluetoothOperationCallback)(gboolean success, gpointer user_data);

// --- NEW: Callback for device list updates ---
// This will be called automatically when devices are found, disappear, or change state.
typedef void (*BluetoothDeviceUpdateCallback)(GList *devices, gpointer user_data);

// A struct to hold information about a single Bluetooth device.
typedef struct {
    gchar *address; // MAC address
    gchar *name;
    gboolean is_connected;
    gchar *object_path; // Internal D-Bus path
} BluetoothDevice;

// --- NEW: Core Manager Lifecycle & Discovery Control ---
gboolean bluetooth_manager_init(BluetoothDeviceUpdateCallback update_callback, gpointer user_data);
void bluetooth_manager_shutdown();
void bluetooth_manager_start_discovery();
void bluetooth_manager_stop_discovery();
// --- END NEW ---

// --- Power Control ---
gboolean is_bluetooth_powered();
void set_bluetooth_powered_async(gboolean powered, BluetoothOperationCallback callback, gpointer user_data);

// Gets a list of all paired/known and recently scanned Bluetooth devices.
// This is now a cheap, synchronous call that returns the manager's cached list.
GList* get_available_bluetooth_devices();

// Asynchronously attempts to connect to a Bluetooth device by its address.
void connect_to_bluetooth_device_async(const gchar *address,
                                       BluetoothOperationCallback callback,
                                       gpointer user_data);

// Asynchronously disconnects from a Bluetooth device by its address.
void disconnect_bluetooth_device_async(const gchar *address,
                                       BluetoothOperationCallback callback,
                                       gpointer user_data);

// Frees a single BluetoothDevice struct.
void bluetooth_device_free(gpointer data);

// Frees a GList of BluetoothDevice structs.
void free_bluetooth_device_list(GList *list);

#endif // BLUETOOTH_MANAGER_H