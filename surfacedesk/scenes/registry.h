// FILE: ./scenes/registry.h
#ifndef SCENE_REGISTRY_H
#define SCENE_REGISTRY_H

#include <gtk/gtk.h>

typedef GtkWidget* (*SceneFactoryFunc)(void);

typedef struct {
    const char *name;       
    int default_w;          
    int default_h;          
    SceneFactoryFunc factory;
} SceneVariant;

typedef struct {
    const char *id;         
    const char *name;       
    const char *icon;       
    int variant_count;
    const SceneVariant *variants;
} SceneFamily;

const SceneFamily* scene_registry_get_all(int *count);
const SceneFamily* scene_registry_lookup_family(const char *id);

// Factories
GtkWidget* scene_clock_simple(void);
GtkWidget* scene_clock_date(void);
GtkWidget* scene_clock_big(void);
GtkWidget* scene_clock_wide(void);
GtkWidget* scene_clock_left(void);
GtkWidget* scene_clock_pill(void);

GtkWidget* scene_sys_stacked(void);
GtkWidget* scene_sys_vertical(void);
GtkWidget* scene_sys_dashboard(void);
GtkWidget* scene_sys_text(void);

GtkWidget* scene_create_custom_label(void);

GtkWidget* scene_media_art(void);
GtkWidget* scene_media_controls(void);
GtkWidget* scene_media_full(void);
GtkWidget* scene_media_wide(void);

#endif