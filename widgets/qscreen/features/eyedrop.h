#ifndef QSCREEN_EYEDROP_H
#define QSCREEN_EYEDROP_H

#include <gdk/gdk.h>
#include <glib.h>

// Extracts the color at a specific pixel in the screenshot.
// Populates out_color with the RGBA values and optionally allocates a Hex string in out_hex.
// Returns TRUE if successful.
gboolean eyedrop_get_color_at_pixel(GdkPixbuf *pixbuf, int x, int y, GdkRGBA *out_color, gchar **out_hex);

#endif // QSCREEN_EYEDROP_H