#ifndef QSCREEN_ANNOTATION_UI_H
#define QSCREEN_ANNOTATION_UI_H

#include "ui_state.h"

GtkWidget* create_annotation_toolbar_top(UIState *state);
GtkWidget* create_annotation_toolbar_bottom(UIState *state);

void enter_annotation_phase(UIState *state);
void cancel_annotation_phase(UIState *state);
void finalize_text_entry(UIState *state);
void annotation_ui_confirm(UIState *state);

void annotation_ui_drag_begin(UIState *state, double scaled_x, double scaled_y, double raw_x, double raw_y);
void annotation_ui_drag_update(UIState *state, double scaled_x, double scaled_y);

// NEW: Double click handler
void annotation_ui_double_click(UIState *state, double raw_x, double raw_y);

gboolean annotation_ui_handle_key(UIState *state, guint keyval, GdkModifierType mod_state);

#endif // QSCREEN_ANNOTATION_UI_H