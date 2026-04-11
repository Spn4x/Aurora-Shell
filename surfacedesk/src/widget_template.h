// FILE: ./src/widget_template.h
#ifndef WIDGET_TEMPLATE_H
#define WIDGET_TEMPLATE_H

#include <gtk/gtk.h>
#include "globals.h"

// Updated signature: Type (Family) + Variant Index
GtkWidget* create_widget_content(const char* type, int variant_index);

void update_widget_geometry(Box *b, GtkWidget *fixed);

#endif