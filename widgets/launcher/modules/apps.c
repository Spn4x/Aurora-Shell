// ===================================================================
//  Aurora Launcher - Apps Module (Corrected & Simplified)
// ===================================================================
// Fetches applications using the standard GIO/GAppInfo API, ensuring
// correct application discovery and process launching.

#include <gio/gio.h>
#include <string.h>
#include "apps.h"
#include "../launcher.h"

// A robust fuzzy search algorithm to match user input against app names.
static gboolean fuzzy_match(const gchar *str, const gchar *search) {
    if (!str || !search) return FALSE;

    g_autofree gchar *name_lower = g_ascii_strdown(str, -1);
    g_autofree gchar *search_lower = g_ascii_strdown(search, -1);

    // Create a version of the search string with spaces removed for better matching.
    char search_no_spaces[strlen(search_lower) + 1];
    int k = 0;
    for (int i = 0; search_lower[i] != '\0'; i++) {
        if (search_lower[i] != ' ') {
            search_no_spaces[k++] = search_lower[i];
        }
    }
    search_no_spaces[k] = '\0';

    // Perform the sequential character match.
    int search_idx = 0;
    int name_idx = 0;
    while (search_no_spaces[search_idx] != '\0' && name_lower[name_idx] != '\0') {
        if (search_no_spaces[search_idx] == name_lower[name_idx]) {
            search_idx++;
        }
        name_idx++;
    }

    return search_no_spaces[search_idx] == '\0';
}

// Public function to get application results based on a search term.
GList* get_app_results(const gchar *search_text) {
    GList *results = NULL;

    if (!search_text || *search_text == '\0') {
        return NULL;
    }

    // Use GIO to get a list of all applications known to the system.
    GList *all_apps = g_app_info_get_all();

    for (GList *l = all_apps; l != NULL; l = l->next) {
        GAppInfo *app_info = G_APP_INFO(l->data);

        // Skip applications that are marked as hidden (e.g., system services).
        if (!g_app_info_should_show(app_info)) {
            continue;
        }

        const gchar *app_name = g_app_info_get_name(app_info);
        if (fuzzy_match(app_name, search_text)) {
            // Get the GIcon and convert it to a string (either a name or a path).
            g_autofree gchar *icon_str = NULL;
            GIcon *icon = g_app_info_get_icon(app_info);
            if (icon) {
                icon_str = g_icon_to_string(icon);
            }

            // Create a result object. We pass the GAppInfo object itself as the data payload.
            // We must g_object_ref it so the result object owns it.
            results = g_list_prepend(results, aurora_result_object_new(
                AURORA_RESULT_APP,
                app_name,
                g_app_info_get_description(app_info),
                icon_str ? icon_str : "application-x-executable",
                g_object_ref(app_info), // Pass the GAppInfo object
                g_object_unref          // Provide its free function
            ));
        }
    }
    
    // We're done with the list provided by GIO. Free the list structure.
    // The GAppInfo objects inside are managed by GLib, we don't unref them here.
    g_list_free(all_apps);

    return g_list_reverse(results);
}