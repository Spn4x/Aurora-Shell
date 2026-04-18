#include "qscreen.h"
#include "ui_state.h"
#include "utils.h"
#include "features/ocr.h"
#include "features/eyedrop.h"
#include "features/annotation_ui.h"
#include <gdk/gdkcairo.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <math.h>

// --- Forward Declarations ---
static void capture_selection_immediately(UIState *state);
static void on_widget_realize(GtkWidget *widget, gpointer user_data);
static void on_window_destroy(GtkWidget *widget, gpointer user_data);
static void set_selection_target(UIState *state, double x, double y, double w, double h);
static void draw_selection_overlay(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data);
static void on_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer data);
static void on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer data);
static void on_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer data);
static void on_mouse_motion(GtkEventControllerMotion *controller, double x, double y, gpointer data);
static void on_window_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data);
static void on_region_button_toggled(GtkToggleButton *button, gpointer user_data);
static void on_window_button_toggled(GtkToggleButton *button, gpointer user_data);
static void on_text_button_toggled(GtkToggleButton *button, gpointer user_data);
static void on_color_button_toggled(GtkToggleButton *button, gpointer user_data);
static void on_screen_button_clicked(GtkButton *button, gpointer user_data);
static void on_annotate_toggle_changed(GtkToggleButton *button, gpointer user_data);
static void ui_state_free(gpointer data);
static void on_ocr_finished(GList *text_boxes, gpointer user_data);
static gboolean hide_notification_and_redraw(gpointer user_data);
static gboolean animation_tick(gpointer data);
static void create_rounded_rect_path(cairo_t *cr, double x, double y, double w, double h, double r);
static gboolean on_key_pressed(GtkEventControllerKey* controller, guint keyval, guint keycode, GdkModifierType mod_state, gpointer user_data);
static void on_drawing_area_resize(GtkWidget *widget, int width, int height, gpointer user_data);

// ===================================================================
//  State Persistence Helpers
// ===================================================================

static char* get_state_file_path() {
    const char *cache_dir = g_get_user_cache_dir();
    return g_build_filename(cache_dir, "qscreen_state.json", NULL);
}

static gboolean get_saved_annotation_state() {
    g_autofree char *path = get_state_file_path();
    gboolean enabled = TRUE; 
    
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_autoptr(GError) error = NULL;
        g_autoptr(JsonParser) parser = json_parser_new();
        if (json_parser_load_from_file(parser, path, &error)) {
            JsonObject *root = json_node_get_object(json_parser_get_root(parser));
            if (json_object_has_member(root, "annotation_enabled")) {
                enabled = json_object_get_boolean_member(root, "annotation_enabled");
            }
        }
    }
    return enabled;
}

static void save_annotation_state(gboolean enabled) {
    g_autofree char *path = get_state_file_path();
    JsonObject *root = json_object_new();
    json_object_set_boolean_member(root, "annotation_enabled", enabled);
    
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, root);
    
    g_autoptr(JsonGenerator) gen = json_generator_new();
    json_generator_set_root(gen, node);
    json_generator_to_file(gen, path, NULL);
    json_node_free(node);
}

static void on_annotate_toggle_changed(GtkToggleButton *button, gpointer user_data) {
    (void)user_data;
    save_annotation_state(gtk_toggle_button_get_active(button));
}

// ===================================================================
//  Lifecycle & Main Callbacks
// ===================================================================

static void capture_selection_immediately(UIState *state) {
    GdkRectangle g = { 
        (int)round(state->current_x), (int)round(state->current_y), 
        (int)round(state->current_w), (int)round(state->current_h) 
    };
    gboolean save = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->save_button));
    process_final_screenshot(state->temp_screenshot_path, &g, save, state->app_state);
    if (state->window) gtk_window_destroy(state->window);
}

static void on_widget_realize(GtkWidget *widget, gpointer user_data) {
    UIState *state = (UIState*)user_data;
    state->window = GTK_WINDOW(gtk_widget_get_root(widget));
}

static void on_window_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget; (void)user_data;
    g_print("qscreen: Window destroyed, cleaning up.\n");
}

