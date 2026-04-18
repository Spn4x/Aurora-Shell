#include "island_widget.h"

struct _IslandWidget {
    GtkBox parent_instance;
    gboolean is_expanded;

    GtkWidget *content_stack;
    GtkWidget *pill_stack;
    GtkWidget *expanded_stack;
    GtkWidget *current_pill_child;
    GtkWidget *current_expanded_child;
};

G_DEFINE_TYPE(IslandWidget, island_widget, GTK_TYPE_BOX)

static gboolean remove_widget_from_stack(gpointer user_data) {
    GtkWidget *widget_to_remove = GTK_WIDGET(user_data);
    if (widget_to_remove && gtk_widget_get_parent(widget_to_remove)) {
        gtk_stack_remove(GTK_STACK(gtk_widget_get_parent(widget_to_remove)), widget_to_remove);
    }
    return G_SOURCE_REMOVE;
}

// THE FIX: Explicitly hide the expanded stack entirely to force GTK
// to drop the layout request and shrink the Wayland surface.
static gboolean hide_expanded_stack_cb(gpointer user_data) {
    IslandWidget *self = ISLAND_WIDGET(user_data);
    
    // Only proceed if the user hasn't clicked expand again during the 450ms animation
    if (!self->is_expanded) {
        gtk_widget_set_visible(self->expanded_stack, FALSE);
        
        if (self->current_expanded_child) {
            gtk_stack_remove(GTK_STACK(self->expanded_stack), self->current_expanded_child);
            self->current_expanded_child = NULL;
        }
    }
    return G_SOURCE_REMOVE;
}

void island_widget_set_expanded(IslandWidget *self, gboolean expanded) {
    g_return_if_fail(ISLAND_IS_WIDGET(self));
    if (self->is_expanded == expanded) return;
    self->is_expanded = expanded;

    if (self->is_expanded) {
        // Unhide the stack before transitioning so it can animate smoothly
        gtk_widget_set_visible(self->expanded_stack, TRUE);
        gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "expanded");
        gtk_widget_add_css_class(GTK_WIDGET(self), "expanded");
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "pill");
        gtk_widget_remove_css_class(GTK_WIDGET(self), "expanded");

        // Wait for the crossfade (400ms) to finish, then obliterate the expanded state
        g_timeout_add(450, hide_expanded_stack_cb, self);
    }
}

void island_widget_transition_to_pill_child(IslandWidget *self, GtkWidget *child) {
    g_return_if_fail(ISLAND_IS_WIDGET(self));
    GtkWidget *old_child = self->current_pill_child;
    GtkWidget *center_box = gtk_center_box_new();

    gtk_widget_set_hexpand(center_box, TRUE);
    gtk_center_box_set_center_widget(GTK_CENTER_BOX(center_box), child);

    gtk_stack_add_child(GTK_STACK(self->pill_stack), center_box);
    gtk_stack_set_visible_child(GTK_STACK(self->pill_stack), center_box);
    self->current_pill_child = center_box;

    if (old_child && old_child != center_box) {
        g_timeout_add(450, remove_widget_from_stack, old_child);
    }
}

void island_widget_transition_to_expanded_child(IslandWidget *self, GtkWidget *child) {
    g_return_if_fail(ISLAND_IS_WIDGET(self));
    GtkWidget *old_child = self->current_expanded_child;
    
    if (child) {
        gtk_stack_add_child(GTK_STACK(self->expanded_stack), child);
        gtk_stack_set_visible_child(GTK_STACK(self->expanded_stack), child);
        self->current_expanded_child = child;
    }

    if (old_child && old_child != child) {
        g_timeout_add(450, remove_widget_from_stack, old_child);
    }
}

GtkWidget* island_widget_new(void) {
    return g_object_new(ISLAND_WIDGET_TYPE, NULL);
}

static void island_widget_init(IslandWidget *self) {
    gtk_widget_add_css_class(GTK_WIDGET(self), "island-box");
    gtk_widget_set_halign(GTK_WIDGET(self), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(self), GTK_ALIGN_START);

    // Provide padding around the island physically without a wrapper
    gtk_widget_set_margin_top(GTK_WIDGET(self), 10);
    gtk_widget_set_margin_bottom(GTK_WIDGET(self), 10);
    gtk_widget_set_margin_start(GTK_WIDGET(self), 10);
    gtk_widget_set_margin_end(GTK_WIDGET(self), 10);

    self->content_stack = gtk_stack_new();
    gtk_stack_set_vhomogeneous(GTK_STACK(self->content_stack), FALSE);
    gtk_stack_set_hhomogeneous(GTK_STACK(self->content_stack), FALSE);
    gtk_stack_set_transition_type(GTK_STACK(self->content_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(self->content_stack), 400);
    gtk_stack_set_interpolate_size(GTK_STACK(self->content_stack), TRUE);
    gtk_box_append(GTK_BOX(self), self->content_stack);

    self->pill_stack = gtk_stack_new();
    gtk_stack_set_vhomogeneous(GTK_STACK(self->pill_stack), FALSE);
    gtk_stack_set_hhomogeneous(GTK_STACK(self->pill_stack), FALSE);
    gtk_stack_set_transition_type(GTK_STACK(self->pill_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_UP_DOWN);
    gtk_stack_set_transition_duration(GTK_STACK(self->pill_stack), 400);
    gtk_stack_set_interpolate_size(GTK_STACK(self->pill_stack), TRUE);
    gtk_stack_add_named(GTK_STACK(self->content_stack), self->pill_stack, "pill");

    self->expanded_stack = gtk_stack_new();
    gtk_stack_set_vhomogeneous(GTK_STACK(self->expanded_stack), FALSE);
    gtk_stack_set_hhomogeneous(GTK_STACK(self->expanded_stack), FALSE);
    gtk_stack_set_transition_type(GTK_STACK(self->expanded_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_UP_DOWN);
    gtk_stack_set_transition_duration(GTK_STACK(self->expanded_stack), 400);
    gtk_stack_set_interpolate_size(GTK_STACK(self->expanded_stack), TRUE);
    gtk_stack_add_named(GTK_STACK(self->content_stack), self->expanded_stack, "expanded");

    // Hide it immediately at startup to prevent ghost sizing
    gtk_widget_set_visible(self->expanded_stack, FALSE);

    GtkWidget *center_box = gtk_center_box_new();
    gtk_widget_set_hexpand(center_box, TRUE);
    gtk_center_box_set_center_widget(GTK_CENTER_BOX(center_box), gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
    gtk_stack_add_child(GTK_STACK(self->pill_stack), center_box);
    self->current_pill_child = center_box;
}

static void island_widget_class_init(IslandWidgetClass *klass G_GNUC_UNUSED) {}

GtkWidget* island_widget_get_pill_stack(IslandWidget *self) {
    g_return_val_if_fail(ISLAND_IS_WIDGET(self), NULL);
    return self->pill_stack;
}