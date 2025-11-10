#ifndef BLUETOOTH_SCANNER_H
#define BLUETOOTH_SCANNER_H

#include <glib.h>
#include "bluetooth_manager.h"

// Callback function prototype: it will be called with a fresh list of devices.
// The receiver of this callback is responsible for freeing the GList.
typedef void (*BluetoothScanResultCallback)(GList *devices, gpointer user_data);

typedef struct _BluetoothScanner BluetoothScanner;

// Creates a new BluetoothScanner object.
BluetoothScanner* bluetooth_scanner_new(BluetoothScanResultCallback callback, gpointer user_data);

// Starts device discovery.
void bluetooth_scanner_start(BluetoothScanner *scanner);

// Stops device discovery.
void bluetooth_scanner_stop(BluetoothScanner *scanner);

// Triggers an immediate refresh of the UI from the manager's cache.
void bluetooth_scanner_trigger_scan(BluetoothScanner *scanner);

// Frees the scanner object.
void bluetooth_scanner_free(BluetoothScanner *scanner);

#endif // BLUETOOTH_SCANNER_H