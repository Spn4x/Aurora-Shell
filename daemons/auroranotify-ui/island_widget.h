#ifndef __ISLAND_WIDGET_H__
#define __ISLAND_WIDGET_H__

#include <gtk/gtk.h>

#define ISLAND_WIDGET_TYPE (island_widget_get_type())
G_DECLARE_FINAL_TYPE(IslandWidget, island_widget, ISLAND, WIDGET, GtkBox)

GtkWidget* island_widget_new (void);
void island_widget_set_expanded (IslandWidget *self, gboolean expanded);

void island_widget_transition_to_pill_child (IslandWidget *self, GtkWidget *child);
void island_widget_transition_to_expanded_child (IslandWidget *self, GtkWidget *child);

#endif // __ISLAND_WIDGET_H__