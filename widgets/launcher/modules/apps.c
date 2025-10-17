#include <gtk/gtk.h>
#include <string.h>
#include "apps.h"
#include "../launcher.h"

// --- AppInfo GObject definition (This is stable and correct) ---
#define APP_TYPE_INFO (app_info_get_type())
struct _AppInfo { GObject parent_instance; gchar *name; gchar *description; gchar *icon_name; gchar *exec_cmd; };
G_DEFINE_TYPE(AppInfo, app_info, G_TYPE_OBJECT)
static void app_info_finalize(GObject *object) { AppInfo *self = APP_INFO(object); g_free(self->name); g_free(self->description); g_free(self->icon_name); g_free(self->exec_cmd); G_OBJECT_CLASS(app_info_parent_class)->finalize(object); }
static void app_info_init(AppInfo *self) { (void)self; }
static void app_info_class_init(AppInfoClass *klass) { GObjectClass *object_class = G_OBJECT_CLASS(klass); object_class->finalize = app_info_finalize; }
static AppInfo* app_info_new(const gchar *name, const gchar *description, const gchar *icon, const gchar *exec) {
    AppInfo *self = g_object_new(APP_TYPE_INFO, NULL);
    self->name = g_strdup(name);
    self->description = g_strdup(description ? description : "");
    self->icon_name = g_strdup(icon ? icon : "application-x-executable");
    self->exec_cmd = g_strdup(exec);
    return self;
}

// --- App Discovery (This is stable and correct) ---
static GList *all_apps = NULL;
static void load_all_desktop_files() {
    if (all_apps) return;
    GList *search_dirs = NULL;
    const gchar * const * system_dirs = g_get_system_data_dirs();
    for (int i = 0; system_dirs[i] != NULL; i++) { search_dirs = g_list_prepend(search_dirs, g_build_filename(system_dirs[i], "applications", NULL)); }
    gchar *user_dir = g_build_filename(g_get_home_dir(), ".local", "share", "applications", NULL);
    search_dirs = g_list_prepend(search_dirs, user_dir);
    for (GList *l = search_dirs; l != NULL; l = l->next) {
        gchar *app_dir = (gchar *)l->data;
        g_autoptr(GDir) dir = g_dir_open(app_dir, 0, NULL);
        if (!dir) continue;
        const gchar *filename;
        while ((filename = g_dir_read_name(dir))) {
            if (g_str_has_suffix(filename, ".desktop")) {
                g_autofree gchar *path = g_build_filename(app_dir, filename, NULL);
                g_autoptr(GKeyFile) key_file = g_key_file_new();
                if (g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, NULL)) {
                    if (g_key_file_get_boolean(key_file, "Desktop Entry", "NoDisplay", NULL)) continue;
                    g_autofree gchar *name = g_key_file_get_string(key_file, "Desktop Entry", "Name", NULL);
                    g_autofree gchar *exec = g_key_file_get_string(key_file, "Desktop Entry", "Exec", NULL);
                    g_autofree gchar *icon = g_key_file_get_string(key_file, "Desktop Entry", "Icon", NULL);
                    g_autofree gchar *description = g_key_file_get_string(key_file, "Desktop Entry", "Comment", NULL);
                    if (name && exec) {
                        char *percent_pos = strchr(exec, '%');
                        if (percent_pos) *percent_pos = '\0';
                        g_strstrip(exec);
                        all_apps = g_list_prepend(all_apps, app_info_new(name, description, icon, exec));
                    }
                }
            }
        }
    }
    g_list_free_full(search_dirs, g_free);
}


// ========================================================================
// <<< THE FINAL, DEFINITIVE, AND CORRECT FUZZY SEARCH
// ========================================================================
static gboolean fuzzy_match(const gchar *str, const gchar *search) {
    if (!str || !search) return FALSE;

    g_autofree gchar *name_lower = g_ascii_strdown(str, -1);
    g_autofree gchar *search_lower = g_ascii_strdown(search, -1);

    // --- THE CORRECT WAY TO REMOVE SPACES ---
    // We create a new character array on the stack to hold the result.
    char search_no_spaces[strlen(search_lower) + 1];
    int k = 0; // This will be our index for the new string.
    for (int i = 0; search_lower[i] != '\0'; i++) {
        // Only copy the character if it is NOT a space.
        if (search_lower[i] != ' ') {
            search_no_spaces[k++] = search_lower[i];
        }
    }
    search_no_spaces[k] = '\0'; // Null-terminate the new string.
    // --- END OF FIX ---

    // Now, the rest of the classic fuzzy find algorithm will work correctly.
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


// --- Public Module Function (Simplified to just use fuzzy_match) ---
GList* get_app_results(const gchar *search_text) {
    load_all_desktop_files();
    GList *results = NULL;

    if (!search_text || *search_text == '\0') {
        return NULL;
    }

    for (GList *l = all_apps; l != NULL; l = l->next) {
        AppInfo *app = APP_INFO(l->data);
        if (fuzzy_match(app->name, search_text)) {
            results = g_list_prepend(results, aurora_result_object_new(
                AURORA_RESULT_APP, app->name, app->description,
                app->icon_name, g_object_ref(app), g_object_unref
            ));
        }
    }

    // A simple reverse is good enough for now. Better scoring can be a future feature.
    return g_list_reverse(results);
}

// The getter function that the linker needed
const gchar* app_info_get_exec_cmd(AppInfo *app) {
    g_return_val_if_fail(APP_IS_INFO(app), NULL);
    return app->exec_cmd;
}