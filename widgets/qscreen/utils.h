#ifndef QSCREEN_UTILS_H
#define QSCREEN_UTILS_H

#include "qscreen.h"

gboolean check_dependencies(void);
void run_command_with_stdin_sync(const gchar *command, const gchar *input);

void process_final_screenshot(const char *source_path, GdkRectangle *geometry, gboolean save_to_disk, QScreenState *state);
void process_fullscreen_screenshot(QScreenState *state);
GList* get_hyprland_windows_geometry(QScreenState *state);
void capture_fullscreen_for_overlay(GChildWatchFunc on_captured, gpointer user_data);

#endif // QSCREEN_UTILS_H