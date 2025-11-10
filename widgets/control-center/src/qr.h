#ifndef QR_H
#define QR_H

#include <gtk/gtk.h>

// Callback for when the QR code is ready (or has failed)
// The pixbuf will be NULL on failure. The caller owns the returned pixbuf.
typedef void (*WifiQRCodeCallback)(GdkPixbuf *qr_code, gpointer user_data);

// Asynchronously fetches credentials and generates a QR code for the given SSID.
void generate_wifi_qr_code_async(const gchar *ssid, WifiQRCodeCallback callback, gpointer user_data);

#endif // QR_H