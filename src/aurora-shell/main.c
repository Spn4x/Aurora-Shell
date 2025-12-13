// ===============================================
//  Aurora Shell - Main Host Application
// ===============================================

#include <glib.h>
#include <glib/gstdio.h> 
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <dlfcn.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <gio/gio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h> 

// ===============================================
// --- Type Definitions ---
// ===============================================
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

// Data for qscreen's "Capture First" logic
typedef struct {
    AuroraShell *shell;
    JsonObject *config_obj;
    gchar *temp_path;
} QScreenLaunchData;

// Global argv for restart logic
static char **global_argv = NULL;

static void load_all_widgets(AuroraShell *shell);
static WidgetState* create_single_widget(AuroraShell *shell, JsonObject *item_obj); 

// ===============================================
// --- Helper and Utility Functions ---
// ===============================================

// --- GLOBAL THEME MANAGEMENT ---

typedef struct {
    GtkCssProvider *provider;
    char *path;
    AuroraShell *shell;
} GlobalThemeData;

// THE FIX: Recursively force every single child widget to redraw.
// This guarantees that GtkDrawingAreas (like SysInfo bars) update instantly.
static void recursive_force_redraw(GtkWidget *widget) {
    if (!widget) return;
    
    // Force this specific widget to redraw
    gtk_widget_queue_draw(widget);
    
    // Iterate over children (GTK4 style) and force them too
    GtkWidget *child = gtk_widget_get_first_child(widget);
    while (child) {
        recursive_force_redraw(child);
        child = gtk_widget_get_next_sibling(child);
    }
}

static void on_global_theme_changed(GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event_type, gpointer user_data) {
    (void)monitor; (void)file; (void)other_file;
    
    // Wait for the write to finish completely
    if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT) {
        GlobalThemeData *data = (GlobalThemeData *)user_data;
        g_print("Global theme colors changed. Reloading from: %s\n", data->path);
        
        // 1. Reload the CSS definitions into the global display context
        gtk_css_provider_load_from_path(data->provider, data->path);

        // 2. NUCLEAR REDRAW: Force every pixel of every widget to repaint right now.
        if (data->shell && data->shell->widgets) {
            GHashTableIter iter;
            gpointer key, value;
            g_hash_table_iter_init(&iter, data->shell->widgets);
            while (g_hash_table_iter_next(&iter, &key, &value)) {
                WidgetState *ws = (WidgetState *)value;
                if (ws && ws->window) {
                    recursive_force_redraw(GTK_WIDGET(ws->window));
                }
            }
        }
    }
}

static void load_global_theme(AuroraShell *shell) {
    g_autofree gchar *colors_path = g_build_filename(g_get_user_config_dir(), "aurora-shell", "aurora-colors.css", NULL);
    
    if (!g_file_test(colors_path, G_FILE_TEST_EXISTS)) {
        g_print("Creating default global colors file at %s\n", colors_path);
        g_file_set_contents(colors_path, 
            "@define-color aurora_bg #1e1e2e;\n"
            "@define-color aurora_fg #cdd6f4;\n"
            "@define-color aurora_accent #89b4fa;\n"
            "@define-color aurora_surface #313244;\n", 
            -1, NULL);
    }

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(provider, colors_path);

    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), 
        GTK_STYLE_PROVIDER(provider), 
        GTK_STYLE_PROVIDER_PRIORITY_USER
    );

    GFile *file = g_file_new_for_path(colors_path);
    GFileMonitor *monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, NULL);
    
    if (monitor) {
        GlobalThemeData *data = g_new(GlobalThemeData, 1);
        data->provider = provider;
        data->path = g_strdup(colors_path);
        data->shell = shell; 
        g_signal_connect(monitor, "changed", G_CALLBACK(on_global_theme_changed), data);
    }
    
    g_object_unref(file);
}

