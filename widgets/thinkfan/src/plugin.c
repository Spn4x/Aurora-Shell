#include <gtk/gtk.h>
#include <adwaita.h>
#include <gmodule.h>
#include "window.h"
#include "backend.h"

// Define the exported entry point expected by aurora-shell
G_MODULE_EXPORT GtkWidget* create_widget(const char *config_string) {
    (void)config_string; // Not using JSON config inside the widget yet
    
    // Ensure libadwaita is initialized (needed for AdwViewStack)
    adw_init();

    // Initialize the sensor backend
    backend_init();

    // Create and return the box content
    return thinkfan_widget_new();
}