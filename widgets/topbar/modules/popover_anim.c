#include "popover_anim.h"

// --- Helper Functions ---

 void free_popover_anim_state(gpointer data) {
    PopoverAnimState *state = data;
    if (!state) return;
    if (state->animation_id > 0) {
        g_source_remove(state->animation_id);
        state->animation_id = 0;
    }
    g_list_free(state->revealers);
    g_free(state);
}

static gboolean popover_cascade_tick(gpointer user_data) {
    PopoverAnimState *state = user_data;
    if (!state || !state->current_item) {
        if (state) state->animation_id = 0;
        return G_SOURCE_REMOVE;
    }

    gtk_revealer_set_reveal_child(GTK_REVEALER(state->current_item->data), TRUE);
    state->current_item = state->current_item->next;
    
    if (state->current_item == NULL) {
        state->animation_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

// --- Signal Handlers (The Fix is in 'on_popover_unmap') ---

static void on_popover_map(GtkWidget *widget, gpointer user_data) {
    (void)user_data;
    PopoverAnimState *state = g_object_get_data(G_OBJECT(widget), "anim-state");
    if (!state) return;

    if (state->source_button) {
        gtk_widget_add_css_class(state->source_button, "popover-open");
    }

    // This part is fine, it just ensures all items start hidden before the cascade.
    for (GList *l = state->revealers; l != NULL; l = l->next) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(l->data), FALSE);
    }
    
    // Start the animation timer
    if (state->animation_id > 0) g_source_remove(state->animation_id);
    state->current_item = state->revealers;
    state->animation_id = g_timeout_add(40, popover_cascade_tick, state); 
}

static void on_popover_unmap(GtkWidget *widget, gpointer user_data) {
    (void)user_data;
    PopoverAnimState *state = g_object_get_data(G_OBJECT(widget), "anim-state");
    if (!state) return;

    // Stop any running animation.
    if (state->animation_id > 0) {
        g_source_remove(state->animation_id);
        state->animation_id = 0;
    }

    // Reset button style.
    if (state->source_button) {
        gtk_widget_remove_css_class(state->source_button, "popover-open");
    }

    // --- THE FIX ---
    // Forcefully hide all items immediately when the popover closes.
    // This resets them for the next time `on_popover_map` is called.
    for (GList *l = state->revealers; l != NULL; l = l->next) {
        GtkRevealer *revealer = GTK_REVEALER(l->data);
        if (GTK_IS_REVEALER(revealer)) {
            // We set the duration to 0 to make the reset instant and invisible to the user.
            gtk_revealer_set_transition_duration(revealer, 0);
            gtk_revealer_set_reveal_child(revealer, FALSE);
            // Restore the duration for the next animation.
            gtk_revealer_set_transition_duration(revealer, 250);
        }
    }
}

// --- Public API Functions (Unchanged) ---

void attach_popover_animation(GtkPopover *popover, GtkWidget *source_button) {
    if (g_object_get_data(G_OBJECT(popover), "anim-state")) return;
    
    PopoverAnimState *state = g_new0(PopoverAnimState, 1);
    state->source_button = source_button;
    g_object_set_data_full(G_OBJECT(popover), "anim-state", state, free_popover_anim_state);
    
    g_signal_connect(popover, "map", G_CALLBACK(on_popover_map), NULL);
    g_signal_connect(popover, "unmap", G_CALLBACK(on_popover_unmap), NULL);
}

void reset_popover_animation(GtkPopover *popover) {
    PopoverAnimState *state = g_object_get_data(G_OBJECT(popover), "anim-state");
    if (!state) return;

    g_list_free(state->revealers);
    state->revealers = NULL;

    GtkWidget *list_box = gtk_popover_get_child(popover);
    if (!list_box) return;

    for (GtkWidget *child = gtk_widget_get_first_child(list_box); child != NULL; child = gtk_widget_get_next_sibling(child)) {
        if (GTK_IS_REVEALER(child)) {
            state->revealers = g_list_append(state->revealers, child);
        }
    }
}