#ifndef QSCREEN_ANNOTATION_H
#define QSCREEN_ANNOTATION_H

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <cairo.h>

typedef enum {
    ANNOTATION_STROKE,
    ANNOTATION_TEXT,
    ANNOTATION_RECTANGLE,
    ANNOTATION_CIRCLE,
    ANNOTATION_ARROW,
    ANNOTATION_PIXELATE,
    ANNOTATION_IMAGE 
} AnnotationType;

typedef struct {
    double x, y;
} AnnotationPoint;

typedef struct {
    AnnotationType type;
    GdkRGBA color;
    double line_width;
    double rotation;
    
    GArray *points;
    char *text;
    double font_size;
    
    GdkPixbuf *pixbuf; 
    
    // --- NEW: Hardware/Memory Caching for 144fps dragging ---
    cairo_surface_t *cached_surface;
    int cached_x, cached_y, cached_w, cached_h;
    double cached_line_width;
    
    double x, y;
    double x2, y2;
} AnnotationItem;

AnnotationItem* annotation_stroke_new(const GdkRGBA *color, double line_width);
void annotation_stroke_add_point(AnnotationItem *item, double x, double y);

AnnotationItem* annotation_text_new(const GdkRGBA *color, const char *text, double x, double y, double font_size);

AnnotationItem* annotation_shape_new(AnnotationType type, const GdkRGBA *color, double line_width, double x, double y);
void annotation_shape_update(AnnotationItem *item, double x2, double y2);

AnnotationItem* annotation_image_new(GdkPixbuf *pixbuf, double x, double y);

void annotation_item_free(gpointer data);
void annotation_items_free_list(GList *items);

void annotation_item_get_bounds(AnnotationItem *item, double *bx, double *by, double *bw, double *bh);
void annotation_item_translate(AnnotationItem *item, double dx, double dy);

void annotation_draw_all(cairo_t *cr, GList *items, GdkPixbuf *bg, AnnotationItem *selected_item, double offset_x, double offset_y, double zoom_x, double zoom_y);
gboolean annotation_save_composite(GdkPixbuf *bg, GdkRectangle *crop, GList *items, const char *output_png_path);

#endif // QSCREEN_ANNOTATION_H