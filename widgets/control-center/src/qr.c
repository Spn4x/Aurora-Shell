#include "qr.h"
#include <qrencode.h>
#include <cairo.h>
#include "utils.h"

// D-Bus constants
#define NM_DBUS_SERVICE "org.freedesktop.NetworkManager"
#define NM_SETTINGS_PATH "/org/freedesktop/NetworkManager/Settings"
#define NM_SETTINGS_INTERFACE "org.freedesktop.NetworkManager.Settings"
#define NM_SETTINGS_CONNECTION_INTERFACE "org.freedesktop.NetworkManager.Settings.Connection"

// Data for our background task
typedef struct {
    gchar *ssid;
    WifiQRCodeCallback user_callback;
    gpointer user_data;
} QRTaskData;

// Helper to find the D-Bus path of a saved connection by its SSID
static gchar* find_connection_path_for_ssid(GDBusConnection *bus, const gchar *ssid, GError **error) {
    g_autoptr(GVariant) connections_variant = g_dbus_connection_call_sync(bus,
        NM_DBUS_SERVICE, NM_SETTINGS_PATH, NM_SETTINGS_INTERFACE, "ListConnections",
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
    if (*error) return NULL;

    g_autoptr(GVariantIter) iter;
    g_variant_get(connections_variant, "(ao)", &iter);
    const gchar *conn_path;
    while (g_variant_iter_loop(iter, "o", &conn_path)) {
        g_autoptr(GVariant) settings_variant = g_dbus_connection_call_sync(bus,
            NM_DBUS_SERVICE, conn_path, NM_SETTINGS_CONNECTION_INTERFACE, "GetSettings",
            NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
        if (!settings_variant) continue;

        g_autoptr(GVariant) settings_dict = g_variant_get_child_value(settings_variant, 0);
        g_autoptr(GVariant) wifi_settings = g_variant_lookup_value(settings_dict, "802-11-wireless", G_VARIANT_TYPE("a{sv}"));
        if (!wifi_settings) continue;

        g_autoptr(GVariant) ssid_variant = g_variant_lookup_value(wifi_settings, "ssid", G_VARIANT_TYPE("ay"));
        if (ssid_variant) {
            gsize len;
            const gchar *ssid_bytes = g_variant_get_fixed_array(ssid_variant, &len, sizeof(guint8));
            if (len == strlen(ssid) && memcmp(ssid_bytes, ssid, len) == 0) {
                return g_strdup(conn_path);
            }
        }
    }
    return NULL;
}

// Helper to convert a qrencode object to a GdkPixbuf
static GdkPixbuf* qrcode_to_pixbuf(const QRcode *qrcode) {
    if (!qrcode) return NULL;
    const int scale = 8;
    const int margin = 2; // 2 modules of white space around the QR code
    int size = (qrcode->width + margin * 2) * scale;

    // Create a full-color ARGB surface directly. No more A8 mask.
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surface);

    // 1. Fill the entire background with white.
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    // 2. Set the drawing color to black for the QR code modules.
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);

    // 3. Loop through the QR data and draw the black squares.
    unsigned char *p = qrcode->data;
    for (int y = 0; y < qrcode->width; y++) {
        for (int x = 0; x < qrcode->width; x++) {
            if (*p & 1) { // If the bit is 1, it's a black module
                // The coordinates are shifted by the margin to center the code.
                cairo_rectangle(cr, (x + margin) * scale, (y + margin) * scale, scale, scale);
            }
            p++;
        }
    }
    // Fill all the rectangles we defined in one go. This is more efficient.
    cairo_fill(cr);

    // Convert the final, correctly drawn surface to a pixbuf.
    GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, size, size);

    // Clean up Cairo resources.
    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    return pixbuf;
}

// Main background thread logic
static void qr_code_thread_func(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object; (void)cancellable;
    QRTaskData *data = task_data;
    g_autoptr(GdkPixbuf) result_pixbuf = NULL;
    
    g_print("[QR BACKEND] Using nmcli to fetch password for: '%s'\n", data->ssid);

    // Use nmcli to get the PSK directly. 
    // -s: show secrets, -g: get specifically the psk field
    gchar *cmd = g_strdup_printf("nmcli -s -g 802-11-wireless-security.psk connection show \"%s\"", data->ssid);
    gchar *password = run_command(cmd); // This uses your existing utility!
    g_free(cmd);

    if (password) {
        g_strstrip(password); 
    }

    gchar *qr_string = NULL;
    if (password && strlen(password) > 0) {
        g_print("[QR BACKEND] -> Password found successfully.\n");
        // Most routers use WPA/WPA2, so T:WPA is the safest standard for the QR
        qr_string = g_strdup_printf("WIFI:S:%s;T:WPA;P:%s;;", data->ssid, password);
    } else {
        g_print("[QR BACKEND] -> No password found (Open network).\n");
        qr_string = g_strdup_printf("WIFI:S:%s;T:nopass;;", data->ssid);
    }

    // Generate the QR code as you were doing before
    QRcode *qrcode = QRcode_encodeString(qr_string, 0, QR_ECLEVEL_L, QR_MODE_8, 1);
    result_pixbuf = qrcode_to_pixbuf(qrcode);
    
    QRcode_free(qrcode);
    g_free(qr_string);
    g_free(password);

    if (result_pixbuf) {
        g_task_return_pointer(task, g_object_ref(result_pixbuf), g_object_unref);
    } else {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "QR generation failed");
    }
}

// Main thread callback
static void on_qr_code_finished(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object;
    QRTaskData *data = user_data;
    g_autoptr(GError) error = NULL;
    
    GdkPixbuf *pixbuf = g_task_propagate_pointer(G_TASK(res), &error);
    
    if (error) {
        g_warning("Failed to generate QR code: %s", error->message);
        data->user_callback(NULL, data->user_data);
    } else {
        data->user_callback(pixbuf, data->user_data);
        if(pixbuf) g_object_unref(pixbuf);
    }
    
    g_free(data->ssid);
    g_free(data);
}

// Public API function
void generate_wifi_qr_code_async(const gchar *ssid, WifiQRCodeCallback callback, gpointer user_data) {
    QRTaskData *data = g_new0(QRTaskData, 1);
    data->ssid = g_strdup(ssid);
    data->user_callback = callback;
    data->user_data = user_data;
    
    GTask *task = g_task_new(NULL, NULL, on_qr_code_finished, data);
    g_task_set_task_data(task, data, NULL);
    g_task_run_in_thread(task, qr_code_thread_func);
    g_object_unref(task);
}