static void ui_state_free(gpointer data) {
    UIState *state = data;
    if (!state) return;
    if (state->animation_timer_id > 0) g_source_remove(state->animation_timer_id);
    g_clear_object(&state->screenshot_pixbuf);
    if (state->temp_screenshot_path) {
        g_remove(state->temp_screenshot_path);
        g_free(state->temp_screenshot_path);
    }
    if (state->window_geometries) g_list_free_full(state->window_geometries, g_free);
    if (state->text_boxes) free_ocr_results(state->text_boxes);
    g_list_free(state->selected_text_boxes);
    annotation_items_free_list(state->strokes);
    annotation_items_free_list(state->redo_strokes);
    g_list_free(state->color_indicators);
    g_free(state->app_state);
    g_free(state);
}

static void on_drawing_area_resize(GtkWidget *widget, int width, int height, gpointer user_data) {
    (void)widget; UIState *state = user_data;
    if (!state->screenshot_pixbuf || width == 0 || height == 0) return;
    state->scale_x = (double)gdk_pixbuf_get_width(state->screenshot_pixbuf) / (double)width;
    state->scale_y = (double)gdk_pixbuf_get_height(state->screenshot_pixbuf) / (double)height;
}

static gboolean animation_tick(gpointer data) {
    UIState *state = data;
    state->current_x += (state->target_x - state->current_x) * 0.3;
    state->current_y += (state->target_y - state->current_y) * 0.3;
    state->current_w += (state->target_w - state->current_w) * 0.3;
    state->current_h += (state->target_h - state->current_h) * 0.3;
    if (fabs(state->current_x - state->target_x) < 0.5 && fabs(state->current_y - state->target_y) < 0.5 && fabs(state->current_w - state->target_w) < 0.5 && fabs(state->current_h - state->target_h) < 0.5) {
        state->current_x = state->target_x; state->current_y = state->target_y; state->current_w = state->target_w; state->current_h = state->target_h;
        state->is_animating = FALSE; state->animation_timer_id = 0;
        gtk_widget_queue_draw(state->drawing_area);
        return G_SOURCE_REMOVE;
    }
    gtk_widget_queue_draw(state->drawing_area);
    return G_SOURCE_CONTINUE;
}

static void set_selection_target(UIState *state, double x, double y, double w, double h) {
    state->target_x = x; state->target_y = y; state->target_w = w; state->target_h = h;
    if (!state->is_animating) {
        state->is_animating = TRUE;
        state->animation_timer_id = g_timeout_add(16, animation_tick, state);
    }
}

static void create_rounded_rect_path(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_move_to(cr, x + r, y);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI_2, 0); cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI_2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI_2, G_PI); cairo_arc(cr, x + r, y + r, r, G_PI, G_PI * 1.5);
    cairo_close_path(cr);
}

