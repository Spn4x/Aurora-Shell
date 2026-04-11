// FILE: ./scenes/registry.c
#include "registry.h"
#include <stddef.h>

static const SceneVariant clock_variants[] = {
    { "Simple Digital", 2, 1, scene_clock_simple },
    { "Date & Time",    3, 2, scene_clock_date },
    { "Large Display",  4, 2, scene_clock_big },
    { "Wide Side",      4, 1, scene_clock_wide },
    { "Left Modern",    3, 2, scene_clock_left },
    { "Pill Badge",     3, 2, scene_clock_pill }
};

static const SceneVariant system_variants[] = {
    { "Stacked Bars",   3, 2, scene_sys_stacked },
    { "Vertical Tower", 2, 3, scene_sys_vertical },
    { "Dashboard",      4, 2, scene_sys_dashboard },
    { "Minimal Text",   2, 1, scene_sys_text }
};

static const SceneVariant text_variants[] = {
    { "Custom Text",    3, 1, scene_create_custom_label }
};

static const SceneVariant media_variants[] = {
    { "Now Playing",    2, 2, scene_media_art },
    { "Control Pill",   3, 1, scene_media_controls },
    { "Full Player",    3, 4, scene_media_full },
    { "Wide Bar",       4, 1, scene_media_wide }
};

static const SceneFamily families[] = {
    { "Clock",  "Clock",          "preferences-system-time-symbolic",      6, clock_variants },
    { "System", "System Monitor", "utilities-system-monitor-symbolic",     4, system_variants },
    { "Label",  "Text Label",     "insert-text-symbolic",                  1, text_variants }, 
    { "Media",  "Media Player",   "multimedia-player-symbolic",            4, media_variants }
};

const SceneFamily* scene_registry_get_all(int *count) {
    *count = sizeof(families) / sizeof(families[0]);
    return families;
}

const SceneFamily* scene_registry_lookup_family(const char *id) {
    for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); i++) {
        if (g_strcmp0(families[i].id, id) == 0) return &families[i];
    }
    return NULL;
}