// FILE: ./src/theme_manager.c
#include "theme_manager.h"
#include <glib.h>
#include <glib/gstdio.h>

// Declare the Rust function
extern gboolean rust_generate_theme(const char *path);

// --- CSS Loading (Stays in C as it uses GTK API) ---

static void load_local_css_internal(void) {
    const char *home = g_get_home_dir();
    g_autofree char *local_css = g_build_filename(home, ".config", "aurora-shell", "aurora-colors.css", NULL);
    
    if (g_file_test(local_css, G_FILE_TEST_IS_REGULAR)) {
        GtkCssProvider *p = gtk_css_provider_new();
        gtk_css_provider_load_from_path(p, local_css);
        
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), 
            GTK_STYLE_PROVIDER(p), 
            GTK_STYLE_PROVIDER_PRIORITY_USER
        );
        g_object_unref(p);
    }
}

// --- Background Task ---

static void run_theme_task(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    char *wallpaper_path = (char *)task_data;
    
    // Call into Rust
    // The Rust side handles Wallust, parsing, math, and file writing synchronously.
    gboolean success = rust_generate_theme(wallpaper_path);

    g_task_return_boolean(task, success);
}

static void on_task_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
    g_task_propagate_boolean(G_TASK(res), NULL);
    
    // Reload CSS in the running GTK app
    load_local_css_internal();
    
    ThemeCompleteCallback cb = (ThemeCompleteCallback)user_data;
    if (cb) cb();
}

void theme_manager_init(void) {
    load_local_css_internal();
}

void theme_manager_apply(const char *wallpaper_path, ThemeCompleteCallback callback) {
    if (!wallpaper_path) {
        if (callback) callback();
        return;
    }
    
    GTask *task = g_task_new(NULL, NULL, on_task_complete, (gpointer)callback);
    g_task_set_task_data(task, g_strdup(wallpaper_path), g_free);
    g_task_run_in_thread(task, run_theme_task);
    g_object_unref(task);
}