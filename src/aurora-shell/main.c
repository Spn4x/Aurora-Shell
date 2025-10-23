// ===============================================
//  Aurora Shell - Main Host Application
// ===============================================

#include <glib.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <dlfcn.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <gio/gio.h>

typedef struct {
    GtkWindow *window;
    GtkWidget *widget;
    gboolean is_interactive;
    JsonObject *config_obj;
} WidgetState;

typedef struct {
    GtkApplication *app;
    GHashTable *widgets;
    GFileMonitor *config_monitor;
    JsonNode *config_root;
} AuroraShell;

typedef GtkWidget* (*CreateWidgetFunc)(const char *config_string);

typedef struct {
    GtkCssProvider *provider;
    char *path;
} CssReloadData;

static void load_all_widgets(AuroraShell *shell);

static void ensure_user_config_exists() {
    g_autofree gchar *user_config_dir_path = g_build_filename(g_get_user_config_dir(), "aurora-shell", NULL);
    g_autofree gchar *user_config_file_path = g_build_filename(user_config_dir_path, "config.json", NULL);

    if (g_file_test(user_config_file_path, G_FILE_TEST_EXISTS)) {
        return;
    }

    g_print("First run: User config not found. Creating default configuration at %s\n", user_config_dir_path);

    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) user_config_dir = g_file_new_for_path(user_config_dir_path);
    if (!g_file_make_directory_with_parents(user_config_dir, NULL, &error)) {
        g_warning("Failed to create user config directory: %s", error->message);
        return;
    }

    g_autoptr(GFile) default_config_file = g_file_new_for_path("/usr/local/share/aurora-shell/config.json");
    g_autoptr(GFile) user_config_file = g_file_new_for_path(user_config_file_path);
    if (!g_file_copy(default_config_file, user_config_file, G_FILE_COPY_NONE, NULL, NULL, NULL, &error)) {
        g_warning("Failed to copy default config.json: %s", error->message);
    }

    g_autofree gchar *cp_command = g_strdup_printf("cp -r /usr/local/share/aurora-shell/templates %s", user_config_dir_path);
    g_spawn_command_line_sync(cp_command, NULL, NULL, NULL, NULL);
}

static void on_stylesheet_changed(GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event_type, gpointer user_data) {
    (void)monitor; (void)file; (void)other_file;
    if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT) {
        CssReloadData *data = (CssReloadData *)user_data;
        g_print("CSS file changed, reloading: %s\n", data->path);
        gtk_css_provider_load_from_path(data->provider, data->path);
    }
}

static void free_css_reload_data(gpointer data, GClosure *closure) {
    (void)closure;
    CssReloadData *reload_data = (CssReloadData *)data;
    g_free(reload_data->path);
    g_free(reload_data);
}

static void on_mouse_enter(GtkEventControllerMotion *controller, double x, double y, gpointer user_data) {
    (void)controller; (void)x; (void)y;
    WidgetState *state = (WidgetState *)user_data;
    gtk_widget_grab_focus(GTK_WIDGET(state->window));
}

static void hide_widget(WidgetState *state) {
    if (state && gtk_widget_get_visible(GTK_WIDGET(state->window))) {
        gtk_widget_set_visible(GTK_WIDGET(state->window), FALSE);
    }
}

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    (void)controller; (void)keycode; (void)state;
    if (keyval == GDK_KEY_Escape) {
        hide_widget((WidgetState *)user_data);
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

static void unload_all_widgets(AuroraShell *shell) {
    g_print("Unloading all widgets...\n");
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, shell->widgets);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        WidgetState *state = (WidgetState *)value;
        if (GTK_IS_WINDOW(state->window)) {
            gtk_window_destroy(state->window);
        }
    }
    g_hash_table_remove_all(shell->widgets);

    while (g_main_context_pending(NULL)) {
        g_main_context_iteration(NULL, FALSE);
    }
    g_print("All widgets unloaded.\n");
}

static void on_config_changed(GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event_type, gpointer user_data) {
    (void)monitor; (void)file; (void)other_file;
    if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT) {
        g_print("Configuration file changed. Reloading shell...\n");
        AuroraShell *shell = (AuroraShell *)user_data;
        unload_all_widgets(shell);
        if (shell->config_root) {
            json_node_free(shell->config_root);
            shell->config_root = NULL;
        }
        load_all_widgets(shell);
    }
}

static GtkLayerShellLayer parse_layer_string(const gchar *layer_str) {
    if (g_strcmp0(layer_str, "bottom") == 0) return GTK_LAYER_SHELL_LAYER_BOTTOM;
    if (g_strcmp0(layer_str, "background") == 0) return GTK_LAYER_SHELL_LAYER_BACKGROUND;
    if (g_strcmp0(layer_str, "overlay") == 0) return GTK_LAYER_SHELL_LAYER_OVERLAY;
    return GTK_LAYER_SHELL_LAYER_TOP;
}

