// FILE: ./src/storage.c
#include <gtk/gtk.h>
#include <glib.h>
#include <sys/stat.h>
#include "globals.h"
#include "widget_template.h"
#include "drawer.h" 
#include "../scenes/registry.h"

#define CONF_DIR "surfacedesk"
#define CONF_FILE "layout.conf"

static char* get_config_path(void) {
    const char *config_home = g_get_user_config_dir();
    char *dir_path = g_build_filename(config_home, CONF_DIR, NULL);
    g_mkdir_with_parents(dir_path, 0755);
    char *file_path = g_build_filename(dir_path, CONF_FILE, NULL);
    g_free(dir_path);
    return file_path;
}

void save_layout(void) {
    GKeyFile *keyfile = g_key_file_new();
    PhysicsContext *ctx = &app_state.physics;

    g_key_file_set_integer(keyfile, "Global", "cell_size", ctx->cell_size);
    g_key_file_set_integer(keyfile, "Global", "next_id", ctx->next_id);
    if (app_state.current_wallpaper_path) 
        g_key_file_set_string(keyfile, "Global", "wallpaper", app_state.current_wallpaper_path);

    for (GList *l = ctx->boxes; l != NULL; l = l->next) {
        Box *b = (Box*)l->data;
        char group[32];
        snprintf(group, 32, "Widget-%d", b->id);

        g_key_file_set_string(keyfile, group, "type", b->type);
        g_key_file_set_integer(keyfile, group, "variant", b->variant_index);
        
        g_key_file_set_integer(keyfile, group, "grid_x", b->grid_x);
        g_key_file_set_integer(keyfile, group, "grid_y", b->grid_y);
        g_key_file_set_integer(keyfile, group, "grid_w", b->grid_w);
        g_key_file_set_integer(keyfile, group, "grid_h", b->grid_h);
        g_key_file_set_double(keyfile, group, "nudge_x", b->nudge_x);
        g_key_file_set_double(keyfile, group, "nudge_y", b->nudge_y);
        
        g_key_file_set_boolean(keyfile, group, "transparent", b->is_transparent);
        g_key_file_set_boolean(keyfile, group, "is_24h", b->is_24h);
        g_key_file_set_integer(keyfile, group, "font_size_time", b->font_size_time);
        g_key_file_set_integer(keyfile, group, "font_size_date", b->font_size_date);
        g_key_file_set_integer(keyfile, group, "padding", b->padding); 
        
        g_key_file_set_boolean(keyfile, group, "use_dominant", b->use_dominant_color);
        if (b->custom_text) g_key_file_set_string(keyfile, group, "text", b->custom_text);
    }

    char *path = get_config_path();
    g_autofree char *data = g_key_file_to_data(keyfile, NULL, NULL);
    g_file_set_contents(path, data, -1, NULL);
    g_free(path);
    g_key_file_free(keyfile);
}

void load_layout(void) {
    char *path = get_config_path();
    GKeyFile *keyfile = g_key_file_new();
    if (!g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, NULL)) {
        g_free(path); g_key_file_free(keyfile); return;
    }

    PhysicsContext *ctx = &app_state.physics;
    if (g_key_file_has_key(keyfile, "Global", "cell_size", NULL)) 
        ctx->cell_size = g_key_file_get_integer(keyfile, "Global", "cell_size", NULL);
    if (g_key_file_has_key(keyfile, "Global", "next_id", NULL)) 
        ctx->next_id = g_key_file_get_integer(keyfile, "Global", "next_id", NULL);
    if (g_key_file_has_key(keyfile, "Global", "wallpaper", NULL)) {
        if (app_state.current_wallpaper_path) g_free(app_state.current_wallpaper_path);
        app_state.current_wallpaper_path = g_key_file_get_string(keyfile, "Global", "wallpaper", NULL);
    }

    gsize num_groups = 0;
    char **groups = g_key_file_get_groups(keyfile, &num_groups);

    for (size_t i = 0; i < num_groups; i++) {
        if (g_str_has_prefix(groups[i], "Widget-")) {
            char *raw_type = g_key_file_get_string(keyfile, groups[i], "type", NULL);
            if (!raw_type) continue;
            
            // Clean up old login widgets from storage silently
            if (g_strcmp0(raw_type, "Login") == 0 || g_strcmp0(raw_type, "LoginPassword") == 0 || g_strcmp0(raw_type, "LoginAvatar") == 0) {
                g_free(raw_type);
                continue;
            }

            Box *b = g_malloc0(sizeof(Box));
            sscanf(groups[i], "Widget-%d", &b->id);
            b->type = raw_type;
            
            if (g_key_file_has_key(keyfile, groups[i], "variant", NULL))
                b->variant_index = g_key_file_get_integer(keyfile, groups[i], "variant", NULL);
            else 
                b->variant_index = 0;

            b->grid_x = g_key_file_get_integer(keyfile, groups[i], "grid_x", NULL);
            b->grid_y = g_key_file_get_integer(keyfile, groups[i], "grid_y", NULL);
            b->grid_w = g_key_file_get_integer(keyfile, groups[i], "grid_w", NULL);
            b->grid_h = g_key_file_get_integer(keyfile, groups[i], "grid_h", NULL);
            b->nudge_x = g_key_file_get_double(keyfile, groups[i], "nudge_x", NULL);
            b->nudge_y = g_key_file_get_double(keyfile, groups[i], "nudge_y", NULL);

            const SceneFamily *fam = scene_registry_lookup_family(b->type);
            if (fam && b->variant_index >= 0 && b->variant_index < fam->variant_count) {
                b->widget = create_widget_content(b->type, b->variant_index);
                b->min_w = 1; b->min_h = 1; 
            } else {
                b->widget = gtk_label_new("Unknown Widget");
                b->min_w = 1; b->min_h = 1;
            }

            b->vis_x = (b->grid_x * ctx->cell_size) + b->nudge_x;
            b->vis_y = (b->grid_y * ctx->cell_size) + b->nudge_y;
            b->vis_w = b->grid_w * ctx->cell_size;
            b->vis_h = b->grid_h * ctx->cell_size;
            b->target_x = b->vis_x; b->target_y = b->vis_y;
            b->target_w = b->vis_w; b->target_h = b->vis_h;

            b->is_transparent = g_key_file_get_boolean(keyfile, groups[i], "transparent", NULL);
            b->is_24h = g_key_file_get_boolean(keyfile, groups[i], "is_24h", NULL);
            b->font_size_time = g_key_file_get_integer(keyfile, groups[i], "font_size_time", NULL);
            b->font_size_date = g_key_file_get_integer(keyfile, groups[i], "font_size_date", NULL);
            
            if (g_key_file_has_key(keyfile, groups[i], "padding", NULL))
                b->padding = g_key_file_get_integer(keyfile, groups[i], "padding", NULL);
            else
                b->padding = 0; 

            b->custom_text = g_key_file_get_string(keyfile, groups[i], "text", NULL);
            b->use_dominant_color = g_key_file_get_boolean(keyfile, groups[i], "use_dominant", NULL);

            gtk_fixed_put(GTK_FIXED(ctx->fixed_container), b->widget, 0, 0);
            apply_box_settings(b);
            ctx->boxes = g_list_append(ctx->boxes, b);
        }
    }

    g_strfreev(groups);
    g_key_file_free(keyfile);
    g_free(path);
}