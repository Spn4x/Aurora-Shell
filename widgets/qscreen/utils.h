#ifndef QSCREEN_UTILS_H
#define QSCREEN_UTILS_H

#include "qscreen.h"

// Struct to hold Niri specific window data
typedef struct {
    GdkRectangle geometry; // We only use width and height now
    gchar *title;
    gchar *app_id;
    guint64 id;
    gboolean is_visible;
} NiriWindowInfo;

void niri_window_info_free(gpointer data);

gchar* run_niri_ipc(const char *cmd);
GList* get_niri_windows_info(QScreenState *state);

gboolean check_dependencies(void);
void run_command_with_stdin_sync(const gchar *command, const gchar *input);

void process_final_screenshot(const char *source_path, GdkRectangle *geometry, gboolean save_to_disk, QScreenState *state);
void process_fullscreen_screenshot(QScreenState *state);
void capture_fullscreen_for_overlay(GChildWatchFunc on_captured, gpointer user_data);
void process_precomposited_screenshot(const char *source_path, gboolean save_to_disk, QScreenState *state);

#endif // QSCREEN_UTILS_H