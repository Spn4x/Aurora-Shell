#include "annotation.h"
#include <math.h>

AnnotationItem* annotation_stroke_new(const GdkRGBA *color, double line_width) {
    AnnotationItem *item = g_new0(AnnotationItem, 1);
    item->type = ANNOTATION_STROKE;
    item->color = *color;
    item->line_width = line_width;
    item->points = g_array_new(FALSE, FALSE, sizeof(AnnotationPoint));
    return item;
}

void annotation_stroke_add_point(AnnotationItem *item, double x, double y) {
    if (item->type == ANNOTATION_STROKE) {
        AnnotationPoint pt = {x, y};
        g_array_append_val(item->points, pt);
    }
}

AnnotationItem* annotation_text_new(const GdkRGBA *color, const char *text, double x, double y, double font_size) {
    AnnotationItem *item = g_new0(AnnotationItem, 1);
    item->type = ANNOTATION_TEXT;
    item->color = *color;
    item->text = g_strdup(text);
    item->x = x;
    item->y = y;
    item->font_size = font_size;
    return item;
}

AnnotationItem* annotation_shape_new(AnnotationType type, const GdkRGBA *color, double line_width, double x, double y) {
    AnnotationItem *item = g_new0(AnnotationItem, 1);
    item->type = type;
    item->color = *color;
    item->line_width = line_width;
    item->x = x;
    item->y = y;
    item->x2 = x;
    item->y2 = y;
    return item;
}

void annotation_shape_update(AnnotationItem *item, double x2, double y2) {
    if (item->type == ANNOTATION_RECTANGLE || item->type == ANNOTATION_CIRCLE) {
        item->x2 = x2;
        item->y2 = y2;
    }
}

void annotation_item_free(gpointer data) {
    AnnotationItem *item = data;
    if (!item) return;
    if (item->type == ANNOTATION_STROKE && item->points) {
        g_array_free(item->points, TRUE);
    } else if (item->type == ANNOTATION_TEXT && item->text) {
        g_free(item->text);
    }
    g_free(item);
}

void annotation_items_free_list(GList *items) {
    g_list_free_full(items, annotation_item_free);
}

void annotation_draw_all(cairo_t *cr, GList *items, double offset_x, double offset_y, double zoom_x, double zoom_y) {
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    for (GList *l = items; l != NULL; l = l->next) {
        AnnotationItem *item = l->data;
        cairo_set_source_rgba(cr, item->color.red, item->color.green, item->color.blue, item->color.alpha);

        if (item->type == ANNOTATION_STROKE) {
            if (item->points->len < 2) continue;
            cairo_set_line_width(cr, item->line_width * ((zoom_x + zoom_y) / 2.0));
            AnnotationPoint *pts = (AnnotationPoint *)item->points->data;
            cairo_move_to(cr, (pts[0].x + offset_x) * zoom_x, (pts[0].y + offset_y) * zoom_y);
            for (guint i = 1; i < item->points->len; i++) {
                cairo_line_to(cr, (pts[i].x + offset_x) * zoom_x, (pts[i].y + offset_y) * zoom_y);
            }
            cairo_stroke(cr);
        } 
        else if (item->type == ANNOTATION_RECTANGLE) {
            cairo_set_line_width(cr, item->line_width * ((zoom_x + zoom_y) / 2.0));
            double rx = (item->x + offset_x) * zoom_x;
            double ry = (item->y + offset_y) * zoom_y;
            double rw = (item->x2 - item->x) * zoom_x;
            double rh = (item->y2 - item->y) * zoom_y;
            cairo_rectangle(cr, rx, ry, rw, rh);
            cairo_stroke(cr);
        }
        else if (item->type == ANNOTATION_CIRCLE) {
            cairo_set_line_width(cr, item->line_width * ((zoom_x + zoom_y) / 2.0));
            double rx = (item->x + offset_x) * zoom_x;
            double ry = (item->y + offset_y) * zoom_y;
            double rw = (item->x2 - item->x) * zoom_x;
            double rh = (item->y2 - item->y) * zoom_y;
            
            double cx = rx + rw / 2.0;
            double cy = ry + rh / 2.0;
            double radius_x = fabs(rw / 2.0);
            double radius_y = fabs(rh / 2.0);
            
            cairo_save(cr);
            cairo_translate(cr, cx, cy);
            if (radius_x > 0.0 && radius_y > 0.0) {
                cairo_scale(cr, radius_x, radius_y);
                cairo_arc(cr, 0, 0, 1.0, 0, 2 * G_PI);
            }
            cairo_restore(cr);
            cairo_stroke(cr);
        }
        else if (item->type == ANNOTATION_TEXT) {
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, item->font_size * zoom_y); 
            cairo_font_extents_t fe;
            cairo_font_extents(cr, &fe);
            cairo_move_to(cr, (item->x + offset_x) * zoom_x, (item->y + offset_y) * zoom_y + fe.ascent);
            cairo_show_text(cr, item->text);
        }
    }
}

gboolean annotation_save_composite(GdkPixbuf *bg, GdkRectangle *crop, GList *items, const char *output_png_path) {
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, crop->width, crop->height);
    cairo_t *cr = cairo_create(surface);

    gdk_cairo_set_source_pixbuf(cr, bg, -crop->x, -crop->y);
    cairo_paint(cr);

    annotation_draw_all(cr, items, -crop->x, -crop->y, 1.0, 1.0);

    cairo_status_t status = cairo_surface_write_to_png(surface, output_png_path);
    
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    
    return (status == CAIRO_STATUS_SUCCESS);
}