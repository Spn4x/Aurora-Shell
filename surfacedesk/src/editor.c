// FILE: ./src/editor.c
#include "editor.h"
#include "storage.h"
#include "widget_template.h"
#include "drawer.h"
#include "../scenes/registry.h"
#include <math.h>

static int last_view_w = 0;
static int last_view_h = 0;
static gint64 last_drag_time = 0; 

static int snap(double px) { 
    return (int)round(px / (double)app_state.physics.cell_size); 
}

void transform_coords_from_window(double *x, double *y) {
    PhysicsContext *ctx = &app_state.physics;
    *x -= ctx->current_offset_x;
    *y -= ctx->current_offset_y;
}

void update_widget_geometry_safe(Box *b) {
    if (app_state.physics.fixed_container && b->widget) 
        update_widget_geometry(b, app_state.physics.fixed_container);
}

static gboolean is_overlapping(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2) { 
    return !(x1 >= x2 + w2 || x1 + w1 <= x2 || y1 >= y2 + h2 || y1 + h1 <= y2); 
}

static gboolean check_collision(PhysicsContext *ctx, Box *ignore_me, int x, int y, int w, int h) {
    for (GList *l = ctx->boxes; l != NULL; l = l->next) {
        Box *other = (Box *)l->data;
        if (other == ignore_me) continue;
        if (is_overlapping(x, y, w, h, other->grid_x, other->grid_y, other->grid_w, other->grid_h)) return TRUE;
    }
    return FALSE;
}

static HandleType hit_test(Box *b, double x, double y) {
    double l = b->vis_x; double r = b->vis_x + b->vis_w; 
    double t = b->vis_y; double b_y = b->vis_y + b->vis_h;
    
    double hs = HANDLE_SIZE;
    if (fabs(x - l) < hs && fabs(y - t) < hs) return HANDLE_TL;
    if (fabs(x - r) < hs && fabs(y - t) < hs) return HANDLE_TR;
    if (fabs(x - l) < hs && fabs(y - b_y) < hs) return HANDLE_BL;
    if (fabs(x - r) < hs && fabs(y - b_y) < hs) return HANDLE_BR;
    
    if (x > l && x < r && y > t && y < b_y) return HANDLE_MOVE;
    return HANDLE_NONE;
}

static double lerp(double current, double target) {
    if (fabs(target - current) < SNAP_EPSILON) return target; 
    return current + (target - current) * ANIM_SPEED;
}

void recalculate_targets(int w, int h) {
    PhysicsContext *ctx = &app_state.physics;
    int win_w = (w > 0) ? w : (ctx->overlay_draw ? gtk_widget_get_width(ctx->overlay_draw) : 0);
    int win_h = (h > 0) ? h : (ctx->overlay_draw ? gtk_widget_get_height(ctx->overlay_draw) : 0);
    if (win_w < 1 || win_h < 1) return;
    
    int cell = ctx->cell_size;
    int cols = floor((double)win_w / cell);
    int rows = floor((double)win_h / cell);
    if (cols <= 0) cols = 1;
    if (rows <= 0) rows = 1;
    
    ctx->target_offset_x = (double)(win_w - (cols * cell)) / 2.0;
    ctx->target_offset_y = (double)(win_h - (rows * cell)) / 2.0;

    for (GList *l = ctx->boxes; l != NULL; l = l->next) {
        Box *b = (Box*)l->data;
        b->target_x = (b->grid_x * cell) + b->nudge_x;
        b->target_y = (b->grid_y * cell) + b->nudge_y;
        b->target_w = b->grid_w * cell;
        b->target_h = b->grid_h * cell;
    }
}

void editor_force_snap(void) {
    recalculate_targets(-1, -1);
    PhysicsContext *ctx = &app_state.physics;
    ctx->current_offset_x = ctx->target_offset_x;
    ctx->current_offset_y = ctx->target_offset_y;
    for (GList *l = ctx->boxes; l != NULL; l = l->next) {
        Box *b = (Box*)l->data;
        b->vis_x = b->target_x; b->vis_y = b->target_y;
        b->vis_w = b->target_w; b->vis_h = b->target_h;
        update_widget_geometry_safe(b);
    }
    if (app_state.is_editing && ctx->overlay_draw) gtk_widget_queue_draw(ctx->overlay_draw);
}

