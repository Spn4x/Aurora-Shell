#include "annotation_ui.h"
#include "utils.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

static void resize_text_entry(UIState *state) {
    if (!state->active_text_entry) return;
    
    const char *text = gtk_editable_get_text(GTK_EDITABLE(state->active_text_entry));
    if (!text || strlen(text) == 0) text = " ";

    PangoLayout *layout = gtk_widget_create_pango_layout(state->active_text_entry, text);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, "sans-serif");
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    
    double scaled_font_size = state->current_font_size / state->scale_y;
    pango_font_description_set_absolute_size(desc, scaled_font_size * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    
    int width, height;
    pango_layout_get_pixel_size(layout, &width, &height);
    gtk_widget_set_size_request(state->active_text_entry, width + 20, -1);
    
    pango_font_description_free(desc);
    g_object_unref(layout);
}

static void update_active_text_appearance(UIState *state) {
    if (!state->active_text_entry) return;

    PangoAttrList *attrs = pango_attr_list_new();
    guint16 r = (guint16)(state->current_color.red * 65535);
    guint16 g = (guint16)(state->current_color.green * 65535);
    guint16 b = (guint16)(state->current_color.blue * 65535);
    
    pango_attr_list_insert(attrs, pango_attr_foreground_new(r, g, b));
    pango_attr_list_insert(attrs, pango_attr_family_new("sans-serif"));
    
    double scaled_font_size = state->current_font_size / state->scale_y;
    pango_attr_list_insert(attrs, pango_attr_size_new_absolute(scaled_font_size * PANGO_SCALE));
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    
    gtk_entry_set_attributes(GTK_ENTRY(state->active_text_entry), attrs);
    pango_attr_list_unref(attrs);
    resize_text_entry(state);
}

void finalize_text_entry(UIState *state) {
    if (!state->active_text_entry) return;
    
    GtkWidget *entry = state->active_text_entry;
    state->active_text_entry = NULL;
    
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (text && strlen(text) > 0) {
        AnnotationItem *item = annotation_text_new(&state->current_color, text, state->active_text_x, state->active_text_y, state->current_font_size);
        item->rotation = state->active_text_rotation; 
        
        state->strokes = g_list_append(state->strokes, item);
        
        if (state->redo_strokes) {
            annotation_items_free_list(state->redo_strokes);
            state->redo_strokes = NULL;
        }
    }
    
    state->active_text_rotation = 0.0; 
    
    gtk_fixed_remove(GTK_FIXED(state->annotation_fixed), entry);
    
    // --- FIX: Turn off the invisible shield so we can click the canvas again! ---
    gtk_widget_set_can_target(state->annotation_fixed, FALSE);
    
    gtk_widget_queue_draw(state->drawing_area);
    gtk_widget_grab_focus(state->drawing_area);
}

static void on_text_entry_activate(GtkEntry *entry, gpointer user_data) {
    (void)entry; finalize_text_entry((UIState *)user_data);
}

static void on_text_entry_changed(GtkEditable *editable, gpointer user_data) {
    (void)editable; resize_text_entry((UIState *)user_data);
}

void enter_annotation_phase(UIState *state) {
    state->is_annotating = TRUE;
    state->ignore_next_drag = FALSE;
    
    gtk_revealer_set_reveal_child(state->bottom_panel_revealer, FALSE);
    gtk_revealer_set_reveal_child(state->top_panel_revealer, TRUE);
    gtk_revealer_set_reveal_child(state->ann_bottom_panel_revealer, TRUE); 
    
    gdk_rgba_parse(&state->current_color, "#ff3333");
    
    for (GList *l = state->color_indicators; l != NULL; l = l->next) {
        GtkWidget *btn = GTK_WIDGET(l->data);
        GdkRGBA *color = g_object_get_data(G_OBJECT(btn), "color-rgba");
        if (gdk_rgba_equal(color, &state->current_color)) {
            gtk_widget_add_css_class(btn, "active-color");
        } else {
            gtk_widget_remove_css_class(btn, "active-color");
        }
    }
    
    if (state->redo_strokes) {
        annotation_items_free_list(state->redo_strokes);
        state->redo_strokes = NULL;
    }

    state->selected_item = NULL;
    if (state->angle_scale) gtk_widget_set_sensitive(state->angle_scale, FALSE);

    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(state->drag_gesture), GTK_PHASE_CAPTURE);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(state->motion_controller), GTK_PHASE_NONE);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(state->click_gesture), GTK_PHASE_CAPTURE);

    gtk_widget_queue_draw(state->drawing_area);
    gtk_widget_grab_focus(state->drawing_area);
}

