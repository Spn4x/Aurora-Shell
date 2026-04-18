#include "annotation.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

AnnotationItem* annotation_stroke_new(const GdkRGBA *color, double line_width) {
    AnnotationItem *item = g_new0(AnnotationItem, 1);
    item->type = ANNOTATION_STROKE;
    item->color = *color;
    item->line_width = line_width;
    item->rotation = 0.0;
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
    item->rotation = 0.0;
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
    item->rotation = 0.0;
    return item;
}

AnnotationItem* annotation_image_new(GdkPixbuf *pixbuf, double x, double y) {
    AnnotationItem *item = g_new0(AnnotationItem, 1);
    item->type = ANNOTATION_IMAGE;
    item->pixbuf = pixbuf;
    item->x = x;
    item->y = y;
    item->x2 = x + gdk_pixbuf_get_width(pixbuf);
    item->y2 = y + gdk_pixbuf_get_height(pixbuf);
    item->rotation = 0.0;
    
    // --- FIX: Cache the surface IMMEDIATELY upon creation ---
    // This prevents the lag spike on the very first frame after pasting.
    int w = gdk_pixbuf_get_width(pixbuf);
    int h = gdk_pixbuf_get_height(pixbuf);
    item->cached_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *tmp_cr = cairo_create(item->cached_surface);
    gdk_cairo_set_source_pixbuf(tmp_cr, pixbuf, 0, 0);
    cairo_paint(tmp_cr);
    cairo_destroy(tmp_cr);
    
    return item;
}

void annotation_shape_update(AnnotationItem *item, double x2, double y2) {
    if (item->type == ANNOTATION_RECTANGLE || item->type == ANNOTATION_CIRCLE || item->type == ANNOTATION_ARROW || item->type == ANNOTATION_PIXELATE || item->type == ANNOTATION_IMAGE) {
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
    } else if (item->type == ANNOTATION_IMAGE && item->pixbuf) {
        g_object_unref(item->pixbuf); 
    }
    
    if (item->cached_surface) {
        cairo_surface_destroy(item->cached_surface);
    }
    
    g_free(item);
}

void annotation_items_free_list(GList *items) {
    g_list_free_full(items, annotation_item_free);
}

void annotation_item_get_bounds(AnnotationItem *item, double *bx, double *by, double *bw, double *bh) {
    if (!item) return;
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    
    if (item->type == ANNOTATION_STROKE && item->points->len > 0) {
        AnnotationPoint *pts = (AnnotationPoint *)item->points->data;
        x1 = x2 = pts[0].x; 
        y1 = y2 = pts[0].y;
        for (guint i = 1; i < item->points->len; i++) {
            if (pts[i].x < x1) x1 = pts[i].x;
            if (pts[i].x > x2) x2 = pts[i].x;
            if (pts[i].y < y1) y1 = pts[i].y;
            if (pts[i].y > y2) y2 = pts[i].y;
        }
    } else if (item->type == ANNOTATION_TEXT) {
        x1 = item->x; 
        y1 = item->y; 
        x2 = item->x + (strlen(item->text) * item->font_size * 0.65); 
        y2 = item->y + (item->font_size * 1.1); 
    } else {
        x1 = MIN(item->x, item->x2);
        x2 = MAX(item->x, item->x2);
        y1 = MIN(item->y, item->y2);
        y2 = MAX(item->y, item->y2);
    }
    
    double pad = MAX(15.0, item->line_width);
    *bx = x1 - pad;
    *by = y1 - pad;
    *bw = (x2 - x1) + (pad * 2);
    *bh = (y2 - y1) + (pad * 2);
}

void annotation_item_translate(AnnotationItem *item, double dx, double dy) {
    if (!item) return;
    if (item->type == ANNOTATION_STROKE) {
        AnnotationPoint *pts = (AnnotationPoint *)item->points->data;
        for (guint i = 0; i < item->points->len; i++) {
            pts[i].x += dx;
            pts[i].y += dy;
        }
    } else {
        item->x += dx;
        item->y += dy;
        item->x2 += dx;
        item->y2 += dy;
    }
}

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