static void ensure_user_config_exists() {
    g_autofree gchar *user_config_dir_path = g_build_filename(g_get_user_config_dir(), "aurora-shell", NULL);
    g_autofree gchar *user_config_file_path = g_build_filename(user_config_dir_path, "config.json", NULL);
    if (g_file_test(user_config_file_path, G_FILE_TEST_EXISTS)) { return; }
    g_print("First run: User config not found. Creating default configuration at %s\n", user_config_dir_path);
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) user_config_dir = g_file_new_for_path(user_config_dir_path);
    if (!g_file_make_directory_with_parents(user_config_dir, NULL, &error)) { g_warning("Failed to create user config directory: %s", error->message); return; }
    g_autoptr(GFile) default_config_file = g_file_new_for_path("/usr/local/share/aurora-shell/config.json");
    g_autoptr(GFile) user_config_file = g_file_new_for_path(user_config_file_path);
    if (!g_file_copy(default_config_file, user_config_file, G_FILE_COPY_NONE, NULL, NULL, NULL, &error)) { g_warning("Failed to copy default config.json: %s", error->message); }
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

static void hide_widget(WidgetState *state) {
    if (state && state->window && gtk_widget_get_visible(GTK_WIDGET(state->window))) {
        gtk_widget_set_visible(GTK_WIDGET(state->window), FALSE);
    }
}

static void on_mouse_enter(GtkEventControllerMotion *controller, double x, double y, gpointer user_data) {
    (void)controller; (void)x; (void)y;
    WidgetState *state = (WidgetState *)user_data;
    if (state && state->window) {
        gtk_widget_grab_focus(GTK_WIDGET(state->window));
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

static void on_widget_window_destroyed(GtkWidget *widget, gpointer user_data) {
    WidgetState *state = (WidgetState *)user_data;
    if (state->window == GTK_WINDOW(widget)) {
        state->window = NULL;
    }
}

static void free_widget_state(gpointer data) {
    WidgetState *state = (WidgetState *)data;
    if (state->window) {
        g_signal_handlers_disconnect_by_func(state->window, on_widget_window_destroyed, state);
        gtk_window_destroy(state->window);
        state->window = NULL;
    }
    g_free(state);
}


// ===============================================
// --- Core Shell Logic ---
// ===============================================

static void on_config_changed(GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event_type, gpointer user_data) {
    (void)monitor; (void)file; (void)other_file; (void)user_data;
    
    if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT) {
        g_print("\n>>> Configuration changed. PERFORMING HARD RESTART (execv) <<<\n");
        system("pkill -9 -x auroranotify-ui");
        system("pkill -9 -x auroranotifyd");
        system("pkill -9 -x aurora-insight-daemon");

        if (global_argv) {
            execv("/proc/self/exe", global_argv);
        } else {
            execl("/proc/self/exe", "aurora-shell", NULL);
        }
        g_critical("FATAL: Failed to re-execute aurora-shell: %s", g_strerror(errno));
        exit(1);
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
        gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_TOP, json_object_get_int_member_with_default(margin_obj, "top", 0));
        gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_BOTTOM, json_object_get_int_member_with_default(margin_obj, "bottom", 0));
        gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_LEFT, json_object_get_int_member_with_default(margin_obj, "left", 0));
        gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_RIGHT, json_object_get_int_member_with_default(margin_obj, "right", 0));
    }
}

static void launch_daemon_if_needed(const char *command) {
    g_autofree gchar *check_pattern = g_strdup_printf("[%c]%s", command[0], command + 1);
    char *argv[] = { "/usr/bin/pgrep", "-f", check_pattern, NULL };
    g_autoptr(GError) error = NULL; gint exit_status = 0;
    gboolean success = g_spawn_sync(NULL, (gchar**)argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &exit_status, &error);
    if (!success) { g_warning("Failed to run pgrep to check for daemon '%s': %s", command, error->message); }
    if (success && WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == 0) { g_print("Daemon '%s' is already running.\n", command); return; }
    g_print("Spawning daemon: '%s'\n", command);
    g_autoptr(GError) spawn_error = NULL;
    g_spawn_command_line_async(command, &spawn_error);
    if (spawn_error) { g_warning("Failed to spawn daemon '%s': %s", command, spawn_error->message); }
}