static void apply_anchor_and_margins(GtkWindow *window, GtkWidget *widget, JsonObject *widget_obj) {
    const char *anchor_str = json_object_get_string_member_with_default(widget_obj, "anchor", "center");
    if (strstr(anchor_str, "top"))    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    if (strstr(anchor_str, "bottom")) gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    if (strstr(anchor_str, "left"))   gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    if (strstr(anchor_str, "right"))  gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    if (strstr(anchor_str, "left"))       gtk_widget_set_halign(widget, GTK_ALIGN_START);
    else if (strstr(anchor_str, "right")) gtk_widget_set_halign(widget, GTK_ALIGN_END);
    else                                  gtk_widget_set_halign(widget, GTK_ALIGN_CENTER);
    if (strstr(anchor_str, "top"))        gtk_widget_set_valign(widget, GTK_ALIGN_START);
    else if (strstr(anchor_str, "bottom"))gtk_widget_set_valign(widget, GTK_ALIGN_END);
    else                                  gtk_widget_set_valign(widget, GTK_ALIGN_CENTER);
    if (json_object_has_member(widget_obj, "margin")) {
        JsonObject *margin_obj = json_object_get_object_member(widget_obj, "margin");
        gtk_widget_set_margin_top(widget, json_object_get_int_member_with_default(margin_obj, "top", 0));
        gtk_widget_set_margin_bottom(widget, json_object_get_int_member_with_default(margin_obj, "bottom", 0));
        gtk_widget_set_margin_start(widget, json_object_get_int_member_with_default(margin_obj, "left", 0));
        gtk_widget_set_margin_end(widget, json_object_get_int_member_with_default(margin_obj, "right", 0));
    }
}

static void launch_daemon_if_needed(const char *command) {
    g_autofree gchar *check_command = g_strdup_printf("pgrep -f %s", command);
    if (g_spawn_command_line_sync(check_command, NULL, NULL, NULL, NULL)) {
        g_print("Daemon '%s' is already running.\n", command);
        return;
    }

    g_print("Spawning daemon: '%s'\n", command);
    g_autoptr(GError) error = NULL;
    gchar **argv = g_strsplit(command, " ", -1);

    g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error);
    g_strfreev(argv);

    if (error) {
        g_warning("Failed to spawn daemon '%s': %s", command, error->message);
    }
}