static gboolean on_animation_tick(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data) {
    PhysicsContext *ctx = &app_state.physics;
    if (app_state.is_picking_wallpaper) { ctx->tick_id = 0; return G_SOURCE_REMOVE; }

    gboolean keep_running = FALSE;
    ctx->current_offset_x = lerp(ctx->current_offset_x, ctx->target_offset_x);
    ctx->current_offset_y = lerp(ctx->current_offset_y, ctx->target_offset_y);
    if (fabs(ctx->target_offset_x - ctx->current_offset_x) > SNAP_EPSILON) keep_running = TRUE;
    if (fabs(ctx->target_offset_y - ctx->current_offset_y) > SNAP_EPSILON) keep_running = TRUE;

    for (GList *l = ctx->boxes; l != NULL; l = l->next) {
        Box *b = (Box*)l->data;
        if (!(ctx->is_dragging && ctx->active_box == b && ctx->active_handle == HANDLE_MOVE)) {
            b->vis_x = lerp(b->vis_x, b->target_x);
            b->vis_y = lerp(b->vis_y, b->target_y);
        }
        b->vis_w = lerp(b->vis_w, b->target_w);
        b->vis_h = lerp(b->vis_h, b->target_h);
        
        update_widget_geometry_safe(b);
        
        if (fabs(b->target_x - b->vis_x) > SNAP_EPSILON || fabs(b->target_y - b->vis_y) > SNAP_EPSILON) 
            keep_running = TRUE;
    }
    
    if (app_state.is_editing && ctx->overlay_draw) gtk_widget_queue_draw(ctx->overlay_draw);
    if (!keep_running) { ctx->tick_id = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

void start_animation(void) {
    PhysicsContext *ctx = &app_state.physics;
    if (ctx->tick_id == 0 && ctx->overlay_draw) {
        ctx->tick_id = gtk_widget_add_tick_callback(ctx->overlay_draw, on_animation_tick, NULL, NULL);
    }
}

void editor_on_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer user_data) {
    if (!app_state.is_editing) return;
    PhysicsContext *ctx = &app_state.physics;

    double wx = x; double wy = y;
    transform_coords_from_window(&wx, &wy);

    HandleType hover_handle = HANDLE_NONE;
    for (GList *l = g_list_last(ctx->boxes); l != NULL; l = l->prev) {
        Box *b = (Box *)l->data;
        if ((hover_handle = hit_test(b, wx, wy)) != HANDLE_NONE) break;
    }

    // FIX: Only send cursor updates to Wayland when the cursor *actually* needs to change.
    // This stops the massive IPC flood that was destroying your framerate.
    static HandleType last_handle = -1;
    if (hover_handle != last_handle) {
        last_handle = hover_handle;
        
        GdkCursor *c = ctx->default_cursor;
        if (hover_handle == HANDLE_MOVE) c = ctx->move_cursor;
        else if (hover_handle != HANDLE_NONE) c = ctx->resize_cursor;

        GtkNative *native = gtk_widget_get_native(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl)));
        if (native) gdk_surface_set_cursor(gtk_native_get_surface(native), c);
    }
}

void editor_on_click_select(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    if (!app_state.is_editing) return; 

    transform_coords_from_window(&x, &y);
    PhysicsContext *ctx = &app_state.physics;
    Box *hit = NULL;

    for (GList *l = ctx->boxes; l != NULL; l = l->next) {
        Box *b = (Box *)l->data;
        if (x >= b->vis_x && x <= b->vis_x + b->vis_w && y >= b->vis_y && y <= b->vis_y + b->vis_h) { hit = b; break; }
    }
    ctx->selected_box = hit;
    if (hit) {
        gtk_widget_set_visible(app_state.show_drawer_btn, FALSE);
        gtk_revealer_set_reveal_child(GTK_REVEALER(app_state.drawer_revealer), TRUE);
        drawer_show_config(hit); 
    } else {
        drawer_show_library();
    }
    gtk_widget_queue_draw(ctx->overlay_draw);
}

