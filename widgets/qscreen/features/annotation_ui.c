#include "annotation_ui.h"
#include "utils.h"
#include <math.h>

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
    
    const char *text = gtk_editable_get_text(GTK_EDITABLE(state->active_text_entry));
    if (text && strlen(text) > 0) {
        AnnotationItem *item = annotation_text_new(&state->current_color, text, state->active_text_x, state->active_text_y, state->current_font_size);
        state->strokes = g_list_append(state->strokes, item);
        
        if (state->redo_strokes) {
            annotation_items_free_list(state->redo_strokes);
            state->redo_strokes = NULL;
        }
    }
    gtk_fixed_remove(GTK_FIXED(state->annotation_fixed), state->active_text_entry);
    state->active_text_entry = NULL;
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
    gtk_revealer_set_reveal_child(state->bottom_panel_revealer, FALSE);
    gtk_revealer_set_reveal_child(state->top_panel_revealer, TRUE);
    
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

    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(state->drag_gesture), GTK_PHASE_CAPTURE);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(state->motion_controller), GTK_PHASE_NONE);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(state->click_gesture), GTK_PHASE_NONE);

    gtk_widget_queue_draw(state->drawing_area);
    gtk_widget_grab_focus(state->drawing_area);
}

void cancel_annotation_phase(UIState *state) {
    finalize_text_entry(state); 
    state->is_annotating = FALSE;
    gtk_revealer_set_reveal_child(state->top_panel_revealer, FALSE);
    gtk_revealer_set_reveal_child(state->bottom_panel_revealer, TRUE);
    
    annotation_items_free_list(state->strokes); state->strokes = NULL;
    annotation_items_free_list(state->redo_strokes); state->redo_strokes = NULL;
    
    qscreen_set_mode(state, state->current_mode);
    gtk_widget_queue_draw(state->drawing_area);
    gtk_widget_grab_focus(state->drawing_area);
}

static void annotation_ui_undo(UIState *state) {
    finalize_text_entry(state);
    if (!state->strokes) return; 

    GList *last = g_list_last(state->strokes);
    state->redo_strokes = g_list_prepend(state->redo_strokes, last->data);
    state->strokes = g_list_delete_link(state->strokes, last);
    gtk_widget_queue_draw(state->drawing_area);
}

static void annotation_ui_redo(UIState *state) {
    finalize_text_entry(state);
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

void annotation_ui_drag_begin(UIState *state, double scaled_x, double scaled_y, double raw_x, double raw_y) {
    finalize_text_entry(state);

    if (state->current_ann_mode == ANN_MODE_TEXT) {
        state->active_text_x = scaled_x;
        state->active_text_y = scaled_y;
        
        state->active_text_entry = gtk_entry_new();
        gtk_entry_set_has_frame(GTK_ENTRY(state->active_text_entry), FALSE);
        gtk_widget_add_css_class(state->active_text_entry, "annotation-text-entry"); 

        update_active_text_appearance(state);
        gtk_editable_set_text(GTK_EDITABLE(state->active_text_entry), "Enter text");
        gtk_editable_select_region(GTK_EDITABLE(state->active_text_entry), 0, -1);
        resize_text_entry(state);

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
        } else if (state->current_ann_mode == ANN_MODE_PIXELATE) { // NEW
            state->current_stroke = annotation_shape_new(ANNOTATION_PIXELATE, &state->current_color, state->current_brush_size, scaled_x, scaled_y);
        }
        
        state->strokes = g_list_append(state->strokes, state->current_stroke);
    }
}

void annotation_ui_drag_update(UIState *state, double scaled_x, double scaled_y) {
    if (!state->current_stroke) return;
    
    if (state->current_ann_mode == ANN_MODE_DRAW) {
        annotation_stroke_add_point(state->current_stroke, scaled_x, scaled_y);
    } 
    // Trigger shape update for PIXELATE as well
    else if (state->current_ann_mode == ANN_MODE_RECTANGLE || state->current_ann_mode == ANN_MODE_CIRCLE || state->current_ann_mode == ANN_MODE_ARROW || state->current_ann_mode == ANN_MODE_PIXELATE) {
        annotation_shape_update(state->current_stroke, scaled_x, scaled_y);
    }
    gtk_widget_queue_draw(state->drawing_area);
}

