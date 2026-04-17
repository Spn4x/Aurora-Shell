#ifndef QSCREEN_ANNOTATION_H
#define QSCREEN_ANNOTATION_H

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <cairo.h>

typedef enum {
    ANNOTATION_STROKE,
    ANNOTATION_TEXT,
    ANNOTATION_RECTANGLE,
    ANNOTATION_CIRCLE
} AnnotationType;

typedef struct {
    double x, y;
} AnnotationPoint;

typedef struct {
    AnnotationType type;
    GdkRGBA color;
    double line_width;
    
    // For freehand strokes
    GArray *points;
    
    // For text
    char *text;
    double font_size;
    
    // For Shapes & Text Origin
    double x, y;
    double x2, y2;
} AnnotationItem;

AnnotationItem* annotation_stroke_new(const GdkRGBA *color, double line_width);
void annotation_stroke_add_point(AnnotationItem *item, double x, double y);

AnnotationItem* annotation_text_new(const GdkRGBA *color, const char *text, double x, double y, double font_size);

AnnotationItem* annotation_shape_new(AnnotationType type, const GdkRGBA *color, double line_width, double x, double y);
void annotation_shape_update(AnnotationItem *item, double x2, double y2);

void annotation_item_free(gpointer data);
void annotation_items_free_list(GList *items);

void annotation_draw_all(cairo_t *cr, GList *items, double offset_x, double offset_y, double zoom_x, double zoom_y);
gboolean annotation_save_composite(GdkPixbuf *bg, GdkRectangle *crop, GList *items, const char *output_png_path);

#endif // QSCREEN_ANNOTATION_H