// ===== src/bluetooth_scanner.c =====
#include "bluetooth_scanner.h"
#include "bluetooth_manager.h"

struct _BluetoothScanner {
    // The scanner no longer manages timers, only the callback.
    BluetoothScanResultCallback callback;
    gpointer user_data;
};

BluetoothScanner* bluetooth_scanner_new(BluetoothScanResultCallback callback, gpointer user_data) {
    BluetoothScanner *scanner = g_new0(BluetoothScanner, 1);
    scanner->callback = callback;
    scanner->user_data = user_data;
    return scanner;
}

void bluetooth_scanner_start(BluetoothScanner *scanner) {
    g_print("Starting Bluetooth discovery...\n");
    bluetooth_manager_start_discovery();
    // Trigger one scan immediately to populate the UI from the cache
    bluetooth_scanner_trigger_scan(scanner);
}

void bluetooth_scanner_stop(BluetoothScanner *scanner) {
    (void)scanner;
    g_print("Stopping Bluetooth discovery...\n");
    bluetooth_manager_stop_discovery();
}

void bluetooth_scanner_trigger_scan(BluetoothScanner *scanner) {
    if (!scanner || !scanner->callback) {
        return;
    }
    // This no longer triggers a hardware scan. It just gets the latest
    // device list from the manager's cache and sends it to the UI.
    GList *devices = get_available_bluetooth_devices();
    scanner->callback(devices, scanner->user_data);
}

void bluetooth_scanner_free(BluetoothScanner *scanner) {
    if (!scanner) return;
    bluetooth_scanner_stop(scanner);
    g_free(scanner);
}