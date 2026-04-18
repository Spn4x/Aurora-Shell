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
    if (item->type == ANNOTATION_RECTANGLE || item->type == ANNOTATION_CIRCLE || item->type == ANNOTATION_ARROW || item->type == ANNOTATION_PIXELATE) {
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

// Helper for drawing the arrow head
static void draw_arrow_head(cairo_t *cr, double x2, double y2, double angle, double size) {
    cairo_save(cr);
    cairo_translate(cr, x2, y2);
    cairo_rotate(cr, angle);
    
    cairo_new_path(cr);
    cairo_move_to(cr, 0, 0);
    cairo_line_to(cr, -size, size * 0.5);
    cairo_line_to(cr, -size, -size * 0.5);
    cairo_close_path(cr);
    
    cairo_fill(cr);
    cairo_restore(cr);
}

// UPDATE: Added bg parameter
void annotation_draw_all(cairo_t *cr, GList *items, GdkPixbuf *bg, double offset_x, double offset_y, double zoom_x, double zoom_y) {
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    for (GList *l = items; l != NULL; l = l->next) {
        AnnotationItem *item = l->data;
        
        cairo_new_path(cr); 
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
        else if (item->type == ANNOTATION_PIXELATE && bg != NULL) {
            // Unscaled original image coordinates
            double px = MIN(item->x, item->x2);
            double py = MIN(item->y, item->y2);
            double pw = fabs(item->x2 - item->x);
            double ph = fabs(item->y2 - item->y);
            
            // Bounds check against the actual image
            int bg_w = gdk_pixbuf_get_width(bg);
            int bg_h = gdk_pixbuf_get_height(bg);
            
            int ipx = MAX(0, (int)px);
            int ipy = MAX(0, (int)py);
            int ipw = MIN((int)pw, bg_w - ipx);
            int iph = MIN((int)ph, bg_h - ipy);
            
            if (ipw > 0 && iph > 0) {
                // Brush size acts as the chunk/block size!
                int block_size = MAX(4, (int)(item->line_width)); 
                int sw = MAX(1, ipw / block_size);
                int sh = MAX(1, iph / block_size);
                
                // 1. Extract region
                GdkPixbuf *sub = gdk_pixbuf_new_subpixbuf(bg, ipx, ipy, ipw, iph);
                // 2. Scale down (destroy detail)
                GdkPixbuf *down = gdk_pixbuf_scale_simple(sub, sw, sh, GDK_INTERP_NEAREST);
                // 3. Scale up with Nearest Neighbor (create sharp blocks)
                GdkPixbuf *up = gdk_pixbuf_scale_simple(down, ipw, iph, GDK_INTERP_NEAREST);
                
                cairo_save(cr);
                cairo_scale(cr, zoom_x, zoom_y);
                cairo_translate(cr, offset_x, offset_y);
                
                // Paint the pixelated block
                gdk_cairo_set_source_pixbuf(cr, up, ipx, ipy);
                cairo_rectangle(cr, ipx, ipy, ipw, iph);
                cairo_fill(cr);
                
                cairo_restore(cr);
                
                g_object_unref(up);
                g_object_unref(down);
                g_object_unref(sub);
            }
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
        else if (item->type == ANNOTATION_ARROW) {
            double actual_line_width = item->line_width * ((zoom_x + zoom_y) / 2.0);
            cairo_set_line_width(cr, actual_line_width);
            
            double x1 = (item->x + offset_x) * zoom_x;
            double y1 = (item->y + offset_y) * zoom_y;
            double x2 = (item->x2 + offset_x) * zoom_x;
            double y2 = (item->y2 + offset_y) * zoom_y;

            double dx = x2 - x1;
            double dy = y2 - y1;
            double len = sqrt(dx*dx + dy*dy);
            
            if (len > 0) {
                double angle = atan2(dy, dx);
                double head_size = actual_line_width * 4.0; 
                
                double shorten_by = fmin(len, head_size * 0.8);
                double line_end_x = x2 - cos(angle) * shorten_by;
                double line_end_y = y2 - sin(angle) * shorten_by;

                cairo_move_to(cr, x1, y1);
                cairo_line_to(cr, line_end_x, line_end_y);
                cairo_stroke(cr);

                draw_arrow_head(cr, x2, y2, angle, head_size);
            }
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

    // UPDATE: Passed bg into draw_all so pixelation works in the final render
    annotation_draw_all(cr, items, bg, -crop->x, -crop->y, 1.0, 1.0);

    cairo_status_t status = cairo_surface_write_to_png(surface, output_png_path);
    
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    
    return (status == CAIRO_STATUS_SUCCESS);
}