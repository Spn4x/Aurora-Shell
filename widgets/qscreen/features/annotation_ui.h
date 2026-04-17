#ifndef QSCREEN_ANNOTATION_UI_H
#define QSCREEN_ANNOTATION_UI_H

#include "ui_state.h"

GtkWidget* create_annotation_toolbar(UIState *state);

void enter_annotation_phase(UIState *state);
void cancel_annotation_phase(UIState *state);
void finalize_text_entry(UIState *state);
void annotation_ui_confirm(UIState *state);

void annotation_ui_drag_begin(UIState *state, double scaled_x, double scaled_y, double raw_x, double raw_y);
void annotation_ui_drag_update(UIState *state, double scaled_x, double scaled_y);

// Returns TRUE if the keypress was handled by the annotation UI
gboolean annotation_ui_handle_key(UIState *state, guint keyval, GdkModifierType mod_state);

#endif // QSCREEN_ANNOTATION_UI_H