gboolean annotation_ui_handle_key(UIState *state, guint keyval, GdkModifierType mod_state) {
    if (!state->is_annotating) return FALSE;

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

// UI Building
static void on_size_spin_changed(GtkSpinButton *spin, gpointer user_data) {
    UIState *state = user_data;
    double val = gtk_spin_button_get_value(spin);
    if (state->current_ann_mode == ANN_MODE_TEXT) {
        state->current_font_size = val;
        update_active_text_appearance(state);
    } else {
        state->current_brush_size = val;
    }
}

static void on_ann_mode_toggled(GtkToggleButton *btn, gpointer user_data) {
    if (!gtk_toggle_button_get_active(btn)) return;
    UIState *state = user_data;
    finalize_text_entry(state);
    
    g_signal_handlers_block_by_func(state->size_scale, on_size_spin_changed, state);
    
    if (GTK_WIDGET(btn) == state->ann_text_btn) {
        state->current_ann_mode = ANN_MODE_TEXT;
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(state->size_scale), 12.0, 120.0);
        gtk_spin_button_set_increments(GTK_SPIN_BUTTON(state->size_scale), 2.0, 10.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->size_scale), state->current_font_size);
    } else {
        if (GTK_WIDGET(btn) == state->ann_draw_btn) state->current_ann_mode = ANN_MODE_DRAW;
        else if (GTK_WIDGET(btn) == state->ann_rect_btn) state->current_ann_mode = ANN_MODE_RECTANGLE;
        else if (GTK_WIDGET(btn) == state->ann_circle_btn) state->current_ann_mode = ANN_MODE_CIRCLE;
        else if (GTK_WIDGET(btn) == state->ann_arrow_btn) state->current_ann_mode = ANN_MODE_ARROW;
        else if (GTK_WIDGET(btn) == state->ann_pixelate_btn) state->current_ann_mode = ANN_MODE_PIXELATE; // NEW

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
    gtk_widget_set_focusable(btn, FALSE);
    gtk_widget_add_css_class(btn, "color-btn");
    
    GdkRGBA *rgba = g_new(GdkRGBA, 1);
    gdk_rgba_parse(rgba, color_hex);
    g_object_set_data_full(G_OBJECT(btn), "color-rgba", rgba, g_free);
    
    GtkCssProvider *p = gtk_css_provider_new();
    g_autofree char *css = g_strdup_printf("button.color-btn { background-color: rgb(%d,%d,%d); }", 
        (int)(rgba->red*255), (int)(rgba->green*255), (int)(rgba->blue*255));
    gtk_css_provider_load_from_string(p, css);
    gtk_style_context_add_provider(gtk_widget_get_style_context(btn), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);

    state->color_indicators = g_list_append(state->color_indicators, btn);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_color_button_clicked), state);
    return btn;
}

