// widgets/launcher/modules/apps.c
#include <gio/gio.h>
#include <string.h>
#include "apps.h"
#include "../launcher.h"

// --- CACHE STATIC VARIABLES ---
static GList *cached_app_list = NULL;
static gboolean cache_initialized = FALSE;

// Helper: Initialize the cache once
static void ensure_app_cache(void) {
    if (cache_initialized) return;

    GList *all_apps = g_app_info_get_all();
    for (GList *l = all_apps; l != NULL; l = l->next) {
        GAppInfo *app_info = G_APP_INFO(l->data);
        
        // Filter out hidden apps immediately during cache build
        if (g_app_info_should_show(app_info)) {
            // We keep a reference to the AppInfo in our cache
            cached_app_list = g_list_prepend(cached_app_list, g_object_ref(app_info));
        }
    }
    
    g_list_free_full(all_apps, g_object_unref); // Free the original list container
    cached_app_list = g_list_reverse(cached_app_list);
    cache_initialized = TRUE;
}

// Keep your fuzzy_match function, it is fine
static gboolean fuzzy_match(const gchar *str, const gchar *search) {
    if (!str || !search) return FALSE;
    // ... (Your existing implementation is fine for in-memory comparisons) ...
    // COPY PASTE YOUR EXISTING fuzzy_match HERE
    g_autofree gchar *name_lower = g_ascii_strdown(str, -1);
    g_autofree gchar *search_lower = g_ascii_strdown(search, -1);

    char search_no_spaces[strlen(search_lower) + 1];
    int k = 0;
    for (int i = 0; search_lower[i] != '\0'; i++) {
        if (search_lower[i] != ' ') search_no_spaces[k++] = search_lower[i];
    }
    search_no_spaces[k] = '\0';

    int search_idx = 0;
    int name_idx = 0;
    while (search_no_spaces[search_idx] != '\0' && name_lower[name_idx] != '\0') {
        if (search_no_spaces[search_idx] == name_lower[name_idx]) search_idx++;
        name_idx++;
    }
    return search_no_spaces[search_idx] == '\0';
}

GList* get_app_results(const gchar *search_text) {
    // 1. Ensure we have the list in RAM
    ensure_app_cache();

    GList *results = NULL;
    if (!search_text || *search_text == '\0') {
        // Optional: Return top 5 apps if empty, or NULL
        return NULL;
    }

    g_autofree gchar *search_lower = g_ascii_strdown(search_text, -1);

    // 2. Iterate over the IN-MEMORY cache (super fast)
    for (GList *l = cached_app_list; l != NULL; l = l->next) {
        GAppInfo *app_info = G_APP_INFO(l->data);
        const gchar *app_name = g_app_info_get_name(app_info);
        
        // Fast pre-check: if search is longer than name, skip
        if (strlen(search_text) > strlen(app_name)) continue;

        g_autofree gchar *app_name_lower = g_ascii_strdown(app_name, -1);
        
        gint score = 0;

        if (g_strcmp0(app_name_lower, search_lower) == 0) {
            score = 100;
        } else if (g_str_has_prefix(app_name_lower, search_lower)) {
            score = 80;
        } else if (strstr(app_name_lower, search_lower)) {
            score = 70; // Substring match is better than fuzzy
        } else if (fuzzy_match(app_name, search_text)) {
            score = 60;
        }
        
        if (score > 0) {
            g_autofree gchar *icon_str = NULL;
            GIcon *icon = g_app_info_get_icon(app_info);
            if (icon) icon_str = g_icon_to_string(icon);

            results = g_list_prepend(results, aurora_result_object_new(
                AURORA_RESULT_APP,
                app_name,
                g_app_info_get_description(app_info),
                icon_str ? icon_str : "application-x-executable",
                g_object_ref(app_info),
                g_object_unref,
                score 
            ));
        }
    }
    
    return results;
}