void cancel_annotation_phase(UIState *state) {
    finalize_text_entry(state); 
    state->is_annotating = FALSE;
    state->selected_item = NULL;
    state->ignore_next_drag = FALSE;
    
    gtk_revealer_set_reveal_child(state->top_panel_revealer, FALSE);
    gtk_revealer_set_reveal_child(state->ann_bottom_panel_revealer, FALSE); 
    gtk_revealer_set_reveal_child(state->bottom_panel_revealer, TRUE);
    
    annotation_items_free_list(state->strokes); state->strokes = NULL;
    annotation_items_free_list(state->redo_strokes); state->redo_strokes = NULL;
    
    qscreen_set_mode(state, state->current_mode);
    gtk_widget_queue_draw(state->drawing_area);
    gtk_widget_grab_focus(state->drawing_area);
}

static void annotation_ui_undo(UIState *state) {
    finalize_text_entry(state);
    state->selected_item = NULL;
    if (state->angle_scale) gtk_widget_set_sensitive(state->angle_scale, FALSE);
    if (!state->strokes) return; 

    GList *last = g_list_last(state->strokes);
    state->redo_strokes = g_list_prepend(state->redo_strokes, last->data);
    state->strokes = g_list_delete_link(state->strokes, last);
    gtk_widget_queue_draw(state->drawing_area);
}

static void annotation_ui_redo(UIState *state) {
    finalize_text_entry(state);
    state->selected_item = NULL;
    if (state->angle_scale) gtk_widget_set_sensitive(state->angle_scale, FALSE);
    if (!state->redo_strokes) return;

    GList *first = state->redo_strokes;
    state->strokes = g_list_append(state->strokes, first->data);
    state->redo_strokes = g_list_remove_link(state->redo_strokes, first);
    gtk_widget_queue_draw(state->drawing_area);
}

static void on_undo_clicked(GtkButton *btn, gpointer user_data) { (void)btn; annotation_ui_undo((UIState *)user_data); }
static void on_redo_clicked(GtkButton *btn, gpointer user_data) { (void)btn; annotation_ui_redo((UIState *)user_data); }
static void on_cancel_clicked(GtkButton *btn, gpointer user_data) { (void)btn; cancel_annotation_phase((UIState *)user_data); }

void annotation_ui_confirm(UIState *state) {
    finalize_text_entry(state); 
    state->selected_item = NULL;
    if (!state->window || !gtk_widget_get_visible(GTK_WIDGET(state->window))) return;

    GdkRectangle crop = { 
        (int)round(state->current_x), (int)round(state->current_y), 
        (int)round(state->current_w), (int)round(state->current_h) 
    };
    
    gboolean save_to_disk = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->save_button));
    g_autofree char *temp_composite = g_build_filename(g_get_tmp_dir(), "qscreen_composite.png", NULL);
    
    annotation_save_composite(state->screenshot_pixbuf, &crop, state->strokes, temp_composite);
    process_precomposited_screenshot(temp_composite, save_to_disk, state->app_state);

    if (state->window) gtk_window_destroy(state->window);
}
static void on_confirm_clicked(GtkButton *btn, gpointer user_data) { (void)btn; annotation_ui_confirm((UIState *)user_data); }

static void on_angle_spin_changed(GtkSpinButton *spin, gpointer user_data);
static void on_size_spin_changed(GtkSpinButton *spin, gpointer user_data);

