#ifndef EDITOR_H
#define EDITOR_H

#include <gtk/gtk.h>
#include "globals.h"

// --- Constants ---
#define EDIT_SCALE 0.85
#define ANIM_SPEED 0.12 
#define SNAP_EPSILON 0.5

// --- Physics & Animation ---
void start_animation(void);
void recalculate_targets(int w, int h);
void update_widget_geometry_safe(Box *b);

// --- Manual Layout Control ---
// NEW: Callable by main to force widgets to jump to correct positions
void editor_force_snap(void); 

// --- Event Handlers ---
void editor_on_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer user_data);
void editor_on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data);
void editor_on_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data);

void editor_on_click_select(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
void editor_on_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer user_data);

// --- Drag and Drop ---
GdkDragAction editor_on_dnd_enter(GtkDropTarget *target, double x, double y, gpointer user_data);
void editor_on_dnd_leave(GtkDropTarget *target, gpointer user_data);
gboolean editor_on_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data);

// --- Drawing ---
void editor_draw_overlay(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);

// --- Utils ---
void transform_coords_from_window(double *x, double *y);

// --- Callbacks ---
void update_ui_visibility(void);

#endif