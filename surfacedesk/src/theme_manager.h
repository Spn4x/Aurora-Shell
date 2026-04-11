// FILE: ./src/theme_manager.h
#ifndef THEME_MANAGER_H
#define THEME_MANAGER_H

#include <gtk/gtk.h>

typedef void (*ThemeCompleteCallback)(void);

// Loads the existing generated CSS on startup
void theme_manager_init(void);

// Generates new colors and configs in background
void theme_manager_apply(const char *wallpaper_path, ThemeCompleteCallback callback);

#endif