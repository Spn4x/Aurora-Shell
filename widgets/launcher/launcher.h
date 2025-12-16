#ifndef LAUNCHER_H
#define LAUNCHER_H

#include <gtk/gtk.h>

// The public entry point for the orchestrator
G_MODULE_EXPORT GtkWidget* create_widget(const char *config_string);

#endif // LAUNCHER_H