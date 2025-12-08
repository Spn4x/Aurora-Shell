#ifndef POPOVER_ANIM_H
#define POPOVER_ANIM_H

#include <gtk/gtk.h>

// State struct for the animation
typedef struct {
    GList *revealers;
    GList *current_item;
    guint animation_id;
    GtkWidget *source_button;
} PopoverAnimState;

// Attaches the cascading animation gimmick to any GtkPopover
void attach_popover_animation(GtkPopover *popover, GtkWidget *source_button);

// Call this when you dynamically repopulate the popover's children
void reset_popover_animation(GtkPopover *popover);

// THE FIX: Declare the cleanup function so other files can see it.
void free_popover_anim_state(gpointer data);

#endif // POPOVER_ANIM_H