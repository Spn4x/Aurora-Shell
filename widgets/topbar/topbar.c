#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <time.h>
#include "topbar.h"
#include "modules/sysinfo.h"
#include "modules/workspaces.h"
#include "modules/audio.h"
#include "modules/zen.h"

typedef struct {
    GtkWidget *clock_label;
    guint clock_timer_id;
} TopbarState;

static void load_module(JsonObject *module_config, TopbarState *state, GtkBox *target_box);
static gboolean update_clock(gpointer user_data);
static void topbar_cleanup(gpointer user_data);
static void on_generic_module_clicked(GtkButton *button, gpointer user_data);
static GtkWidget* create_popover_module(JsonObject *config);
static void free_string_data(gpointer data, GClosure *closure);
static void on_popover_button_clicked(GtkButton *button, gpointer user_data);

static void free_string_data(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

static void on_generic_module_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    const char *command = (const char *)user_data;
    if (command && *command) {
        g_info("Executing on-click command: [%s]", command);
        g_spawn_command_line_async(command, NULL);
    }
}

static void on_popover_button_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWidget *popover = GTK_WIDGET(user_data);
    gtk_popover_popup(GTK_POPOVER(popover));
}

static gboolean update_clock(gpointer user_data) {
    TopbarState *state = (TopbarState *)user_data;
    time_t raw_time;
    struct tm *time_info;
    char buffer[64];
    time(&raw_time);
    time_info = localtime(&raw_time);
    strftime(buffer, sizeof(buffer), "%d/%m/%Y %I:%M %p", time_info);
    gtk_label_set_text(GTK_LABEL(state->clock_label), buffer);
    return G_SOURCE_CONTINUE;
}

static void topbar_cleanup(gpointer data) {
    TopbarState *state = (TopbarState *)data;
    if (state->clock_timer_id > 0) {
        g_source_remove(state->clock_timer_id);
    }
    g_free(state);
}

static GtkWidget* create_popover_module(JsonObject *config) {
    const char *symbol = json_object_get_string_member_with_default(config, "symbol", "?");
    
    GtkWidget *button = gtk_button_new_with_label(symbol);
    gtk_widget_add_css_class(button, "popover-module");

    const char *name = json_object_get_string_member(config, "name");
    if (name) {
        gtk_widget_add_css_class(button, name);
    }

    GtkWidget *popover = gtk_popover_new();
    gtk_widget_set_parent(popover, button);

    GtkWidget *list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_popover_set_child(GTK_POPOVER(popover), list_box);

    if (json_object_has_member(config, "items")) {
        JsonArray *items_array = json_object_get_array_member(config, "items");
        for (guint i = 0; i < json_array_get_length(items_array); i++) {
            JsonObject *item_obj = json_array_get_object_element(items_array, i);
            if (!item_obj) continue;

            const char *label = json_object_get_string_member_with_default(item_obj, "label", "No Label");
            const char *command = json_object_get_string_member(item_obj, "on-click");
            const char* glyph = json_object_get_string_member(item_obj, "glyph");

            GtkWidget *item_button = gtk_button_new();
            gtk_widget_add_css_class(item_button, "popover-item");
            
            // ======================================================================
            // THE FIX: Add the .flat class to remove default button styling
            // ======================================================================
            gtk_widget_add_css_class(item_button, "flat");

            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_button_set_child(GTK_BUTTON(item_button), box);

            if (glyph) {
                GtkWidget *glyph_label = gtk_label_new(glyph);
                gtk_widget_add_css_class(glyph_label, "glyph-label");
                gtk_box_append(GTK_BOX(box), glyph_label);
            }

            GtkWidget *desc_label = gtk_label_new(label);
            gtk_label_set_xalign(GTK_LABEL(desc_label), 0.0);
            gtk_widget_set_hexpand(desc_label, TRUE);
            gtk_box_append(GTK_BOX(box), desc_label);

            if (command) {
                g_signal_connect_data(item_button, "clicked", G_CALLBACK(on_generic_module_clicked), 
                                      g_strdup(command), free_string_data, 0);
            }
            gtk_box_append(GTK_BOX(list_box), item_button);
        }
    }
    
    g_signal_connect(button, "clicked", G_CALLBACK(on_popover_button_clicked), popover);
    g_object_set_data_full(G_OBJECT(button), "popover", popover, g_object_unref);

    return button;
}