void editor_on_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer user_data) {
    if (!app_state.is_editing) return; 
    transform_coords_from_window(&x, &y);
    PhysicsContext *ctx = &app_state.physics;
    Box *target = NULL;
    HandleType handle = HANDLE_NONE;

    for (GList *l = g_list_last(ctx->boxes); l != NULL; l = l->prev) {
        Box *b = (Box *)l->data;
        if ((handle = hit_test(b, x, y)) != HANDLE_NONE) { target = b; break; }
    }
    
    if (target) {
        ctx->is_dragging = TRUE;
        ctx->active_box = target;
        ctx->selected_box = target; 
        gtk_widget_set_visible(app_state.show_drawer_btn, FALSE);
        gtk_revealer_set_reveal_child(GTK_REVEALER(app_state.drawer_revealer), TRUE);
        drawer_show_config(target); 
        ctx->active_handle = handle;
        ctx->start_mouse_x = x; ctx->start_mouse_y = y;
        ctx->start_box_x = target->vis_x; ctx->start_box_y = target->vis_y;
        ctx->start_box_w = target->vis_w; ctx->start_box_h = target->vis_h;
        
        double l = target->vis_x; double t = target->vis_y;
        double r = l + target->vis_w; double b = t + target->vis_h;

        if (handle == HANDLE_MOVE) { 
            ctx->grab_off_x = l - x; ctx->grab_off_y = t - y; 
        } else {
            if (handle == HANDLE_TL) { ctx->grab_off_x = l - x; ctx->grab_off_y = t - y; }
            else if (handle == HANDLE_TR) { ctx->grab_off_x = r - x; ctx->grab_off_y = t - y; }
            else if (handle == HANDLE_BL) { ctx->grab_off_x = l - x; ctx->grab_off_y = b - y; }
            else if (handle == HANDLE_BR) { ctx->grab_off_x = r - x; ctx->grab_off_y = b - y; }
            ctx->anchor_col = (handle == HANDLE_TL || handle == HANDLE_BL) ? target->grid_x + target->grid_w : target->grid_x;
            ctx->anchor_row = (handle == HANDLE_TL || handle == HANDLE_TR) ? target->grid_y + target->grid_h : target->grid_y;
        }
        update_ui_visibility(); 
    }
    gtk_widget_queue_draw(ctx->overlay_draw);
}

void editor_on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data) {
    PhysicsContext *ctx = &app_state.physics;
    if (!ctx->is_dragging || !ctx->active_box) return;

    gint64 now = g_get_monotonic_time();
    if (now - last_drag_time < 16000) return; 
    last_drag_time = now;

    Box *b = ctx->active_box;
    int cell = ctx->cell_size;
    
    int win_w = gtk_widget_get_width(app_state.physics.overlay_draw);
    int win_h = gtk_widget_get_height(app_state.physics.overlay_draw);
    double local_curr_x, local_curr_y;
    gtk_gesture_drag_get_start_point(gesture, &local_curr_x, &local_curr_y);
    local_curr_x += offset_x; local_curr_y += offset_y;
    double root_x = (local_curr_x - win_w/2.0) * EDIT_SCALE + win_w/2.0;
    double root_y = (local_curr_y - win_h/2.0) * EDIT_SCALE + win_h/2.0;
    
    gboolean hovering_trash = (root_x > (win_w/2 - 150) && root_x < (win_w/2 + 150)) && (root_y > win_h - 120);
    if (hovering_trash != ctx->hover_trash) {
        ctx->hover_trash = hovering_trash;
        if (hovering_trash) gtk_widget_add_css_class(app_state.trash_box, "hover");
        else gtk_widget_remove_css_class(app_state.trash_box, "hover");
    }

    double mouse_x = ctx->start_mouse_x + offset_x;
    double mouse_y = ctx->start_mouse_y + offset_y;
    double new_x, new_y, new_w, new_h;
    
    if (ctx->active_handle == HANDLE_MOVE) {
        new_x = mouse_x + ctx->grab_off_x;
        new_y = mouse_y + ctx->grab_off_y;
        new_w = b->vis_w; new_h = b->vis_h;
    } else {
        double target_x = mouse_x + ctx->grab_off_x;
        double target_y = mouse_y + ctx->grab_off_y;
        double anchor_x = ctx->anchor_col * cell;
        double anchor_y = ctx->anchor_row * cell;
        
        if (ctx->active_handle == HANDLE_TL || ctx->active_handle == HANDLE_BL) {
             new_x = (target_x > anchor_x - cell) ? anchor_x - cell : target_x;
             new_w = anchor_x - new_x;
        } else {
             new_x = anchor_x;
             new_w = target_x - anchor_x;
             if (new_w < cell) new_w = cell;
        }
        
        if (ctx->active_handle == HANDLE_TL || ctx->active_handle == HANDLE_TR) {
            new_y = (target_y > anchor_y - cell) ? anchor_y - cell : target_y;
            new_h = anchor_y - new_y;
        } else {
            new_y = anchor_y;
            new_h = target_y - anchor_y;
            if (new_h < cell) new_h = cell;
        }
    }
    
    b->vis_x = new_x; b->vis_y = new_y;
    b->vis_w = new_w; b->vis_h = new_h;
    b->target_x = new_x; b->target_y = new_y;
    b->target_w = new_w; b->target_h = new_h;
    
    update_widget_geometry_safe(b);
    gtk_widget_queue_draw(ctx->overlay_draw);
}

