#ifndef DRAWER_H
#define DRAWER_H

#include "globals.h"
#include <gtk/gtk.h>

GtkWidget *create_drawer(void);
void drawer_show_library(void);
void drawer_show_config(Box *b);
void apply_box_settings(Box *b); // NEW

#endif