static void draw_selection_overlay(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data) {
    (void)area; UIState *state = data;
    if (!state->screenshot_pixbuf || width == 0 || height == 0) return;

    double sel_x = state->current_x / state->scale_x;
    double sel_y = state->current_y / state->scale_y;
    double sel_w = state->current_w / state->scale_x;
    double sel_h = state->current_h / state->scale_y;

    // Background Dimming
    if (state->current_mode != MODE_TEXT && state->current_mode != MODE_COLOR) {
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
        cairo_paint(cr);
    }
    
    // Draw Window/Region selection box
    if (state->current_w > 1 && state->current_h > 1 && state->current_mode != MODE_TEXT && state->current_mode != MODE_COLOR) {
        cairo_save(cr);
        create_rounded_rect_path(cr, sel_x, sel_y, sel_w, sel_h, 10.0);
        cairo_clip(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cr);
        cairo_restore(cr);

        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_set_line_width(cr, 2.0);
        create_rounded_rect_path(cr, sel_x, sel_y, sel_w, sel_h, 10.0);
        cairo_stroke(cr);
    }

    // Draw existing annotations
    if (state->strokes) {
        cairo_save(cr);
        if (state->current_w > 1 && state->current_h > 1) {
            create_rounded_rect_path(cr, sel_x, sel_y, sel_w, sel_h, 10.0);
            cairo_clip(cr);
        }
        
        // NEW FOR PIXELATE: state->screenshot_pixbuf is passed
        annotation_draw_all(cr, state->strokes, state->screenshot_pixbuf, 0, 0, 1.0 / state->scale_x, 1.0 / state->scale_y);
        
        cairo_restore(cr);
    }

    // Draw OCR Boxes
    if (state->current_mode == MODE_TEXT && state->text_boxes) {
        cairo_set_source_rgba(cr, 0.2, 0.5, 1.0, 0.3);
        for (GList *l = state->text_boxes; l != NULL; l = l->next) {
            if (!g_list_find(state->selected_text_boxes, l->data)) {
                QScreenTextBox *box = l->data;
                cairo_rectangle(cr, box->geometry.x / state->scale_x, box->geometry.y / state->scale_y, box->geometry.width / state->scale_x, box->geometry.height / state->scale_y);
            }
        }
        cairo_fill(cr);

        cairo_set_source_rgba(cr, 0.8, 0.5, 0.1, 0.5);
        for (GList *l = state->selected_text_boxes; l != NULL; l = l->next) {
            QScreenTextBox *box = l->data;
            cairo_rectangle(cr, box->geometry.x / state->scale_x, box->geometry.y / state->scale_y, box->geometry.width / state->scale_x, box->geometry.height / state->scale_y);
        }
        cairo_fill(cr);
    }

    // Draw Eyedropper indicator
    if (state->current_mode == MODE_COLOR && state->has_hovered_color) {
        double cx = state->hover_x + 30;
        double cy = state->hover_y + 30;

        if (cx + 100 > width) cx = state->hover_x - 100;
        if (cy + 40 > height) cy = state->hover_y - 40;

        cairo_arc(cr, cx, cy, 20.0, 0, 2 * G_PI);
        cairo_set_source_rgba(cr, state->hovered_color.red, state->hovered_color.green, state->hovered_color.blue, 1.0);
        cairo_fill_preserve(cr);

        cairo_set_line_width(cr, 2.5);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_stroke_preserve(cr);
        cairo_set_line_width(cr, 1.0);
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_stroke(cr);

        g_autofree gchar *hex = NULL;
        eyedrop_get_color_at_pixel(state->screenshot_pixbuf, 
                                   (int)(state->hover_x * state->scale_x), 
                                   (int)(state->hover_y * state->scale_y), 
                                   NULL, &hex);
        
        if (hex) {
            cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 16.0);
            
            cairo_move_to(cr, cx + 28, cy + 6);
            cairo_text_path(cr, hex);
            
            cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
            cairo_set_line_width(cr, 3.0);
            cairo_stroke_preserve(cr);
            
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_fill(cr);
        }
    }
}

// ===================================================================
//  Gestures & Input
// ===================================================================

static void on_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer data) {
    (void)gesture; UIState *state = data;
    double scaled_x = x * state->scale_x;
    double scaled_y = y * state->scale_y;
    state->drag_start_x = scaled_x; 
    state->drag_start_y = scaled_y;

    if (state->is_annotating) {
        annotation_ui_drag_begin(state, scaled_x, scaled_y, x, y);
        return;
    }

    state->current_x = scaled_x; state->current_y = scaled_y;
    state->current_w = 0; state->current_h = 0;
    if (state->current_mode == MODE_TEXT) { g_list_free(state->selected_text_boxes); state->selected_text_boxes = NULL; }
}

static void on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer data) {
    (void)gesture; UIState *state = data;
    double scaled_offset_x = offset_x * state->scale_x;
    double scaled_offset_y = offset_y * state->scale_y;
    double end_x = state->drag_start_x + scaled_offset_x;
    double end_y = state->drag_start_y + scaled_offset_y;

    if (state->is_annotating) {
        annotation_ui_drag_update(state, end_x, end_y);
        return;
    }

    state->current_x = MIN(state->drag_start_x, end_x); state->current_y = MIN(state->drag_start_y, end_y);
    state->current_w = fabs(end_x - state->drag_start_x); state->current_h = fabs(end_y - state->drag_start_y);
    
    if (state->current_mode == MODE_TEXT) {
        g_list_free(state->selected_text_boxes); state->selected_text_boxes = NULL; 
        GdkRectangle selection_rect = { (int)state->current_x, (int)state->current_y, (int)state->current_w, (int)state->current_h };
        for (GList *l = state->text_boxes; l != NULL; l = l->next) { 
            QScreenTextBox *box = l->data; 
            if (gdk_rectangle_intersect(&box->geometry, &selection_rect, NULL)) 
                state->selected_text_boxes = g_list_prepend(state->selected_text_boxes, box);
        }
    } 
    gtk_widget_queue_draw(state->drawing_area);
}