static void load_all_widgets(AuroraShell *shell) {
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *user_config_file_path = g_build_filename(g_get_user_config_dir(), "aurora-shell", "config.json", NULL);
    if (!json_parser_load_from_file(parser, user_config_file_path, NULL)) {
        g_warning("Failed to load or parse user config: %s", user_config_file_path);
        return;
    }

    shell->config_root = json_parser_steal_root(parser);
    if (!shell->config_root || !JSON_NODE_HOLDS_ARRAY(shell->config_root)) {
        g_warning("Config file root is not a valid JSON array. Nothing to load.");
        if (shell->config_root) {
            json_node_free(shell->config_root);
            shell->config_root = NULL;
        }
        return;
    }
    
    JsonArray *config_array = json_node_get_array(shell->config_root);
    for (guint i = 0; i < json_array_get_length(config_array); i++) {
        if (!JSON_NODE_HOLDS_OBJECT(json_array_get_element(config_array, i))) {
            continue;
        }

        JsonObject *item_obj = json_array_get_object_element(config_array, i);
        
        const char *type = json_object_get_string_member_with_default(item_obj, "type", "widget");

        if (g_strcmp0(type, "daemon") == 0) {
            const char *command = json_object_get_string_member(item_obj, "command");
            if (command) {
                launch_daemon_if_needed(command);
            }
            continue;
        }

        const char *name = json_object_get_string_member(item_obj, "name");
        if (!name || !json_object_has_member(item_obj, "plugin")) {
            continue;
        }

        GtkWindow *window = GTK_WINDOW(gtk_application_window_new(shell->app));
        gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
        GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(window));
        
        GtkCssProvider *transparency_provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(transparency_provider, "window { background: transparent; }");
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(transparency_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(transparency_provider);

        gtk_layer_init_for_window(window);
        
        if (json_object_has_member(item_obj, "size")) {
            JsonObject *size_obj = json_object_get_object_member(item_obj, "size");
            gtk_window_set_default_size(window, json_object_get_int_member_with_default(size_obj, "width", -1), json_object_get_int_member_with_default(size_obj, "height", -1));
        }

        const char *plugin_path = json_object_get_string_member(item_obj, "plugin");
        void* handle = dlopen(plugin_path, RTLD_LAZY);
        if (!handle) {
            g_warning("Failed to open plugin '%s' for widget '%s': %s", plugin_path, name, dlerror());
            gtk_window_destroy(window);
            continue;
        }

        CreateWidgetFunc create_widget = (CreateWidgetFunc)dlsym(handle, "create_widget");
        if (!create_widget) {
            g_warning("No 'create_widget' function in plugin '%s' for widget '%s'.", plugin_path, name);
            dlclose(handle);
            gtk_window_destroy(window);
            continue;
        }

        g_autofree gchar *config_string_to_pass = NULL;
        JsonNode *widget_node = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(widget_node, item_obj);
        g_autoptr(JsonGenerator) generator = json_generator_new();
        json_generator_set_root(generator, widget_node);
        config_string_to_pass = json_generator_to_data(generator, NULL);
        json_node_free(widget_node);
        
        GtkWidget *widget = create_widget(config_string_to_pass);
        if (!widget) {
            g_warning("Plugin '%s' for widget '%s' returned a NULL widget.", plugin_path, name);
            dlclose(handle);
            gtk_window_destroy(window);
            continue;
        }
        gtk_window_set_child(window, widget);

        gboolean is_exclusive = json_object_get_boolean_member_with_default(item_obj, "exclusive", FALSE);
        gboolean is_interactive = json_object_get_boolean_member_with_default(item_obj, "interactive", FALSE);

        if (is_exclusive) {
            gtk_layer_auto_exclusive_zone_enable(window);
        }
        
        // ======================================================================
        // THE FINAL, CORRECT FOCUS LOGIC
        // ======================================================================

        // 1. Set keyboard mode: All pop-ups must be able to get focus to be escapable.
        if (!is_exclusive) {
            gtk_layer_set_keyboard_mode(window, GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
        } else {
            gtk_layer_set_keyboard_mode(window, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        }
        
        gtk_layer_set_layer(window, parse_layer_string(json_object_get_string_member_with_default(item_obj, "layer", "top")));
        apply_anchor_and_margins(window, widget, item_obj);

        if (json_object_has_member(item_obj, "margin")) {
            JsonObject *margins_obj = json_object_get_object_member(item_obj, "margin");
            gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_TOP, json_object_get_int_member_with_default(margins_obj, "top", 0));
            gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_BOTTOM, json_object_get_int_member_with_default(margins_obj, "bottom", 0));
            gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_LEFT, json_object_get_int_member_with_default(margins_obj, "left", 0));
            gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_RIGHT, json_object_get_int_member_with_default(margins_obj, "right", 0));
        }
        
        if (json_object_has_member(item_obj, "stylesheet")) {
            const char *stylesheet_name = json_object_get_string_member(item_obj, "stylesheet");
            g_autofree gchar *user_templates_dir = g_build_filename(g_get_user_config_dir(), "aurora-shell", "templates", name, NULL);
            g_autofree gchar *stylesheet_path = g_build_filename(user_templates_dir, stylesheet_name, NULL);

            if (stylesheet_path && g_file_test(stylesheet_path, G_FILE_TEST_IS_REGULAR)) {
                g_print("Loading stylesheet: %s\n", stylesheet_path);
                GtkCssProvider *provider = gtk_css_provider_new();
                gtk_css_provider_load_from_path(provider, stylesheet_path);
                gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_USER);
                
                GFile *css_file = g_file_new_for_path(stylesheet_path);
                GFileMonitor *monitor = g_file_monitor_file(css_file, G_FILE_MONITOR_NONE, NULL, NULL);
                CssReloadData *reload_data = g_new(CssReloadData, 1);
                reload_data->provider = provider;
                reload_data->path = g_strdup(stylesheet_path);
                g_signal_connect_data(monitor, "changed", G_CALLBACK(on_stylesheet_changed), reload_data, (GClosureNotify)free_css_reload_data, 0);
                g_object_set_data_full(G_OBJECT(window), "css-monitor", monitor, g_object_unref);
                g_object_unref(css_file);
            } else {
                g_warning("Stylesheet not found for '%s'. Searched at: %s", name, stylesheet_path ? stylesheet_path : "(null)");
            }
        }
        
        // 2. Grab focus on start: *Only* for pop-ups, so they can be escaped.
        if (json_object_get_boolean_member_with_default(item_obj, "visible_on_start", TRUE)) {
            gtk_window_present(window);
            if (!is_exclusive) {
                 gtk_widget_grab_focus(GTK_WIDGET(window));
            }
        }
        
        WidgetState *state = g_new0(WidgetState, 1);
        state->window = window;
        state->widget = widget;
        state->is_interactive = is_interactive;
        state->config_obj = item_obj;

        // 3. Attach Escape key handler to ALL pop-ups.
        if (!is_exclusive) {
            GtkEventController *key_controller = gtk_event_controller_key_new();
            g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_pressed), state);
            gtk_widget_add_controller(GTK_WIDGET(window), key_controller);
        }

        // 4. Attach mouse-over focus grab: ONLY to truly interactive widgets. This prevents unwanted focus stealing.
        if (state->is_interactive) {
            GtkEventController *motion_controller = gtk_event_controller_motion_new();
            g_signal_connect(motion_controller, "enter", G_CALLBACK(on_mouse_enter), state);
            gtk_widget_add_controller(GTK_WIDGET(window), motion_controller);
        }
        
        g_hash_table_insert(shell->widgets, g_strdup(name), state);
    }
}

