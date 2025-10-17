#ifndef TOPBAR_H
#define TOPBAR_H

#include <gtk/gtk.h>

// =======================================================================
// THE FIX: The function now correctly accepts a const char*
GtkWidget* create_topbar_widget(const char *config_string);
// =======================================================================

#endif // TOPBAR_H