void editor_on_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data) {
    PhysicsContext *ctx = &app_state.physics;
    if (ctx->is_dragging && ctx->active_box) {
        Box *b = ctx->active_box;
        int cell = ctx->cell_size;
        
        if (ctx->hover_trash) {
            gtk_fixed_remove(GTK_FIXED(ctx->fixed_container), b->widget);
            ctx->boxes = g_list_remove(ctx->boxes, b);
            g_free(b->type); g_free(b);
            ctx->active_box = NULL; ctx->selected_box = NULL;
            drawer_show_library();
            
            ctx->hover_trash = FALSE;
            gtk_widget_remove_css_class(app_state.trash_box, "hover");
            save_layout();
            ctx->is_dragging = FALSE;
            update_ui_visibility();
            gtk_widget_queue_draw(ctx->overlay_draw);
            return;
        }
        
        int sl = snap(b->vis_x); int st = snap(b->vis_y);
        int sr = snap(b->vis_x + b->vis_w); int sb = snap(b->vis_y + b->vis_h);
        if (sr <= sl) sr = sl + 1;
        if (sb <= st) sb = st + 1;
        int final_w = sr - sl; int final_h = sb - st;
        
        if (!check_collision(ctx, b, sl, st, final_w, final_h)) {
            b->grid_x = sl; b->grid_y = st; b->grid_w = final_w; b->grid_h = final_h;
        }
        
        b->nudge_x = 0; b->nudge_y = 0; 
        b->target_x = b->grid_x * cell; b->target_y = b->grid_y * cell;
        b->target_w = b->grid_w * cell; b->target_h = b->grid_h * cell;
        
        if (ctx->active_handle != HANDLE_MOVE) {
            b->vis_x = b->target_x; b->vis_y = b->target_y;
            b->vis_w = b->target_w; b->vis_h = b->target_h;
            gtk_fixed_remove(GTK_FIXED(ctx->fixed_container), b->widget);
            b->widget = create_widget_content(b->type, b->variant_index);
            gtk_fixed_put(GTK_FIXED(ctx->fixed_container), b->widget, 0, 0);
            apply_box_settings(b);
            if (ctx->selected_box == b) drawer_show_config(b);
            update_widget_geometry_safe(b);
        } else {
            start_animation();
        }
        
        save_layout();
        ctx->is_dragging = FALSE;
        update_ui_visibility();
        gtk_widget_queue_draw(ctx->overlay_draw);
    }
}

GdkDragAction editor_on_dnd_enter(GtkDropTarget *target, double x, double y, gpointer user_data) { return GDK_ACTION_COPY; }

void editor_on_dnd_leave(GtkDropTarget *target, gpointer user_data) { }

gboolean editor_on_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    if (!app_state.is_editing) return FALSE;
    
    PhysicsContext *ctx = &app_state.physics;
    transform_coords_from_window(&x, &y);
    int cell = ctx->cell_size;
    
    if (G_VALUE_HOLDS(value, G_TYPE_STRING)) {
        const char *payload = g_value_get_string(value);
        char **parts = g_strsplit(payload, ":", 2);
        if (!parts || !parts[0] || !parts[1]) { if(parts) g_strfreev(parts); return FALSE; }
        
        char *fam_id_str = g_strdup(parts[0]);
        int var_idx = atoi(parts[1]);
        g_strfreev(parts);

        const SceneFamily *fam = scene_registry_lookup_family(fam_id_str);
        if (!fam) { g_free(fam_id_str); return FALSE; }
        if (var_idx < 0 || var_idx >= fam->variant_count) { g_free(fam_id_str); return FALSE; }
        const SceneVariant *var = &fam->variants[var_idx];

        int cx = snap(x); int cy = snap(y);

        gboolean collision = FALSE;
        for (GList *l = ctx->boxes; l != NULL; l = l->next) {
            Box *other = (Box *)l->data;
            if (is_overlapping(cx, cy, var->default_w, var->default_h, other->grid_x, other->grid_y, other->grid_w, other->grid_h)) { 
                collision = TRUE; break; 
            }
        }
        if (collision) { g_free(fam_id_str); return FALSE; }

        Box *new_box = g_malloc0(sizeof(Box));
        new_box->id = ++ctx->next_id;
        new_box->type = fam_id_str; 
        new_box->variant_index = var_idx;
        new_box->grid_x = cx; new_box->grid_y = cy;
        new_box->grid_w = var->default_w; new_box->grid_h = var->default_h;
        new_box->min_w = 1; new_box->min_h = 1;
        
        new_box->vis_x = cx * cell; new_box->vis_y = cy * cell;
        new_box->vis_w = new_box->grid_w * cell; new_box->vis_h = new_box->grid_h * cell;
        new_box->target_x = new_box->vis_x; new_box->target_y = new_box->vis_y;
        new_box->target_w = new_box->vis_w; new_box->target_h = new_box->vis_h;
        new_box->is_24h = TRUE;
        
        new_box->widget = create_widget_content(fam_id_str, var_idx);
        gtk_fixed_put(GTK_FIXED(ctx->fixed_container), new_box->widget, new_box->vis_x, new_box->vis_y);
        
        update_widget_geometry_safe(new_box); 
        apply_box_settings(new_box); 
        ctx->boxes = g_list_append(ctx->boxes, new_box);
        
        ctx->active_box = new_box; ctx->selected_box = new_box;
        gtk_widget_set_visible(app_state.show_drawer_btn, FALSE);
        gtk_revealer_set_reveal_child(GTK_REVEALER(app_state.drawer_revealer), TRUE);
        drawer_show_config(new_box);
        
        start_animation();
        save_layout();
        return TRUE;
    }
    return FALSE;
}

