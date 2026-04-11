// FILE: ./src/wallpaper_chooser.h
#ifndef WALLPAPER_CHOOSER_H
#define WALLPAPER_CHOOSER_H

#include <gtk/gtk.h>

// Creates the bottom horizontal shelf widget
GtkWidget *create_wallpaper_shelf(void);

// Focuses the first item (called when opening the chooser)
void wallpaper_chooser_grab_focus(void);

// Reverts the visual wallpaper to the original state (undo preview)
void wallpaper_chooser_cancel(void);

#endif