static WidgetState* create_single_widget(AuroraShell *shell, JsonObject *item_obj) {
    const char *name = json_object_get_string_member(item_obj, "name");
    const char *plugin_path = json_object_get_string_member(item_obj, "plugin");

    gboolean is_exclusive = json_object_get_boolean_member_with_default(item_obj, "exclusive", FALSE);
    gboolean is_interactive = json_object_get_boolean_member_with_default(item_obj, "interactive", FALSE);
    gboolean use_layer_shell = json_object_get_boolean_member_with_default(item_obj, "layer_shell", TRUE);

    GtkWindow *window = GTK_WINDOW(gtk_application_window_new(shell->app));
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(window));
    GtkCssProvider *transparency_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(transparency_provider, "window { background: transparent; }");
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(transparency_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(transparency_provider);
    
    void* handle = dlopen(plugin_path, RTLD_LAZY);
    if (!handle) { g_warning("Failed to open plugin '%s' for widget '%s': %s", plugin_path, name, dlerror()); gtk_window_destroy(window); return NULL; }
    
    CreateWidgetFunc create_widget = (CreateWidgetFunc)dlsym(handle, "create_widget");
    if (!create_widget) { g_warning("No 'create_widget' function in plugin '%s' for widget '%s'.", plugin_path, name); dlclose(handle); gtk_window_destroy(window); return NULL; }
    
    g_autofree gchar *config_string_to_pass = NULL;
    JsonNode *widget_node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(widget_node, item_obj);
    g_autoptr(JsonGenerator) generator = json_generator_new();
    json_generator_set_root(generator, widget_node);
    config_string_to_pass = json_generator_to_data(generator, NULL);
    json_node_free(widget_node);
    
    GtkWidget *widget = create_widget(config_string_to_pass);
    if (!widget) { g_warning("Plugin '%s' for widget '%s' returned a NULL widget.", plugin_path, name); dlclose(handle); gtk_window_destroy(window); return NULL; }

    if (g_strcmp0(name, "qscreen") == 0) {
        gtk_window_destroy(window); 
        window = GTK_WINDOW(gtk_application_window_new(shell->app)); 
        gtk_window_set_child(window, widget);
        gtk_window_set_default_size(window, 600, 400);
        gtk_window_set_title(window, "qscreen");
        is_exclusive = FALSE; 
        use_layer_shell = FALSE; 
    } else {
        gtk_window_set_child(window, widget);
    }
    
    if (use_layer_shell) {
        gtk_layer_init_for_window(window);
        if (is_exclusive) {
            gtk_layer_auto_exclusive_zone_enable(window);
            gtk_layer_set_keyboard_mode(window, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        } else {
            gtk_layer_set_keyboard_mode(window, GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
        }
        gtk_layer_set_layer(window, parse_layer_string(json_object_get_string_member_with_default(item_obj, "layer", "top")));
        apply_anchor_and_margins(window, widget, item_obj);
    } else if (g_strcmp0(name, "qscreen") != 0) {
        gtk_window_fullscreen(window);
    }


    if (json_object_has_member(item_obj, "stylesheet")) {
        const char *stylesheet_name = json_object_get_string_member(item_obj, "stylesheet");
        g_autofree gchar *user_templates_dir = g_build_filename(g_get_user_config_dir(), "aurora-shell", "templates", name, NULL);
        g_autofree gchar *stylesheet_path = g_build_filename(user_templates_dir, stylesheet_name, NULL);
        if (stylesheet_path && g_file_test(stylesheet_path, G_FILE_TEST_IS_REGULAR)) {
            g_print("Loading stylesheet: %s\n", stylesheet_path);
            GtkCssProvider *provider = gtk_css_provider_new();
            gtk_css_provider_load_from_path(provider, stylesheet_path);
            gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            GFile *css_file = g_file_new_for_path(stylesheet_path);
            GFileMonitor *monitor = g_file_monitor_file(css_file, G_FILE_MONITOR_NONE, NULL, NULL);
            CssReloadData *reload_data = g_new(CssReloadData, 1);
            reload_data->provider = provider; reload_data->path = g_strdup(stylesheet_path);
            g_signal_connect_data(monitor, "changed", G_CALLBACK(on_stylesheet_changed), reload_data, (GClosureNotify)free_css_reload_data, 0);
            g_object_set_data_full(G_OBJECT(window), "css-monitor", monitor, g_object_unref);
            g_object_unref(css_file);
        } else { g_warning("Stylesheet not found for '%s'. Searched at: %s", name, stylesheet_path ? stylesheet_path : "(null)"); }
    }
    
    WidgetState *state = g_new0(WidgetState, 1);
    state->window = window; state->widget = widget; state->is_interactive = is_interactive; state->config_obj = item_obj;

    g_signal_connect(window, "destroy", G_CALLBACK(on_widget_window_destroyed), state);

    if (!is_exclusive) {
        GtkEventController *key_controller = gtk_event_controller_key_new();
        g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_pressed), state);
        gtk_widget_add_controller(GTK_WIDGET(window), key_controller);
        GtkEventController *motion_controller = gtk_event_controller_motion_new();
        g_signal_connect(motion_controller, "enter", G_CALLBACK(on_mouse_enter), state);
        gtk_widget_add_controller(GTK_WIDGET(window), motion_controller);
    }
    return state;
}

