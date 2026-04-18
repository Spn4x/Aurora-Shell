#include "eyedrop.h"

gboolean eyedrop_get_color_at_pixel(GdkPixbuf *pixbuf, int x, int y, GdkRGBA *out_color, gchar **out_hex) {
    if (!pixbuf) return FALSE;
    
    int width = gdk_pixbuf_get_width(pixbuf);
    int height = gdk_pixbuf_get_height(pixbuf);
    
    // Boundary check
    if (x < 0 || x >= width || y < 0 || y >= height) {
        return FALSE;
    }

    int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    int n_channels = gdk_pixbuf_get_n_channels(pixbuf);
    guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);
    
    // Calculate the exact pixel position in memory
    guchar *p = pixels + y * rowstride + x * n_channels;

    if (out_color) {
        out_color->red = p[0] / 255.0;
        out_color->green = p[1] / 255.0;
        out_color->blue = p[2] / 255.0;
        out_color->alpha = (n_channels == 4) ? (p[3] / 255.0) : 1.0;
    }

    if (out_hex) {
        // Format to standard upper-case hex code
        *out_hex = g_strdup_printf("#%02X%02X%02X", p[0], p[1], p[2]);
    }

    return TRUE;
}