void annotation_ui_drag_begin(UIState *state, double scaled_x, double scaled_y, double raw_x, double raw_y) {
    if (state->ignore_next_drag) {
        state->ignore_next_drag = FALSE;
        return;
    }

    finalize_text_entry(state);

    if (state->current_ann_mode == ANN_MODE_SELECT) {
        if (state->selected_item) {
            double bx, by, bw, bh;
            annotation_item_get_bounds(state->selected_item, &bx, &by, &bw, &bh);
            double cx = bx + bw / 2.0;
            double cy = by + bh / 2.0;

            double lx = cx + (scaled_x - cx) * cos(-state->selected_item->rotation) - (scaled_y - cy) * sin(-state->selected_item->rotation);
            double ly = cy + (scaled_x - cx) * sin(-state->selected_item->rotation) + (scaled_y - cy) * cos(-state->selected_item->rotation);

            if (hypot(lx - cx, ly - (by - 25)) < 15.0) {
                state->current_drag_action = DRAG_ROTATE;
                state->drag_start_cx = cx;
                state->drag_start_cy = cy;
                return;
            }

            if (fabs(lx - (bx + bw)) < 15.0 && fabs(ly - (by + bh)) < 15.0) {
                state->current_drag_action = DRAG_RESIZE;
                state->last_drag_x = scaled_x;
                state->last_drag_y = scaled_y;
                return;
            }
        }

        state->selected_item = NULL;
        gtk_widget_set_sensitive(state->angle_scale, FALSE); 
        
        for (GList *l = g_list_last(state->strokes); l != NULL; l = l->prev) {
            AnnotationItem *item = l->data;
            double bx, by, bw, bh;
            annotation_item_get_bounds(item, &bx, &by, &bw, &bh);
            double cx = bx + bw / 2.0;
            double cy = by + bh / 2.0;

            double lx = cx + (scaled_x - cx) * cos(-item->rotation) - (scaled_y - cy) * sin(-item->rotation);
            double ly = cy + (scaled_x - cx) * sin(-item->rotation) + (scaled_y - cy) * cos(-item->rotation);
            
            if (lx >= bx && lx <= bx + bw && ly >= by && ly <= by + bh) {
                state->selected_item = item;
                state->last_drag_x = scaled_x;
                state->last_drag_y = scaled_y;
                state->current_drag_action = DRAG_MOVE;
                
                state->strokes = g_list_remove_link(state->strokes, l);
                state->strokes = g_list_append(state->strokes, item);
                g_list_free(l);
                
                g_signal_handlers_block_by_func(state->angle_scale, on_angle_spin_changed, state);
                double deg = state->selected_item->rotation * 180.0 / M_PI;
                while (deg > 180.0) deg -= 360.0;
                while (deg < -180.0) deg += 360.0;
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->angle_scale), deg);
                gtk_widget_set_sensitive(state->angle_scale, TRUE);
                g_signal_handlers_unblock_by_func(state->angle_scale, on_angle_spin_changed, state);

                g_signal_handlers_block_by_func(state->size_scale, on_size_spin_changed, state);
                if (state->selected_item->type == ANNOTATION_TEXT) {
                    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->size_scale), state->selected_item->font_size);
                } else {
                    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->size_scale), state->selected_item->line_width);
                }
                g_signal_handlers_unblock_by_func(state->size_scale, on_size_spin_changed, state);

                break;
            }
        }
        gtk_widget_queue_draw(state->drawing_area);
        return;
    }
    
    state->selected_item = NULL; 
    gtk_widget_set_sensitive(state->angle_scale, FALSE);

    if (state->current_ann_mode == ANN_MODE_TEXT) {
        state->active_text_x = scaled_x;
        state->active_text_y = scaled_y;
        state->active_text_rotation = 0.0; 
        
        state->active_text_entry = gtk_entry_new();
        gtk_entry_set_has_frame(GTK_ENTRY(state->active_text_entry), FALSE);
        gtk_widget_add_css_class(state->active_text_entry, "annotation-text-entry"); 

        update_active_text_appearance(state);
        gtk_editable_set_text(GTK_EDITABLE(state->active_text_entry), "Enter text");
        gtk_editable_select_region(GTK_EDITABLE(state->active_text_entry), 0, -1);
        resize_text_entry(state);

        // Turn on the invisible shield so the entry can receive mouse focus
        gtk_widget_set_can_target(state->annotation_fixed, TRUE);

        gtk_fixed_put(GTK_FIXED(state->annotation_fixed), state->active_text_entry, raw_x, raw_y);
        g_signal_connect(state->active_text_entry, "activate", G_CALLBACK(on_text_entry_activate), state);
        g_signal_connect(state->active_text_entry, "changed", G_CALLBACK(on_text_entry_changed), state);
        gtk_widget_grab_focus(state->active_text_entry);
    } else {
        if (state->redo_strokes) {
            annotation_items_free_list(state->redo_strokes);
            state->redo_strokes = NULL;
        }
        
        if (state->current_ann_mode == ANN_MODE_DRAW) {
            state->current_stroke = annotation_stroke_new(&state->current_color, state->current_brush_size);
            annotation_stroke_add_point(state->current_stroke, scaled_x, scaled_y);
        } else if (state->current_ann_mode == ANN_MODE_RECTANGLE) {
            state->current_stroke = annotation_shape_new(ANNOTATION_RECTANGLE, &state->current_color, state->current_brush_size, scaled_x, scaled_y);
        } else if (state->current_ann_mode == ANN_MODE_CIRCLE) {
            state->current_stroke = annotation_shape_new(ANNOTATION_CIRCLE, &state->current_color, state->current_brush_size, scaled_x, scaled_y);
        } else if (state->current_ann_mode == ANN_MODE_ARROW) {
            state->current_stroke = annotation_shape_new(ANNOTATION_ARROW, &state->current_color, state->current_brush_size, scaled_x, scaled_y);
        } else if (state->current_ann_mode == ANN_MODE_PIXELATE) { 
            state->current_stroke = annotation_shape_new(ANNOTATION_PIXELATE, &state->current_color, state->current_brush_size, scaled_x, scaled_y);
        }
        
        state->strokes = g_list_append(state->strokes, state->current_stroke);
    }
}