static void on_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer data) {
    (void)gesture; (void)offset_x; (void)offset_y; UIState *state = data;
    
    if (state->is_annotating) return;

    if (state->current_mode == MODE_TEXT) {
        if (state->selected_text_boxes) {
            GString *final_text = g_string_new(""); 
            state->selected_text_boxes = g_list_reverse(state->selected_text_boxes);
            for (GList *l = state->selected_text_boxes; l != NULL; l = l->next) { 
                QScreenTextBox *box = l->data; 
                g_string_append(final_text, box->text); 
                g_string_append_c(final_text, ' '); 
            }
            run_command_with_stdin_sync("wl-copy", final_text->str); 
            run_command_with_stdin_sync("notify-send 'Text Copied' 'Selected text is on your clipboard.'", NULL);
            g_string_free(final_text, TRUE);
        } 
        if (state->window) gtk_window_destroy(state->window);
    } else if (state->current_mode != MODE_COLOR) { 
        if (state->current_w > 5 && state->current_h > 5) {
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->annotate_toggle_btn))) {
                enter_annotation_phase(state);
            } else {
                capture_selection_immediately(state);
            }
        } else {
            if (state->window) gtk_window_destroy(state->window);
        }
    }
}

static void on_mouse_motion(GtkEventControllerMotion *controller, double x, double y, gpointer data) {
    (void)controller; UIState *state = data; 
    if (state->is_annotating) return; 
    
    double scaled_x = x * state->scale_x;
    double scaled_y = y * state->scale_y;

    if (state->current_mode == MODE_COLOR) {
        state->hover_x = x; 
        state->hover_y = y;
        state->has_hovered_color = eyedrop_get_color_at_pixel(state->screenshot_pixbuf, (int)scaled_x, (int)scaled_y, &state->hovered_color, NULL);
        gtk_widget_queue_draw(state->drawing_area);
        return;
    }

    if (state->current_mode == MODE_WINDOW) {
        gboolean found_window = FALSE;
        for (GList *l = state->window_geometries; l != NULL; l = l->next) { 
            GdkRectangle *rect = l->data; 
            if (scaled_x >= rect->x && scaled_x <= (rect->x + rect->width) && scaled_y >= rect->y && scaled_y <= (rect->y + rect->height)) { 
                set_selection_target(state, rect->x, rect->y, rect->width, rect->height); 
                found_window = TRUE; 
                break; 
            } 
        }
        if (!found_window) set_selection_target(state, scaled_x, scaled_y, 0, 0);
    }
    
    if (state->current_mode == MODE_TEXT) {
        gboolean hovering_text = FALSE;
        for (GList *l = state->text_boxes; l != NULL; l = l->next) {
            QScreenTextBox *box = l->data;
            if (scaled_x >= box->geometry.x && scaled_x <= (box->geometry.x + box->geometry.width) &&
                scaled_y >= box->geometry.y && scaled_y <= (box->geometry.y + box->geometry.height)) {
                hovering_text = TRUE;
                break;
            }
        }
        gtk_widget_set_cursor_from_name(state->drawing_area, hovering_text ? "text" : "default");
    }
}

static void on_window_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data) {
    (void)gesture; (void)n_press; UIState *state = data;
    
    if (state->current_mode == MODE_COLOR) {
        double scaled_x = x * state->scale_x;
        double scaled_y = y * state->scale_y;
        g_autofree gchar *hex_code = NULL;

        if (eyedrop_get_color_at_pixel(state->screenshot_pixbuf, (int)scaled_x, (int)scaled_y, NULL, &hex_code)) {
            run_command_with_stdin_sync("wl-copy", hex_code);
            g_autofree char *msg = g_strdup_printf("notify-send 'Color Picked' 'Copied %s to clipboard.'", hex_code);
            run_command_with_stdin_sync(msg, NULL);
        }
        if (state->window) gtk_window_destroy(state->window);
        return;
    }

    if (state->current_mode == MODE_WINDOW && !state->is_annotating && state->current_w > 0 && state->current_h > 0) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->annotate_toggle_btn))) {
            enter_annotation_phase(state);
        } else {
            capture_selection_immediately(state);
        }
    }
}