void editor_draw_overlay(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    if (app_state.is_picking_wallpaper || !app_state.is_editing) return;
    
    PhysicsContext *ctx = &app_state.physics;
    
    if (width != last_view_w || height != last_view_h) {
        gboolean is_startup = (last_view_h == 0); 
        last_view_w = width; last_view_h = height;
        recalculate_targets(width, height);
        if (is_startup) {
            ctx->current_offset_x = ctx->target_offset_x;
            ctx->current_offset_y = ctx->target_offset_y;
            for (GList *l = ctx->boxes; l != NULL; l = l->next) {
                Box *b = (Box*)l->data;
                b->vis_x = b->target_x; b->vis_y = b->target_y; 
                b->vis_w = b->target_w; b->vis_h = b->target_h;
                update_widget_geometry_safe(b);
            }
        }
    }
    
    recalculate_targets(width, height);
    if (ctx->tick_id == 0) { 
        ctx->current_offset_x = ctx->target_offset_x; 
        ctx->current_offset_y = ctx->target_offset_y; 
    }
    
    int cell = ctx->cell_size;
    cairo_save(cr);
    cairo_translate(cr, ctx->current_offset_x, ctx->current_offset_y);
    int grid_w = (int)floor((double)width / cell) * cell;
    int grid_h = (int)floor((double)height / cell) * cell;
    
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
    cairo_set_line_width(cr, 1.0);
    for (int x = 0; x <= width; x += cell) { cairo_move_to(cr, x, 0); cairo_line_to(cr, x, grid_h); }
    for (int y = 0; y <= height; y += cell) { cairo_move_to(cr, 0, y); cairo_line_to(cr, grid_w, y); }
    cairo_stroke(cr);
    
    if (ctx->active_box) {
        Box *b = ctx->active_box;
        if (ctx->hover_trash) cairo_set_source_rgb(cr, 1.0, 0.2, 0.2); else cairo_set_source_rgb(cr, 0.2, 0.8, 1.0);
        cairo_set_line_width(cr, 2.0);
        cairo_rectangle(cr, b->vis_x, b->vis_y, b->vis_w, b->vis_h);
        cairo_stroke(cr);
        double hs = 14.0; 
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_rectangle(cr, b->vis_x - hs/2, b->vis_y - hs/2, hs, hs); 
        cairo_rectangle(cr, b->vis_x + b->vis_w - hs/2, b->vis_y - hs/2, hs, hs); 
        cairo_rectangle(cr, b->vis_x - hs/2, b->vis_y + b->vis_h - hs/2, hs, hs); 
        cairo_rectangle(cr, b->vis_x + b->vis_w - hs/2, b->vis_y + b->vis_h - hs/2, hs, hs); 
        cairo_fill(cr);
    } else if (ctx->selected_box) {
        Box *b = ctx->selected_box;
        cairo_set_source_rgb(cr, 0.2, 1.0, 0.6); 
        cairo_set_line_width(cr, 2.0);
        cairo_rectangle(cr, b->vis_x, b->vis_y, b->vis_w, b->vis_h);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}