void annotation_ui_drag_update(UIState *state, double scaled_x, double scaled_y) {
    if (state->current_ann_mode == ANN_MODE_SELECT && state->selected_item) {
        
        if (state->current_drag_action == DRAG_MOVE) {
            double dx = scaled_x - state->last_drag_x;
            double dy = scaled_y - state->last_drag_y;
            annotation_item_translate(state->selected_item, dx, dy);
            state->last_drag_x = scaled_x;
            state->last_drag_y = scaled_y;
            
        } else if (state->current_drag_action == DRAG_ROTATE) {
            double angle = atan2(scaled_y - state->drag_start_cy, scaled_x - state->drag_start_cx);
            state->selected_item->rotation = angle + M_PI_2;
            
            g_signal_handlers_block_by_func(state->angle_scale, on_angle_spin_changed, state);
            double deg = state->selected_item->rotation * 180.0 / M_PI;
            while (deg > 180.0) deg -= 360.0;
            while (deg < -180.0) deg += 360.0;
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->angle_scale), deg);
            g_signal_handlers_unblock_by_func(state->angle_scale, on_angle_spin_changed, state);

        } else if (state->current_drag_action == DRAG_RESIZE) {
            double bx, by, bw, bh;
            annotation_item_get_bounds(state->selected_item, &bx, &by, &bw, &bh);
            double cx = bx + bw / 2.0;
            double cy = by + bh / 2.0;

            double lx = cx + (scaled_x - cx) * cos(-state->selected_item->rotation) - (scaled_y - cy) * sin(-state->selected_item->rotation);
            double ly = cy + (scaled_x - cx) * sin(-state->selected_item->rotation) + (scaled_y - cy) * cos(-state->selected_item->rotation);

            double dx = lx - (bx + bw);
            double dy = ly - (by + bh);

            if (state->selected_item->type == ANNOTATION_TEXT) {
                double scale = (bw + dx) / MAX(1.0, bw);
                if (scale > 0.1) {
                    state->selected_item->font_size *= scale;
                    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->size_scale), state->selected_item->font_size);
                }
            } else if (state->selected_item->type == ANNOTATION_STROKE) {
                double scale_x = (bw + dx) / MAX(1.0, bw);
                double scale_y = (bh + dy) / MAX(1.0, bh);
                
                if (scale_x > 0.1 && scale_y > 0.1) {
                    AnnotationPoint *pts = (AnnotationPoint *)state->selected_item->points->data;
                    for (guint i = 0; i < state->selected_item->points->len; i++) {
                        pts[i].x = cx + (pts[i].x - cx) * scale_x;
                        pts[i].y = cy + (pts[i].y - cy) * scale_y;
                    }
                }
            } else {
                if (state->selected_item->x < state->selected_item->x2) state->selected_item->x2 += dx;
                else state->selected_item->x += dx;
                
                if (state->selected_item->y < state->selected_item->y2) state->selected_item->y2 += dy;
                else state->selected_item->y += dy;
            }

            state->last_drag_x = scaled_x;
            state->last_drag_y = scaled_y;
        }
        
        gtk_widget_queue_draw(state->drawing_area);
        return;
    }
    
    if (!state->current_stroke) return;
    
    if (state->current_ann_mode == ANN_MODE_DRAW) {
        annotation_stroke_add_point(state->current_stroke, scaled_x, scaled_y);
    } 
    else if (state->current_ann_mode == ANN_MODE_RECTANGLE || state->current_ann_mode == ANN_MODE_CIRCLE || state->current_ann_mode == ANN_MODE_ARROW || state->current_ann_mode == ANN_MODE_PIXELATE) {
        annotation_shape_update(state->current_stroke, scaled_x, scaled_y);
    }
    gtk_widget_queue_draw(state->drawing_area);
}

void annotation_ui_double_click(UIState *state, double raw_x, double raw_y) {
    if (!state->is_annotating) return;
    if (state->current_ann_mode != ANN_MODE_SELECT) return;

    double scaled_x = raw_x * state->scale_x;
    double scaled_y = raw_y * state->scale_y;

    for (GList *l = g_list_last(state->strokes); l != NULL; l = l->prev) {
        AnnotationItem *item = l->data;
        if (item->type == ANNOTATION_TEXT) {
            double bx, by, bw, bh;
            annotation_item_get_bounds(item, &bx, &by, &bw, &bh);
            double cx = bx + bw / 2.0;
            double cy = by + bh / 2.0;

            double lx = cx + (scaled_x - cx) * cos(-item->rotation) - (scaled_y - cy) * sin(-item->rotation);
            double ly = cy + (scaled_x - cx) * sin(-item->rotation) + (scaled_y - cy) * cos(-item->rotation);
            
            if (lx >= bx && lx <= bx + bw && ly >= by && ly <= by + bh) {
                state->selected_item = NULL;
                gtk_widget_set_sensitive(state->angle_scale, FALSE);
                
                state->active_text_x = item->x;
                state->active_text_y = item->y;
                state->current_color = item->color;
                state->current_font_size = item->font_size;
                
                state->active_text_rotation = item->rotation;
                
                double place_raw_x = item->x / state->scale_x;
                double place_raw_y = (item->y - item->font_size) / state->scale_y; 
                
                state->active_text_entry = gtk_entry_new();
                gtk_entry_set_has_frame(GTK_ENTRY(state->active_text_entry), FALSE);
                gtk_widget_add_css_class(state->active_text_entry, "annotation-text-entry"); 

                update_active_text_appearance(state);
                gtk_editable_set_text(GTK_EDITABLE(state->active_text_entry), item->text);
                gtk_editable_select_region(GTK_EDITABLE(state->active_text_entry), 0, -1);
                resize_text_entry(state);

                // Turn on the invisible shield again!
                gtk_widget_set_can_target(state->annotation_fixed, TRUE);

                gtk_fixed_put(GTK_FIXED(state->annotation_fixed), state->active_text_entry, place_raw_x, place_raw_y);
                g_signal_connect(state->active_text_entry, "activate", G_CALLBACK(on_text_entry_activate), state);
                g_signal_connect(state->active_text_entry, "changed", G_CALLBACK(on_text_entry_changed), state);
                
                state->ignore_next_drag = TRUE;
                
                gtk_widget_grab_focus(state->active_text_entry);
                
                state->strokes = g_list_remove(state->strokes, item);
                annotation_item_free(item);
                
                gtk_widget_queue_draw(state->drawing_area);
                return;
            }
        }
    }
}