static int command_line_handler(GApplication *app, GApplicationCommandLine *cmdline, gpointer user_data) {
    AuroraShell *shell = (AuroraShell *)user_data;
    gchar **argv = g_application_command_line_get_arguments(cmdline, NULL);

    if (argv[1] && g_strcmp0(argv[1], "--toggle") == 0 && argv[2]) {
        if (g_strcmp0(argv[2], "themer") == 0) {
            g_spawn_command_line_async("wallpaper.sh", NULL);
        } else {
            WidgetState *state = g_hash_table_lookup(shell->widgets, argv[2]);
            if (state) {
                gboolean is_visible = gtk_widget_get_visible(GTK_WIDGET(state->window));
                gtk_widget_set_visible(GTK_WIDGET(state->window), !is_visible);
                
                if (!is_visible) {
                    gtk_window_present(state->window);
                    
                    gboolean is_exclusive = json_object_get_boolean_member_with_default(state->config_obj, "exclusive", FALSE);
                    // 5. Grab focus on toggle: *Only* for pop-ups, so they can be escaped.
                    if (!is_exclusive) {
                        gtk_widget_grab_focus(GTK_WIDGET(state->window));
                    }

                    if (json_object_has_member(state->config_obj, "close")) {
                        JsonArray *close_array = json_object_get_array_member(state->config_obj, "close");
                        for (guint i = 0; i < json_array_get_length(close_array); i++) {
                            const char *widget_to_close_name = json_array_get_string_element(close_array, i);
                            WidgetState *state_to_close = g_hash_table_lookup(shell->widgets, widget_to_close_name);
                            if (state_to_close) {
                                gtk_widget_set_visible(GTK_WIDGET(state_to_close->window), FALSE);
                            }
                        }
                    }
                }
            } else {
                g_warning("Command line: No widget named '%s' found.", argv[2]);
            }
        }
    } else {
        g_application_activate(app);
    }
    
    g_strfreev(argv);
    return 0;
}

static void activate_handler(GApplication *app, gpointer user_data) {
    (void)app;
    AuroraShell *shell = (AuroraShell *)user_data;
    
    ensure_user_config_exists();
    load_all_widgets(shell);

    g_autofree gchar *user_config_file_path = g_build_filename(g_get_user_config_dir(), "aurora-shell", "config.json", NULL);
    GFile *config_file = g_file_new_for_path(user_config_file_path);

    shell->config_monitor = g_file_monitor_file(config_file, G_FILE_MONITOR_NONE, NULL, NULL);
    if (shell->config_monitor) {
        g_print("Monitoring config file for changes: %s\n", user_config_file_path);
        g_signal_connect(shell->config_monitor, "changed", G_CALLBACK(on_config_changed), shell);
    } else {
        g_warning("Could not create file monitor for config file.");
    }

    g_object_unref(config_file);
}

static void free_widget_state(gpointer data) {
    g_free(data);
}

int main(int argc, char **argv) {
    AuroraShell shell_data = {0};
    shell_data.widgets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_widget_state);
    shell_data.app = gtk_application_new("com.meismeric.aurora.shell", G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(shell_data.app, "activate", G_CALLBACK(activate_handler), &shell_data);
    g_signal_connect(G_APPLICATION(shell_data.app), "command-line", G_CALLBACK(command_line_handler), &shell_data);

    int status = g_application_run(G_APPLICATION(shell_data.app), argc, argv);
    
    if (shell_data.config_monitor) {
        g_object_unref(shell_data.config_monitor);
    }
    if (shell_data.config_root) {
        json_node_free(shell_data.config_root);
    }
    
    g_hash_table_destroy(shell_data.widgets);
    g_object_unref(shell_data.app);
    return status;
}