GtkWidget* create_annotation_toolbar(UIState *state) {
    GtkWidget *frame = gtk_frame_new(NULL); 
    gtk_widget_add_css_class(frame, "panel"); 
    
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12); 
    gtk_widget_set_margin_top(box, 6); gtk_widget_set_margin_bottom(box, 6); 
    gtk_frame_set_child(GTK_FRAME(frame), box);

    GtkWidget *cancel_btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_focusable(cancel_btn, FALSE); 
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_cancel_clicked), state);
    gtk_box_append(GTK_BOX(box), cancel_btn);

    GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_VERTICAL); 
    gtk_widget_add_css_class(sep1, "vertical-separator");
    gtk_box_append(GTK_BOX(box), sep1);

    // Tools
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

    // NEW: Pixelate Tool Button (view-conceal-symbolic is standard for hiding things)
    state->ann_pixelate_btn = gtk_toggle_button_new(); 
    gtk_widget_set_focusable(state->ann_pixelate_btn, FALSE); 
    gtk_button_set_child(GTK_BUTTON(state->ann_pixelate_btn), gtk_image_new_from_icon_name("view-conceal-symbolic"));

    state->ann_text_btn = gtk_toggle_button_new(); 
    gtk_widget_set_focusable(state->ann_text_btn, FALSE); 
    gtk_button_set_child(GTK_BUTTON(state->ann_text_btn), gtk_image_new_from_icon_name("insert-text-symbolic"));
    
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(state->ann_rect_btn), GTK_TOGGLE_BUTTON(state->ann_draw_btn));
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(state->ann_circle_btn), GTK_TOGGLE_BUTTON(state->ann_draw_btn));
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(state->ann_arrow_btn), GTK_TOGGLE_BUTTON(state->ann_draw_btn)); 
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(state->ann_pixelate_btn), GTK_TOGGLE_BUTTON(state->ann_draw_btn)); // LINK NEW BUTTON
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(state->ann_text_btn), GTK_TOGGLE_BUTTON(state->ann_draw_btn));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ann_draw_btn), TRUE);
    state->current_ann_mode = ANN_MODE_DRAW;
    
    g_signal_connect(state->ann_draw_btn, "toggled", G_CALLBACK(on_ann_mode_toggled), state);
    g_signal_connect(state->ann_rect_btn, "toggled", G_CALLBACK(on_ann_mode_toggled), state);
    g_signal_connect(state->ann_circle_btn, "toggled", G_CALLBACK(on_ann_mode_toggled), state);
    g_signal_connect(state->ann_arrow_btn, "toggled", G_CALLBACK(on_ann_mode_toggled), state); 
    g_signal_connect(state->ann_pixelate_btn, "toggled", G_CALLBACK(on_ann_mode_toggled), state); // LINK NEW BUTTON
    g_signal_connect(state->ann_text_btn, "toggled", G_CALLBACK(on_ann_mode_toggled), state);

    gtk_box_append(GTK_BOX(box), state->ann_draw_btn);
    gtk_box_append(GTK_BOX(box), state->ann_rect_btn);
    gtk_box_append(GTK_BOX(box), state->ann_circle_btn);
    gtk_box_append(GTK_BOX(box), state->ann_arrow_btn); 
    gtk_box_append(GTK_BOX(box), state->ann_pixelate_btn); // APPEND NEW BUTTON
    gtk_box_append(GTK_BOX(box), state->ann_text_btn);

    state->size_scale = gtk_spin_button_new_with_range(2.0, 40.0, 1.0);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(state->size_scale), TRUE);
    gtk_widget_set_size_request(state->size_scale, 60, -1);
    gtk_widget_set_valign(state->size_scale, GTK_ALIGN_CENTER);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->size_scale), state->current_brush_size);
    g_signal_connect(state->size_scale, "value-changed", G_CALLBACK(on_size_spin_changed), state);
    gtk_box_append(GTK_BOX(box), state->size_scale);

    GtkWidget *sep2 = gtk_separator_new(GTK_ORIENTATION_VERTICAL); 
    gtk_widget_add_css_class(sep2, "vertical-separator");
    gtk_box_append(GTK_BOX(box), sep2);

    // Colors
    gtk_box_append(GTK_BOX(box), create_color_button("#ffffff", state));
    gtk_box_append(GTK_BOX(box), create_color_button("#000000", state));
    gtk_box_append(GTK_BOX(box), create_color_button("#ff3333", state));
    gtk_box_append(GTK_BOX(box), create_color_button("#33ff33", state));
    gtk_box_append(GTK_BOX(box), create_color_button("#3333ff", state));
    gtk_box_append(GTK_BOX(box), create_color_button("#ffff33", state));

    GtkWidget *sep3 = gtk_separator_new(GTK_ORIENTATION_VERTICAL); 
    gtk_widget_add_css_class(sep3, "vertical-separator");
    gtk_box_append(GTK_BOX(box), sep3);

    // Undo / Redo
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

    // Confirm
    GtkWidget *confirm_btn = gtk_button_new_from_icon_name("object-select-symbolic");
    gtk_widget_set_focusable(confirm_btn, FALSE); 
    gtk_widget_add_css_class(confirm_btn, "suggested-action");
    g_signal_connect(confirm_btn, "clicked", G_CALLBACK(on_confirm_clicked), state);
    gtk_box_append(GTK_BOX(box), confirm_btn);

    return frame;
}