static void paste_from_clipboard(UIState *state) {
    gint exit_status = 0;
    g_autofree gchar *tmp_img_path = g_build_filename(g_get_tmp_dir(), "qscreen_paste.png", NULL);
    
    g_autofree gchar *cmd = g_strdup_printf("sh -c \"wl-paste -t image/png > '%s'\"", tmp_img_path);
    gboolean success = g_spawn_command_line_sync(cmd, NULL, NULL, &exit_status, NULL);
    
    double cx = gdk_pixbuf_get_width(state->screenshot_pixbuf) / 2.0;
    double cy = gdk_pixbuf_get_height(state->screenshot_pixbuf) / 2.0;

    if (success && exit_status == 0) {
        GError *err = NULL;
        GdkPixbuf *pasted_pixbuf = gdk_pixbuf_new_from_file(tmp_img_path, &err);
        
        if (pasted_pixbuf) {
            double pw = gdk_pixbuf_get_width(pasted_pixbuf);
            double ph = gdk_pixbuf_get_height(pasted_pixbuf);
            
            double max_w = gdk_pixbuf_get_width(state->screenshot_pixbuf) * 0.7;
            double max_h = gdk_pixbuf_get_height(state->screenshot_pixbuf) * 0.7;
            
            if (pw > max_w || ph > max_h) {
                double scale = fmin(max_w / pw, max_h / ph);
                int new_w = MAX(1, (int)(pw * scale));
                int new_h = MAX(1, (int)(ph * scale));
                
                GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pasted_pixbuf, new_w, new_h, GDK_INTERP_BILINEAR);
                g_object_unref(pasted_pixbuf);
                pasted_pixbuf = scaled;
                pw = new_w;
                ph = new_h;
            }

            AnnotationItem *item = annotation_image_new(pasted_pixbuf, cx - pw/2.0, cy - ph/2.0);
            
            state->strokes = g_list_append(state->strokes, item);
            state->selected_item = item;
            if (state->redo_strokes) { annotation_items_free_list(state->redo_strokes); state->redo_strokes = NULL; }
            
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ann_select_btn), TRUE);
            gtk_widget_queue_draw(state->drawing_area);
            return;
        }
        if (err) g_error_free(err);
    }
    
    g_autofree gchar *paste_text = NULL;
    success = g_spawn_command_line_sync("wl-paste -t text/plain", &paste_text, NULL, &exit_status, NULL);
    
    if (success && paste_text && strlen(paste_text) > 0) {
        g_strchomp(paste_text); 
        if (strlen(paste_text) == 0) return;
        
        AnnotationItem *item = annotation_text_new(&state->current_color, paste_text, cx, cy, state->current_font_size);
        item->x = cx - (strlen(paste_text) * state->current_font_size * 0.3); 
        
        state->strokes = g_list_append(state->strokes, item);
        state->selected_item = item;
        if (state->redo_strokes) { annotation_items_free_list(state->redo_strokes); state->redo_strokes = NULL; }
        
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ann_select_btn), TRUE);
        gtk_widget_queue_draw(state->drawing_area);
    }
}

gboolean annotation_ui_handle_key(UIState *state, guint keyval, GdkModifierType mod_state) {
    if (!state->is_annotating) return FALSE;

    if ((keyval == GDK_KEY_v || keyval == GDK_KEY_V) && (mod_state & GDK_CONTROL_MASK) && state->active_text_entry == NULL) {
        paste_from_clipboard(state);
        return TRUE;
    }

    if (keyval == GDK_KEY_Delete || keyval == GDK_KEY_BackSpace) {
        if (state->current_ann_mode == ANN_MODE_SELECT && state->selected_item && state->active_text_entry == NULL) {
            state->strokes = g_list_remove(state->strokes, state->selected_item);
            annotation_item_free(state->selected_item);
            state->selected_item = NULL;
            gtk_widget_set_sensitive(state->angle_scale, FALSE);
            gtk_widget_queue_draw(state->drawing_area);
            return TRUE;
        }
    }

    if (keyval == GDK_KEY_Escape) {
        cancel_annotation_phase(state);
        return TRUE;
    } else if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if (state->active_text_entry) finalize_text_entry(state);
        else annotation_ui_confirm(state);
        return TRUE;
    } else if (keyval == GDK_KEY_z || keyval == GDK_KEY_Z) {
        if ((mod_state & GDK_CONTROL_MASK) && (mod_state & GDK_SHIFT_MASK)) annotation_ui_redo(state);
        else if (mod_state & GDK_CONTROL_MASK) annotation_ui_undo(state);
        return TRUE;
    } else if ((keyval == GDK_KEY_y || keyval == GDK_KEY_Y || keyval == GDK_KEY_r || keyval == GDK_KEY_R) && (mod_state & GDK_CONTROL_MASK)) {
        annotation_ui_redo(state);
        return TRUE;
    }
    return FALSE;
}