static void load_all_widgets(AuroraShell *shell) {
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *user_config_file_path = g_build_filename(g_get_user_config_dir(), "aurora-shell", "config.json", NULL);
    
    GError *error = NULL;
    if (!json_parser_load_from_file(parser, user_config_file_path, &error)) {
        g_warning("Failed to parse config: %s", error->message);
        g_error_free(error);
        return;
    }
    
    shell->config_root = json_parser_steal_root(parser);
    if (!shell->config_root || json_node_get_node_type(shell->config_root) != JSON_NODE_ARRAY) {
        g_warning("Config root is not an array.");
        return;
    }

    JsonArray *config_array = json_node_get_array(shell->config_root);
    guint len = json_array_get_length(config_array);

    for (guint i = 0; i < len; i++) {
        JsonNode *element_node = json_array_get_element(config_array, i);
        if (!element_node || json_node_get_node_type(element_node) != JSON_NODE_OBJECT) {
            continue; 
        }

        JsonObject *item_obj = json_node_get_object(element_node);
        if (!item_obj) continue;

        const char *type = json_object_get_string_member_with_default(item_obj, "type", "widget");
        
        if (g_strcmp0(type, "daemon") == 0) {
            if (json_object_has_member(item_obj, "command")) {
                launch_daemon_if_needed(json_object_get_string_member(item_obj, "command"));
            }
            continue;
        }
        
        if (g_strcmp0(type, "widget") != 0) continue;
        
        if (!json_object_has_member(item_obj, "name")) continue;
        const char *name = json_object_get_string_member(item_obj, "name");
        
        WidgetState *state = create_single_widget(shell, item_obj);
        if (state) {
            if (json_object_get_boolean_member_with_default(item_obj, "visible_on_start", TRUE)) gtk_window_present(state->window);
            g_hash_table_insert(shell->widgets, g_strdup(name), state);
        }
    }
}


// ===============================================
// --- GApplication Signal Handlers ---
// ===============================================
static void on_qscreen_pre_capture_finished(GPid pid, gint status, gpointer user_data) {
    g_spawn_close_pid(pid);
    QScreenLaunchData *data = (QScreenLaunchData *)user_data;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        g_print("qscreen pre-capture successful. Creating window.\n");
        json_object_set_string_member(data->config_obj, "temp_screenshot_path", data->temp_path);
        const char *name = json_object_get_string_member(data->config_obj, "name");
        WidgetState *new_state = create_single_widget(data->shell, data->config_obj);
        if (new_state) {
            gtk_window_present(new_state->window);
            g_hash_table_replace(data->shell->widgets, g_strdup(name), new_state);
        }
    } else {
        g_warning("qscreen pre-capture command (grim) failed. Aborting.");
        g_remove(data->temp_path);
    }
    g_free(data->temp_path);
    g_free(data);
}