static gboolean on_key_pressed(GtkEventControllerKey* controller, guint keyval, guint keycode, GdkModifierType mod_state, gpointer user_data) {
    (void)controller; (void)keycode; UIState *state = user_data;
    
    if (annotation_ui_handle_key(state, keyval, mod_state)) return TRUE;

    if (keyval == GDK_KEY_Escape && !state->is_annotating) {
        if (state->window) gtk_window_destroy(state->window);
        return TRUE;
    }
    return FALSE;
}

// ===================================================================
//  Phase 1 Tools & OCR
// ===================================================================

void qscreen_set_mode(UIState *state, SelectionMode mode) {
    state->current_mode = mode;
    
    gboolean needs_motion_click = (mode == MODE_WINDOW || mode == MODE_COLOR || mode == MODE_TEXT);
    gboolean needs_drag = (mode == MODE_REGION || mode == MODE_TEXT);

    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(state->motion_controller), needs_motion_click ? GTK_PHASE_CAPTURE : GTK_PHASE_NONE);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(state->click_gesture), needs_motion_click ? GTK_PHASE_CAPTURE : GTK_PHASE_NONE);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(state->drag_gesture), needs_drag ? GTK_PHASE_CAPTURE : GTK_PHASE_NONE);
    
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->region_button), mode == MODE_REGION);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->window_button), mode == MODE_WINDOW);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->text_button), mode == MODE_TEXT);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->color_button), mode == MODE_COLOR);

    state->has_hovered_color = FALSE; 
    
    if (mode != MODE_TEXT) {
        gtk_widget_set_cursor_from_name(state->drawing_area, "default");
    }

    if(mode == MODE_TEXT && !state->ocr_has_run){
        state->ocr_has_run = TRUE;
        gtk_stack_set_visible_child_name(GTK_STACK(state->ocr_notification_stack),"scanning");
        gtk_revealer_set_reveal_child(GTK_REVEALER(state->ocr_notification_revealer),TRUE);
        run_ocr_on_screenshot_async(state->temp_screenshot_path, on_ocr_finished, state);
    } else {
        gtk_widget_queue_draw(state->drawing_area);
    }
}

static void on_region_button_toggled(GtkToggleButton *b, gpointer d) { if(gtk_toggle_button_get_active(b)){ UIState *s=d; set_selection_target(s, s->current_x+s->current_w/2, s->current_y+s->current_h/2,0,0); qscreen_set_mode(s,MODE_REGION); }}
static void on_window_button_toggled(GtkToggleButton *b, gpointer d) { if(gtk_toggle_button_get_active(b)){ UIState *s=d; set_selection_target(s, s->current_x+s->current_w/2, s->current_y+s->current_h/2,0,0); qscreen_set_mode(s,MODE_WINDOW); }}
static void on_text_button_toggled(GtkToggleButton *b, gpointer d) { if(gtk_toggle_button_get_active(b)){ UIState *s=d; set_selection_target(s, s->current_x+s->current_w/2, s->current_y+s->current_h/2,0,0); qscreen_set_mode(s,MODE_TEXT); }}
static void on_color_button_toggled(GtkToggleButton *b, gpointer d) { if(gtk_toggle_button_get_active(b)){ UIState *s=d; set_selection_target(s, s->current_x+s->current_w/2, s->current_y+s->current_h/2,0,0); qscreen_set_mode(s,MODE_COLOR); }}

static void on_screen_button_clicked(GtkButton *b, gpointer d) { 
    (void)b; UIState *s=d; 
    
    s->current_x = 0;
    s->current_y = 0;
    s->current_w = gdk_pixbuf_get_width(s->screenshot_pixbuf);
    s->current_h = gdk_pixbuf_get_height(s->screenshot_pixbuf);

    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->annotate_toggle_btn))) {
        enter_annotation_phase(s);
    } else {
        capture_selection_immediately(s);
    }
}