static void on_size_spin_changed(GtkSpinButton *spin, gpointer user_data) {
    UIState *state = user_data;
    double val = gtk_spin_button_get_value(spin);
    if (state->current_ann_mode == ANN_MODE_TEXT) {
        state->current_font_size = val;
        update_active_text_appearance(state);
    } else if (state->current_ann_mode == ANN_MODE_SELECT && state->selected_item) {
        if (state->selected_item->type == ANNOTATION_TEXT) {
            state->selected_item->font_size = val;
        } else {
            state->selected_item->line_width = val;
        }
        gtk_widget_queue_draw(state->drawing_area);
    } else {
        state->current_brush_size = val;
    }
}

static void on_angle_spin_changed(GtkSpinButton *spin, gpointer user_data) {
    UIState *state = user_data;
    if (state->current_ann_mode == ANN_MODE_SELECT && state->selected_item) {
        double deg = gtk_spin_button_get_value(spin);
        state->selected_item->rotation = deg * M_PI / 180.0;
        gtk_widget_queue_draw(state->drawing_area);
    }
}

static void on_ann_mode_toggled(GtkToggleButton *btn, gpointer user_data) {
    if (!gtk_toggle_button_get_active(btn)) return;
    UIState *state = user_data;
    finalize_text_entry(state);
    
    if (GTK_WIDGET(btn) != state->ann_select_btn) {
        state->selected_item = NULL;
        gtk_widget_set_sensitive(state->angle_scale, FALSE); 
        gtk_widget_queue_draw(state->drawing_area);
    }
    
    g_signal_handlers_block_by_func(state->size_scale, on_size_spin_changed, state);
    
    if (GTK_WIDGET(btn) == state->ann_select_btn) {
        state->current_ann_mode = ANN_MODE_SELECT;
        gtk_widget_set_sensitive(state->size_scale, TRUE);
    } else if (GTK_WIDGET(btn) == state->ann_text_btn) {
        state->current_ann_mode = ANN_MODE_TEXT;
        gtk_widget_set_sensitive(state->size_scale, TRUE);
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(state->size_scale), 12.0, 120.0);
        gtk_spin_button_set_increments(GTK_SPIN_BUTTON(state->size_scale), 2.0, 10.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->size_scale), state->current_font_size);
    } else {
        if (GTK_WIDGET(btn) == state->ann_draw_btn) state->current_ann_mode = ANN_MODE_DRAW;
        else if (GTK_WIDGET(btn) == state->ann_rect_btn) state->current_ann_mode = ANN_MODE_RECTANGLE;
        else if (GTK_WIDGET(btn) == state->ann_circle_btn) state->current_ann_mode = ANN_MODE_CIRCLE;
        else if (GTK_WIDGET(btn) == state->ann_arrow_btn) state->current_ann_mode = ANN_MODE_ARROW;
        else if (GTK_WIDGET(btn) == state->ann_pixelate_btn) state->current_ann_mode = ANN_MODE_PIXELATE;

        gtk_widget_set_sensitive(state->size_scale, TRUE);
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(state->size_scale), 2.0, 40.0);
        gtk_spin_button_set_increments(GTK_SPIN_BUTTON(state->size_scale), 1.0, 5.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->size_scale), state->current_brush_size);
    }
    
    g_signal_handlers_unblock_by_func(state->size_scale, on_size_spin_changed, state);
}

static void on_color_button_clicked(GtkButton *clicked_btn, gpointer user_data) {
    UIState *state = user_data;
    GdkRGBA *color = g_object_get_data(G_OBJECT(clicked_btn), "color-rgba");
    if (color) {
        state->current_color = *color;
        
        if (state->current_ann_mode == ANN_MODE_SELECT && state->selected_item) {
            state->selected_item->color = *color;
            gtk_widget_queue_draw(state->drawing_area);
        }
        
        for (GList *l = state->color_indicators; l != NULL; l = l->next) {
            GtkWidget *btn = GTK_WIDGET(l->data);
            if (btn == GTK_WIDGET(clicked_btn)) {
                gtk_widget_add_css_class(btn, "active-color");
            } else {
                gtk_widget_remove_css_class(btn, "active-color");
            }
        }
        update_active_text_appearance(state);
    }
}