static void load_module(JsonObject *module_config, TopbarState *state, GtkBox *target_box) {
    GtkWidget *module_widget = NULL;
    
    const char *module_type = json_object_get_string_member(module_config, "type");
    
    if (module_type && g_strcmp0(module_type, "popover") == 0) {
        module_widget = create_popover_module(module_config);
    } else {
        const char *module_name = json_object_get_string_member(module_config, "name");
        if (!module_name) return;

        if (g_strcmp0(module_name, "clock") == 0) {
            GtkWidget *button = gtk_button_new(); gtk_widget_add_css_class(button, "clock-module");
            state->clock_label = gtk_label_new(""); gtk_button_set_child(GTK_BUTTON(button), state->clock_label);
            update_clock(state); state->clock_timer_id = g_timeout_add_seconds(1, update_clock, state);
            module_widget = button;
        } else if (g_strcmp0(module_name, "workspaces") == 0) {
            module_widget = create_workspaces_module();
        } else if (g_strcmp0(module_name, "sysinfo") == 0) {
            module_widget = create_sysinfo_module();
        } else if (g_strcmp0(module_name, "audio") == 0) {
            module_widget = create_audio_module();
        } else if (g_strcmp0(module_name, "zen") == 0) {
            module_widget = create_zen_module();
        }
    }
    
    if (module_widget) {
        if (json_object_has_member(module_config, "on-click")) {
            if (GTK_IS_BUTTON(module_widget)) {
                const char *cmd = json_object_get_string_member(module_config, "on-click");
                g_signal_connect_data(module_widget, "clicked", G_CALLBACK(on_generic_module_clicked), 
                                      g_strdup(cmd), free_string_data, 0);
            } else { g_warning("Module has 'on-click' but is not a clickable widget."); }
        }
        
        gtk_widget_add_css_class(module_widget, "module");
        if (GTK_IS_BUTTON(module_widget) || GTK_IS_MENU_BUTTON(module_widget)) {
            gtk_widget_add_css_class(module_widget, "flat");
        }
        gtk_box_append(target_box, module_widget);
    }
}

G_MODULE_EXPORT GtkWidget* create_widget(const char *config_string) {
    TopbarState *state = g_new0(TopbarState, 1);
    
    GtkWidget *root_container = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(root_container, "aurora-topbar");
    gtk_widget_add_css_class(root_container, "aurora-topbar-widget");
    g_object_set_data_full(G_OBJECT(root_container), "topbar-state", state, topbar_cleanup);

    GtkWidget *left_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(left_box, "left-modules");
    GtkWidget *center_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(center_box, "center-modules");
    GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(right_box, "right-modules");

    gtk_box_append(GTK_BOX(root_container), left_box);
    GtkWidget *center_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(center_spacer, TRUE);
    gtk_box_append(GTK_BOX(root_container), center_spacer);
    gtk_box_append(GTK_BOX(root_container), center_box);
    GtkWidget *right_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(right_spacer, TRUE);
    gtk_box_append(GTK_BOX(root_container), right_spacer);
    gtk_box_append(GTK_BOX(root_container), right_box);
    
    g_autoptr(JsonParser) parser = json_parser_new();
    if (config_string && *config_string) {
        if (json_parser_load_from_data(parser, config_string, -1, NULL)) {
            JsonObject *root_obj = json_node_get_object(json_parser_get_root(parser));
            if (json_object_has_member(root_obj, "config")) {
                JsonObject *config_obj = json_object_get_object_member(root_obj, "config");
                if (json_object_has_member(config_obj, "modules")) {
                    JsonObject *modules_obj = json_object_get_object_member(config_obj, "modules");
                    
                    GHashTable *section_map = g_hash_table_new(g_str_hash, g_str_equal);
                    g_hash_table_insert(section_map, "left", left_box);
                    g_hash_table_insert(section_map, "center", center_box);
                    g_hash_table_insert(section_map, "right", right_box);
                    
                    GList *members = json_object_get_members(modules_obj);
                    for (GList *l = members; l != NULL; l = l->next) {
                        const char *section_name = (const char *)l->data;
                        GtkBox *target_box = g_hash_table_lookup(section_map, section_name);
                        if (target_box) {
                            JsonArray *section_array = json_object_get_array_member(modules_obj, section_name);
                            for (guint i = 0; i < json_array_get_length(section_array); i++) {
                                JsonObject *module_config = json_array_get_object_element(section_array, i);
                                load_module(module_config, state, target_box);
                            }
                        }
                    }
                    g_list_free(members);
                    g_hash_table_destroy(section_map);
                }
            }
        }
    }
    
    return root_container;
}