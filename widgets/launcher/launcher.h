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

GtkWidget* create_launcher_widget();

// This is the NEW public declaration. Now other files can see this function.
AuroraResultObject* aurora_result_object_new(
    AuroraResultType type,
    const gchar *name,
    const gchar *description,
    const gchar *icon_name,
    gpointer data,
    GDestroyNotify data_free_func
);

#endif // LAUNCHER_H