static GtkWidget* create_color_button(const char *color_hex, UIState *state) {
    GtkWidget *btn = gtk_button_new();
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(btn, GTK_ALIGN_CENTER);
    gtk_widget_set_focusable(btn, FALSE);
    
    gtk_widget_add_css_class(btn, "color-btn");
    
    GdkRGBA *rgba = g_new(GdkRGBA, 1);
    gdk_rgba_parse(rgba, color_hex);
    g_object_set_data_full(G_OBJECT(btn), "color-rgba", rgba, g_free);
    
    GtkCssProvider *p = gtk_css_provider_new();
    g_autofree char *css = g_strdup_printf(
        "button.color-btn { background-color: rgb(%d,%d,%d); }", 
        (int)(rgba->red*255), (int)(rgba->green*255), (int)(rgba->blue*255));
    
    gtk_css_provider_load_from_string(p, css);
    
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_style_context_add_provider(gtk_widget_get_style_context(btn), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    G_GNUC_END_IGNORE_DEPRECATIONS
    
    g_object_unref(p);

    state->color_indicators = g_list_append(state->color_indicators, btn);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_color_button_clicked), state);
    return btn;
}

GtkWidget* create_annotation_toolbar_top(UIState *state) {
    GtkWidget *frame = gtk_frame_new(NULL); 
    gtk_widget_add_css_class(frame, "panel"); 
    
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_frame_set_child(GTK_FRAME(frame), box);

    GtkWidget *cancel_btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_focusable(cancel_btn, FALSE); 
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_cancel_clicked), state);
    gtk_box_append(GTK_BOX(box), cancel_btn);

    GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_VERTICAL); 
    gtk_widget_add_css_class(sep1, "vertical-separator");
    gtk_box_append(GTK_BOX(box), sep1);

    state->ann_select_btn = gtk_toggle_button_new(); 
    gtk_widget_set_focusable(state->ann_select_btn, FALSE); 
    gtk_button_set_child(GTK_BUTTON(state->ann_select_btn), gtk_image_new_from_icon_name("edit-select-symbolic"));
    gtk_widget_set_tooltip_text(state->ann_select_btn, "Select / Move / Edit (Ctrl+V to Paste)");
    
    state->ann_draw_btn = gtk_toggle_button_new(); 
    gtk_widget_set_focusable(state->ann_draw_btn, FALSE); 
    gtk_button_set_child(GTK_BUTTON(state->ann_draw_btn), gtk_image_new_from_icon_name("document-edit-symbolic"));
    
    state->ann_rect_btn = gtk_toggle_button_new(); 
    gtk_widget_set_focusable(state->ann_rect_btn, FALSE); 
    gtk_button_set_child(GTK_BUTTON(state->ann_rect_btn), gtk_image_new_from_icon_name("media-playback-stop-symbolic"));

    state->ann_circle_btn = gtk_toggle_button_new(); 
    gtk_widget_set_focusable(state->ann_circle_btn, FALSE); 
    gtk_button_set_child(GTK_BUTTON(state->ann_circle_btn), gtk_image_new_from_icon_name("media-record-symbolic"));

    state->ann_arrow_btn = gtk_toggle_button_new(); 
    gtk_widget_set_focusable(state->ann_arrow_btn, FALSE); 
    gtk_button_set_child(GTK_BUTTON(state->ann_arrow_btn), gtk_image_new_from_icon_name("go-next-symbolic"));

    state->ann_pixelate_btn = gtk_toggle_button_new(); 
    gtk_widget_set_focusable(state->ann_pixelate_btn, FALSE); 
    gtk_button_set_child(GTK_BUTTON(state->ann_pixelate_btn), gtk_image_new_from_icon_name("view-conceal-symbolic"));

    state->ann_text_btn = gtk_toggle_button_new(); 
    gtk_widget_set_focusable(state->ann_text_btn, FALSE); 
    gtk_button_set_child(GTK_BUTTON(state->ann_text_btn), gtk_image_new_from_icon_name("insert-text-symbolic"));
    
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(state->ann_draw_btn), GTK_TOGGLE_BUTTON(state->ann_select_btn));
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(state->ann_rect_btn), GTK_TOGGLE_BUTTON(state->ann_select_btn));
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(state->ann_circle_btn), GTK_TOGGLE_BUTTON(state->ann_select_btn));
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(state->ann_arrow_btn), GTK_TOGGLE_BUTTON(state->ann_select_btn)); 
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(state->ann_pixelate_btn), GTK_TOGGLE_BUTTON(state->ann_select_btn)); 
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(state->ann_text_btn), GTK_TOGGLE_BUTTON(state->ann_select_btn));
    
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ann_draw_btn), TRUE);
    state->current_ann_mode = ANN_MODE_DRAW;
    
    g_signal_connect(state->ann_select_btn, "toggled", G_CALLBACK(on_ann_mode_toggled), state);
    g_signal_connect(state->ann_draw_btn, "toggled", G_CALLBACK(on_ann_mode_toggled), state);
    g_signal_connect(state->ann_rect_btn, "toggled", G_CALLBACK(on_ann_mode_toggled), state);
    g_signal_connect(state->ann_circle_btn, "toggled", G_CALLBACK(on_ann_mode_toggled), state);
    g_signal_connect(state->ann_arrow_btn, "toggled", G_CALLBACK(on_ann_mode_toggled), state); 
    g_signal_connect(state->ann_pixelate_btn, "toggled", G_CALLBACK(on_ann_mode_toggled), state); 
    g_signal_connect(state->ann_text_btn, "toggled", G_CALLBACK(on_ann_mode_toggled), state);

    gtk_box_append(GTK_BOX(box), state->ann_select_btn);
    gtk_box_append(GTK_BOX(box), state->ann_draw_btn);
    gtk_box_append(GTK_BOX(box), state->ann_rect_btn);
    gtk_box_append(GTK_BOX(box), state->ann_circle_btn);
    gtk_box_append(GTK_BOX(box), state->ann_arrow_btn); 
    gtk_box_append(GTK_BOX(box), state->ann_pixelate_btn); 
    gtk_box_append(GTK_BOX(box), state->ann_text_btn);

    GtkWidget *sep3 = gtk_separator_new(GTK_ORIENTATION_VERTICAL); 
    gtk_widget_add_css_class(sep3, "vertical-separator");
    gtk_box_append(GTK_BOX(box), sep3);

    GtkWidget *undo_btn = gtk_button_new_from_icon_name("edit-undo-symbolic");
    gtk_widget_set_focusable(undo_btn, FALSE);
    g_signal_connect(undo_btn, "clicked", G_CALLBACK(on_undo_clicked), state);
    gtk_box_append(GTK_BOX(box), undo_btn);

    GtkWidget *redo_btn = gtk_button_new_from_icon_name("edit-redo-symbolic");
    gtk_widget_set_focusable(redo_btn, FALSE);
    g_signal_connect(redo_btn, "clicked", G_CALLBACK(on_redo_clicked), state);
    gtk_box_append(GTK_BOX(box), redo_btn);

    GtkWidget *sep4 = gtk_separator_new(GTK_ORIENTATION_VERTICAL); 
    gtk_widget_add_css_class(sep4, "vertical-separator");
    gtk_box_append(GTK_BOX(box), sep4);

    GtkWidget *confirm_btn = gtk_button_new_from_icon_name("object-select-symbolic");
    gtk_widget_set_focusable(confirm_btn, FALSE); 
    gtk_widget_add_css_class(confirm_btn, "suggested-action");
    g_signal_connect(confirm_btn, "clicked", G_CALLBACK(on_confirm_clicked), state);
    gtk_box_append(GTK_BOX(box), confirm_btn);

    return frame;
}

