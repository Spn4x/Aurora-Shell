#ifndef QSCREEN_H
#define QSCREEN_H

#include <gtk/gtk.h>
#include <gdk/gdk.h>

// Enum for modes passed via config.json
typedef enum {
    QSCREEN_MODE_INTERACTIVE,
    QSCREEN_MODE_REGION,
    QSCREEN_MODE_WINDOW,
    QSCREEN_MODE_FULLSCREEN,
    QSCREEN_MODE_TEXT
} QScreenMode;

// Struct to hold parsed config values
typedef struct {
    // This GtkApplication pointer is no longer needed in the plugin model,
    // but we keep the struct for consistency with utils.c
    GtkApplication *app;
    gchar *temp_path_for_cleanup; // Path for the main background screenshot
    QScreenMode initial_mode;
    gboolean save_on_launch;
} QScreenState;

// This is the one and only public function for our plugin,
// called by the aurora-shell orchestrator.
GtkWidget* create_widget(const char *config_string);

#endif // QSCREEN_H