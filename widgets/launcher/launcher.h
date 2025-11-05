#ifndef LAUNCHER_H
#define LAUNCHER_H

#include <gtk/gtk.h>

// Forward declarations
G_DECLARE_FINAL_TYPE(AppInfo, app_info, APP, INFO, GObject)
G_DECLARE_FINAL_TYPE(AuroraResultObject, aurora_result_object, AURORA, RESULT_OBJECT, GObject)

typedef enum {
    AURORA_RESULT_APP,
    AURORA_RESULT_CALCULATOR,
    AURORA_RESULT_COMMAND,
    AURORA_RESULT_SYSTEM_ACTION,
    AURORA_RESULT_FILE
} AuroraResultType;

// --- PUBLIC FUNCTION DECLARATIONS ---

// The function in your plugin's main .c file that creates the widget
GtkWidget* create_widget(const char *config_string);

// The constructor for the result objects, used by your modules (apps.c, etc.)
AuroraResultObject* aurora_result_object_new(
    AuroraResultType type,
    const gchar *name,
    const gchar *description,
    const gchar *icon_name,
    gpointer data,
    GDestroyNotify data_free_func,
    gint score // <<< FIX IS HERE: Add the new score parameter
);

#endif // LAUNCHER_H