GtkWidget* create_annotation_toolbar_bottom(UIState *state) {
    GtkWidget *frame = gtk_frame_new(NULL); 
    gtk_widget_add_css_class(frame, "panel"); 
    
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_frame_set_child(GTK_FRAME(frame), box);

    state->size_scale = gtk_spin_button_new_with_range(2.0, 40.0, 1.0);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(state->size_scale), TRUE);
    gtk_widget_set_tooltip_text(state->size_scale, "Thickness / Font Size");
    gtk_orientable_set_orientation(GTK_ORIENTABLE(state->size_scale), GTK_ORIENTATION_HORIZONTAL);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->size_scale), state->current_brush_size);
    g_signal_connect(state->size_scale, "value-changed", G_CALLBACK(on_size_spin_changed), state);
    gtk_box_append(GTK_BOX(box), state->size_scale);

    state->angle_scale = gtk_spin_button_new_with_range(-180.0, 180.0, 1.0);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(state->angle_scale), TRUE);
    gtk_widget_set_tooltip_text(state->angle_scale, "Rotation Angle");
    gtk_orientable_set_orientation(GTK_ORIENTABLE(state->angle_scale), GTK_ORIENTATION_HORIZONTAL);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->angle_scale), 0.0);
    gtk_widget_set_sensitive(state->angle_scale, FALSE); 
    g_signal_connect(state->angle_scale, "value-changed", G_CALLBACK(on_angle_spin_changed), state);
    gtk_box_append(GTK_BOX(box), state->angle_scale);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL); 
    gtk_widget_add_css_class(sep, "vertical-separator");
    gtk_box_append(GTK_BOX(box), sep);

    gtk_box_append(GTK_BOX(box), create_color_button("#ffffff", state));
    gtk_box_append(GTK_BOX(box), create_color_button("#000000", state));
    gtk_box_append(GTK_BOX(box), create_color_button("#ff3333", state));
    gtk_box_append(GTK_BOX(box), create_color_button("#33ff33", state));
    gtk_box_append(GTK_BOX(box), create_color_button("#3333ff", state));
    gtk_box_append(GTK_BOX(box), create_color_button("#ffff33", state));

    return frame;
}