void annotation_draw_all(cairo_t *cr, GList *items, GdkPixbuf *bg, AnnotationItem *selected_item, double offset_x, double offset_y, double zoom_x, double zoom_y) {
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    for (GList *l = items; l != NULL; l = l->next) {
        AnnotationItem *item = l->data;
        
        cairo_save(cr);
        
        if (item->rotation != 0.0) {
            double bx, by, bw, bh;
            annotation_item_get_bounds(item, &bx, &by, &bw, &bh);
            double cx = (bx + bw / 2.0 + offset_x) * zoom_x;
            double cy = (by + bh / 2.0 + offset_y) * zoom_y;
            
            cairo_translate(cr, cx, cy);
            cairo_rotate(cr, item->rotation);
            cairo_translate(cr, -cx, -cy);
        }

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
        else if (item->type == ANNOTATION_IMAGE && item->cached_surface != NULL) {
            // --- FIX: Zero-CPU resizing! ---
            // Instead of re-creating the image cache at the new size, we just
            // use Cairo's native matrix math to stretch our cached original!
            double bx = MIN(item->x, item->x2);
            double by = MIN(item->y, item->y2);
            double bw = fabs(item->x2 - item->x);
            double bh = fabs(item->y2 - item->y);

            double original_w = (double)gdk_pixbuf_get_width(item->pixbuf);
            double original_h = (double)gdk_pixbuf_get_height(item->pixbuf);
            
            double scale_img_x = bw / MAX(1.0, original_w);
            double scale_img_y = bh / MAX(1.0, original_h);

            cairo_save(cr);
            cairo_translate(cr, (bx + offset_x) * zoom_x, (by + offset_y) * zoom_y);
            
            // Apply scale matrix to the canvas
            cairo_scale(cr, scale_img_x * zoom_x, scale_img_y * zoom_y);
            
            // Draw the original cached image surface
            cairo_set_source_surface(cr, item->cached_surface, 0, 0);
            
            // Use FAST filter for buttery smooth rendering while dragged/resizing
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
            
            cairo_paint(cr);
            cairo_restore(cr);
        }
        else if (item->type == ANNOTATION_PIXELATE && bg != NULL) {
            double px = MIN(item->x, item->x2);
            double py = MIN(item->y, item->y2);
            double pw = fabs(item->x2 - item->x);
            double ph = fabs(item->y2 - item->y);
            
            int bg_w = gdk_pixbuf_get_width(bg);
            int bg_h = gdk_pixbuf_get_height(bg);
            
            int ipx = MAX(0, (int)px);
            int ipy = MAX(0, (int)py);
            int ipw = MIN((int)pw, bg_w - ipx);
            int iph = MIN((int)ph, bg_h - ipy);
            
            if (ipw > 0 && iph > 0) {
                if (item->cached_surface == NULL || item->cached_x != ipx || item->cached_y != ipy || item->cached_w != ipw || item->cached_h != iph || item->cached_line_width != item->line_width) {
                    if (item->cached_surface) cairo_surface_destroy(item->cached_surface);
                    
                    int block_size = MAX(4, (int)(item->line_width)); 
                    int sw = MAX(1, ipw / block_size);
                    int sh = MAX(1, iph / block_size);
                    
                    GdkPixbuf *sub = gdk_pixbuf_new_subpixbuf(bg, ipx, ipy, ipw, iph);
                    GdkPixbuf *down = gdk_pixbuf_scale_simple(sub, sw, sh, GDK_INTERP_NEAREST);
                    GdkPixbuf *up = gdk_pixbuf_scale_simple(down, ipw, iph, GDK_INTERP_NEAREST);
                    
                    item->cached_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ipw, iph);
                    cairo_t *tmp_cr = cairo_create(item->cached_surface);
                    gdk_cairo_set_source_pixbuf(tmp_cr, up, 0, 0);
                    cairo_paint(tmp_cr);
                    cairo_destroy(tmp_cr);
                    
                    g_object_unref(up);
                    g_object_unref(down);
                    g_object_unref(sub);
                    
                    item->cached_x = ipx;
                    item->cached_y = ipy;
                    item->cached_w = ipw;
                    item->cached_h = iph;
                    item->cached_line_width = item->line_width;
                }

                cairo_save(cr);
                cairo_scale(cr, zoom_x, zoom_y);
                cairo_translate(cr, offset_x, offset_y);
                cairo_set_source_surface(cr, item->cached_surface, ipx, ipy);
                cairo_rectangle(cr, ipx, ipy, ipw, iph);
                cairo_fill(cr);
                cairo_restore(cr);
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
        
        cairo_restore(cr); 
    }

    if (selected_item) {
        double bx, by, bw, bh;
        annotation_item_get_bounds(selected_item, &bx, &by, &bw, &bh);
        double cx = (bx + bw / 2.0 + offset_x) * zoom_x;
        double cy = (by + bh / 2.0 + offset_y) * zoom_y;
        
        cairo_save(cr);
        
        cairo_translate(cr, cx, cy);
        cairo_rotate(cr, selected_item->rotation);
        cairo_translate(cr, -cx, -cy);

        cairo_new_path(cr);
        cairo_set_source_rgba(cr, 0.2, 0.6, 1.0, 0.8); 
        cairo_set_line_width(cr, 2.0);
        
        double dashes[] = {6.0, 4.0};
        cairo_set_dash(cr, dashes, 2, 0);
        cairo_rectangle(cr, (bx + offset_x) * zoom_x, (by + offset_y) * zoom_y, bw * zoom_x, bh * zoom_y);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0); 
        
        cairo_move_to(cr, cx, (by + offset_y) * zoom_y);
        cairo_line_to(cr, cx, (by - 25 + offset_y) * zoom_y);
        cairo_stroke(cr);
        
        cairo_arc(cr, cx, (by - 25 + offset_y) * zoom_y, 6, 0, 2 * M_PI);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.2, 0.6, 1.0);
        cairo_stroke(cr);

        cairo_rectangle(cr, (bx + bw - 5 + offset_x) * zoom_x, (by + bh - 5 + offset_y) * zoom_y, 10, 10);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.2, 0.6, 1.0);
        cairo_stroke(cr);

        cairo_restore(cr);
    }
}

gboolean annotation_save_composite(GdkPixbuf *bg, GdkRectangle *crop, GList *items, const char *output_png_path) {
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, crop->width, crop->height);
    cairo_t *cr = cairo_create(surface);

    gdk_cairo_set_source_pixbuf(cr, bg, -crop->x, -crop->y);
    cairo_paint(cr);

    annotation_draw_all(cr, items, bg, NULL, -crop->x, -crop->y, 1.0, 1.0);

    cairo_status_t status = cairo_surface_write_to_png(surface, output_png_path);
    
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    
    return (status == CAIRO_STATUS_SUCCESS);
}