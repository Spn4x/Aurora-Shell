#ifndef MODULES_APPS_H
#define MODULES_APPS_H

#include <glib.h>
#include "../launcher.h" // Needed for the AppInfo type

// Public function to get app results
GList* get_app_results(const gchar *search_text);

// ========================================================================
// <<< THE FIX: Add a public getter function for the exec command.
// ========================================================================
const gchar* app_info_get_exec_cmd(AppInfo *app);

#endif // MODULES_APPS_H