static int command_line_handler(GApplication *app, GApplicationCommandLine *cmdline, gpointer user_data) {
    AuroraShell *shell = (AuroraShell *)user_data;
    gchar **argv = g_application_command_line_get_arguments(cmdline, NULL);
    if (argv[1] && g_strcmp0(argv[1], "--toggle") == 0 && argv[2]) {
        const char *widget_name = argv[2];
        JsonArray *config_array = json_node_get_array(shell->config_root);
        JsonObject *item_obj = NULL;
        for (guint i = 0; i < json_array_get_length(config_array); i++) {
            JsonNode *element_node = json_array_get_element(config_array, i);
            if (!element_node || json_node_get_node_type(element_node) != JSON_NODE_OBJECT) {
                continue; 
            }
            JsonObject *current_obj = json_node_get_object(element_node);
            
            if (!json_object_has_member(current_obj, "name")) continue;
            
            const char *name = json_object_get_string_member(current_obj, "name");
            if (name && g_strcmp0(name, widget_name) == 0) { 
                item_obj = current_obj; 
                break; 
            }
        }
        if (!item_obj) { g_warning("Command line: No config for '%s' found.", widget_name); }
        else if (g_strcmp0(widget_name, "qscreen") == 0) {
            g_autofree gchar *temp_template = g_build_filename(g_get_tmp_dir(), "aurora-qscreen-XXXXXX.png", NULL);
            gint fd = g_mkstemp(temp_template);
            if (fd == -1) { g_warning("Failed to create temp file for qscreen"); g_strfreev(argv); return 1; }
            close(fd);
            QScreenLaunchData *launch_data = g_new0(QScreenLaunchData, 1);
            launch_data->shell = shell;
            launch_data->config_obj = item_obj;
            launch_data->temp_path = g_strdup(temp_template);
            g_autofree gchar *command = g_strdup_printf("grim \"%s\"", launch_data->temp_path);
            g_autoptr(GError) error = NULL;
            GPid child_pid;
            g_spawn_command_line_async(command, &error);
            g_spawn_async(NULL, (gchar*[]){"/bin/sh", "-c", command, NULL}, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &child_pid, &error);
            if (error) {
                g_warning("Failed to spawn grim: %s", error->message);
                g_free(launch_data->temp_path); g_free(launch_data);
            } else {
                g_child_watch_add(child_pid, on_qscreen_pre_capture_finished, launch_data);
            }
        }
        else {
            const char *type = json_object_get_string_member_with_default(item_obj, "type", "widget");
            if (g_strcmp0(type, "command") == 0 && json_object_has_member(item_obj, "command")) {
                const char *command_to_run = json_object_get_string_member(item_obj, "command");
                g_spawn_command_line_async(command_to_run, NULL);
            } else {
                WidgetState *state = g_hash_table_lookup(shell->widgets, widget_name);
                if (state) {
                    if (state->window) {
                        gboolean is_visible = gtk_widget_get_visible(GTK_WIDGET(state->window));
                        gtk_widget_set_visible(GTK_WIDGET(state->window), !is_visible);
                        if (!is_visible) {
                            if (state->is_interactive) { gtk_widget_grab_focus(GTK_WIDGET(state->window)); }
                            if (json_object_has_member(item_obj, "close")) {
                                JsonArray *widgets_to_close = json_object_get_array_member(item_obj, "close");
                                for (guint j = 0; j < json_array_get_length(widgets_to_close); j++) {
                                    const char *name_to_close = json_array_get_string_element(widgets_to_close, j);
                                    WidgetState *other_state = g_hash_table_lookup(shell->widgets, name_to_close);
                                    if (other_state) hide_widget(other_state);
                                }
                            }
                        }
                    }
                } else { g_warning("No loaded widget named '%s' found.", widget_name); }
            }
        }
    } else { g_application_activate(app); }
    g_strfreev(argv);
    return 0;
}

static void activate_handler(GApplication *app, gpointer user_data) {
    (void)app;
    AuroraShell *shell = (AuroraShell *)user_data;
    ensure_user_config_exists();
    
    // NEW: Load global theme colors before creating any widgets
    load_global_theme(shell);
    
    load_all_widgets(shell);
    
    g_autofree gchar *user_config_file_path = g_build_filename(g_get_user_config_dir(), "aurora-shell", "config.json", NULL);
    GFile *config_file = g_file_new_for_path(user_config_file_path);
    shell->config_monitor = g_file_monitor_file(config_file, G_FILE_MONITOR_NONE, NULL, NULL);
    if (shell->config_monitor) {
        g_signal_connect(shell->config_monitor, "changed", G_CALLBACK(on_config_changed), shell);
    }
    g_object_unref(config_file);
}


// ===============================================
// --- Main Program Entry Point ---
// ===============================================

int main(int argc, char **argv) {
    global_argv = argv;

    AuroraShell shell_data = {0};
    shell_data.widgets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_widget_state);
    shell_data.app = gtk_application_new("com.meismeric.aurora.shell", G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(shell_data.app, "activate", G_CALLBACK(activate_handler), &shell_data);
    g_signal_connect(G_APPLICATION(shell_data.app), "command-line", G_CALLBACK(command_line_handler), &shell_data);
    int status = g_application_run(G_APPLICATION(shell_data.app), argc, argv);
    if (shell_data.config_monitor) { g_object_unref(shell_data.config_monitor); }
    if (shell_data.config_root) { json_node_free(shell_data.config_root); }
    g_hash_table_destroy(shell_data.widgets);
    g_object_unref(shell_data.app);
    return status;
}