static gboolean hide_notification_and_redraw(gpointer d) { UIState *s=d; gtk_revealer_set_reveal_child(GTK_REVEALER(s->ocr_notification_revealer),FALSE); gtk_widget_queue_draw(s->drawing_area); return G_SOURCE_REMOVE; }
static void on_ocr_finished(GList *text_boxes, gpointer data) {
    UIState *state = data; if (!state || !GTK_IS_WIDGET(state->window) || !gtk_widget_get_visible(GTK_WIDGET(state->window))) { if (text_boxes) free_ocr_results(text_boxes); return; }
    state->text_boxes = text_boxes; gtk_stack_set_visible_child_name(GTK_STACK(state->ocr_notification_stack), "done"); g_timeout_add(750, hide_notification_and_redraw, state);
}

// ===================================================================
//  Plugin Entry Point
// ===================================================================

G_MODULE_EXPORT GtkWidget* create_widget(const char *config_string) {
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!config_string || !json_parser_load_from_data(parser, config_string, -1, NULL)) {
        GtkWidget *dummy = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); gtk_widget_set_size_request(dummy, 1, 1); gtk_widget_set_opacity(dummy, 0.0); return dummy;
    }
    
    JsonObject *root_obj = json_node_get_object(json_parser_get_root(parser));
    if (!json_object_has_member(root_obj, "temp_screenshot_path")) {
        GtkWidget *dummy = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); gtk_widget_set_size_request(dummy, 1, 1); gtk_widget_set_opacity(dummy, 0.0); return dummy;
    }
    
    UIState *state = g_new0(UIState, 1);
    state->app_state = g_new0(QScreenState, 1);
    state->temp_screenshot_path = g_strdup(json_object_get_string_member(root_obj, "temp_screenshot_path"));
    state->scale_x = 1.0; state->scale_y = 1.0;
    state->current_brush_size = 6.0;
    state->current_font_size = 32.0;
    state->has_hovered_color = FALSE; 

    if (json_object_has_member(root_obj, "config")) {
        JsonObject *config = json_object_get_object_member(root_obj, "config");
        const char *mode_str = json_object_get_string_member_with_default(config, "mode", "region");
        if (g_strcmp0(mode_str, "text") == 0) state->app_state->initial_mode = QSCREEN_MODE_TEXT;
        else if (g_strcmp0(mode_str, "window") == 0) state->app_state->initial_mode = QSCREEN_MODE_WINDOW;
        else state->app_state->initial_mode = QSCREEN_MODE_REGION;
        state->app_state->save_on_launch = json_object_get_boolean_member_with_default(config, "save", FALSE);
    }
    
    g_autoptr(GError) error = NULL;
    state->screenshot_pixbuf = gdk_pixbuf_new_from_file(state->temp_screenshot_path, &error);
    if (error) { ui_state_free(state); GtkWidget *dummy = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); gtk_widget_set_size_request(dummy, 1, 1); gtk_widget_set_opacity(dummy, 0.0); return dummy; }

    GtkAspectFrame *aspect_frame = GTK_ASPECT_FRAME(gtk_aspect_frame_new(0.5, 0.5, (float)gdk_pixbuf_get_width(state->screenshot_pixbuf) / (float)gdk_pixbuf_get_height(state->screenshot_pixbuf), FALSE));
    GtkWidget *overlay = gtk_overlay_new();
    gtk_aspect_frame_set_child(aspect_frame, overlay);
    gtk_widget_set_name(GTK_WIDGET(aspect_frame), "qscreen-widget");
    g_object_set_data_full(G_OBJECT(aspect_frame), "ui-state", state, ui_state_free);
    
    GtkWidget *picture = gtk_picture_new_for_filename(state->temp_screenshot_path);
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_FILL);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), picture);

    state->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_focusable(state->drawing_area, TRUE); 
    g_signal_connect(state->drawing_area, "resize", G_CALLBACK(on_drawing_area_resize), state);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(state->drawing_area), draw_selection_overlay, state, NULL);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), state->drawing_area);

    state->annotation_fixed = gtk_fixed_new();
    gtk_widget_set_can_target(state->annotation_fixed, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), state->annotation_fixed);

    g_signal_connect(GTK_WIDGET(aspect_frame), "unrealize", G_CALLBACK(on_window_destroy), NULL);
    GtkEventController *key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_pressed), state);
    gtk_widget_add_controller(GTK_WIDGET(aspect_frame), key_controller);
    
    // Notifications
    state->ocr_notification_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(state->ocr_notification_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(state->ocr_notification_revealer), 250);
    GtkWidget *notification_container = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); gtk_widget_add_css_class(notification_container, "ocr-notification");
    state->ocr_notification_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(state->ocr_notification_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_UP);
    GtkWidget *scanning_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); GtkWidget *spinner = gtk_spinner_new(); gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_box_append(GTK_BOX(scanning_content), spinner); gtk_box_append(GTK_BOX(scanning_content), gtk_label_new("Scanning for text..."));
    GtkWidget *done_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); gtk_box_append(GTK_BOX(done_content), gtk_label_new("\u2713 Done!"));
    gtk_stack_add_named(GTK_STACK(state->ocr_notification_stack), scanning_content, "scanning"); gtk_stack_add_named(GTK_STACK(state->ocr_notification_stack), done_content, "done");
    gtk_box_append(GTK_BOX(notification_container), state->ocr_notification_stack); gtk_revealer_set_child(GTK_REVEALER(state->ocr_notification_revealer), notification_container);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), state->ocr_notification_revealer);
    gtk_widget_set_valign(state->ocr_notification_revealer, GTK_ALIGN_START); gtk_widget_set_halign(state->ocr_notification_revealer, GTK_ALIGN_CENTER);

    // Phase 1 Toolbar
    state->bottom_panel_revealer = GTK_REVEALER(gtk_revealer_new());
    gtk_revealer_set_transition_type(state->bottom_panel_revealer, GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    GtkWidget *panel_frame = gtk_frame_new(NULL); gtk_widget_add_css_class(panel_frame, "panel");
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(main_box, 15); gtk_widget_set_margin_end(main_box, 15); gtk_widget_set_margin_top(main_box, 15); gtk_widget_set_margin_bottom(main_box, 15);
    gtk_frame_set_child(GTK_FRAME(panel_frame), main_box);
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    
    state->region_button = gtk_toggle_button_new(); gtk_button_set_child(GTK_BUTTON(state->region_button), gtk_image_new_from_icon_name("image-x-generic-symbolic"));
    state->window_button = gtk_toggle_button_new(); gtk_button_set_child(GTK_BUTTON(state->window_button), gtk_image_new_from_icon_name("window-new-symbolic"));
    state->text_button = gtk_toggle_button_new(); gtk_button_set_child(GTK_BUTTON(state->text_button), gtk_image_new_from_icon_name("edit-find-symbolic"));
    state->color_button = gtk_toggle_button_new(); gtk_button_set_child(GTK_BUTTON(state->color_button), gtk_image_new_from_icon_name("color-select-symbolic"));

    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(state->window_button), GTK_TOGGLE_BUTTON(state->region_button));
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(state->text_button), GTK_TOGGLE_BUTTON(state->region_button));
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(state->color_button), GTK_TOGGLE_BUTTON(state->region_button));
    
    state->screen_button = gtk_button_new_from_icon_name("video-display-symbolic");
    state->save_button = gtk_toggle_button_new(); gtk_button_set_child(GTK_BUTTON(state->save_button), gtk_image_new_from_icon_name("document-save-symbolic"));
    gtk_widget_add_css_class(state->save_button, "save-button");

    state->annotate_toggle_btn = gtk_toggle_button_new(); 
    gtk_button_set_child(GTK_BUTTON(state->annotate_toggle_btn), gtk_image_new_from_icon_name("document-edit-symbolic"));
    gtk_widget_set_tooltip_text(state->annotate_toggle_btn, "Annotate before capturing");
    
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->annotate_toggle_btn), get_saved_annotation_state());
    g_signal_connect(state->annotate_toggle_btn, "toggled", G_CALLBACK(on_annotate_toggle_changed), NULL);

    gtk_widget_set_focusable(state->region_button, FALSE);
    gtk_widget_set_focusable(state->window_button, FALSE);
    gtk_widget_set_focusable(state->text_button, FALSE);
    gtk_widget_set_focusable(state->color_button, FALSE);
    gtk_widget_set_focusable(state->screen_button, FALSE);
    gtk_widget_set_focusable(state->save_button, FALSE);
    gtk_widget_set_focusable(state->annotate_toggle_btn, FALSE);
    
    gtk_box_append(GTK_BOX(button_box), state->region_button); 
    gtk_box_append(GTK_BOX(button_box), state->window_button);
    gtk_box_append(GTK_BOX(button_box), state->text_button); 
    gtk_box_append(GTK_BOX(button_box), state->color_button); 
    gtk_box_append(GTK_BOX(button_box), state->screen_button);
    
    GtkWidget *separator1 = gtk_separator_new(GTK_ORIENTATION_VERTICAL); gtk_widget_add_css_class(separator1, "vertical-separator");
    gtk_box_append(GTK_BOX(button_box), separator1); gtk_box_append(GTK_BOX(button_box), state->annotate_toggle_btn);
    GtkWidget *separator2 = gtk_separator_new(GTK_ORIENTATION_VERTICAL); gtk_widget_add_css_class(separator2, "vertical-separator");
    gtk_box_append(GTK_BOX(button_box), separator2); gtk_box_append(GTK_BOX(button_box), state->save_button);
    gtk_box_append(GTK_BOX(main_box), button_box);
    
    gtk_revealer_set_child(state->bottom_panel_revealer, panel_frame);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), GTK_WIDGET(state->bottom_panel_revealer));
    gtk_widget_set_valign(GTK_WIDGET(state->bottom_panel_revealer), GTK_ALIGN_END); 
    gtk_widget_set_halign(GTK_WIDGET(state->bottom_panel_revealer), GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(GTK_WIDGET(state->bottom_panel_revealer), 40);
    gtk_revealer_set_reveal_child(state->bottom_panel_revealer, TRUE);

    // Phase 2 Toolbar 
    state->top_panel_revealer = GTK_REVEALER(gtk_revealer_new());
    gtk_revealer_set_transition_type(state->top_panel_revealer, GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    GtkWidget *annotation_toolbar = create_annotation_toolbar(state);
    gtk_revealer_set_child(state->top_panel_revealer, annotation_toolbar);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), GTK_WIDGET(state->top_panel_revealer));
    gtk_widget_set_valign(GTK_WIDGET(state->top_panel_revealer), GTK_ALIGN_START);
    gtk_widget_set_halign(GTK_WIDGET(state->top_panel_revealer), GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(GTK_WIDGET(state->top_panel_revealer), 15);

    // Gestures
    g_signal_connect(state->region_button, "toggled", G_CALLBACK(on_region_button_toggled), state);
    g_signal_connect(state->window_button, "toggled", G_CALLBACK(on_window_button_toggled), state);
    g_signal_connect(state->text_button, "toggled", G_CALLBACK(on_text_button_toggled), state);
    g_signal_connect(state->color_button, "toggled", G_CALLBACK(on_color_button_toggled), state); 
    g_signal_connect(state->screen_button, "clicked", G_CALLBACK(on_screen_button_clicked), state);
    
    state->motion_controller = gtk_event_controller_motion_new();
    g_signal_connect(state->motion_controller, "motion", G_CALLBACK(on_mouse_motion), state);
    gtk_widget_add_controller(state->drawing_area, state->motion_controller);
    state->click_gesture = gtk_gesture_click_new();
    g_signal_connect(state->click_gesture, "pressed", G_CALLBACK(on_window_click), state);
    gtk_widget_add_controller(state->drawing_area, GTK_EVENT_CONTROLLER(state->click_gesture));
    state->drag_gesture = gtk_gesture_drag_new();
    g_signal_connect(state->drag_gesture, "drag-begin", G_CALLBACK(on_drag_begin), state);
    g_signal_connect(state->drag_gesture, "drag-update", G_CALLBACK(on_drag_update), state);
    g_signal_connect(state->drag_gesture, "drag-end", G_CALLBACK(on_drag_end), state);
    gtk_widget_add_controller(state->drawing_area, GTK_EVENT_CONTROLLER(state->drag_gesture));

    state->window_geometries = get_hyprland_windows_geometry(state->app_state);
    
    qscreen_set_mode(state, (SelectionMode)state->app_state->initial_mode);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->save_button), state->app_state->save_on_launch);
    g_signal_connect(GTK_WIDGET(aspect_frame), "realize", G_CALLBACK(on_widget_realize), state);
    
    return GTK_WIDGET(aspect_frame);
}