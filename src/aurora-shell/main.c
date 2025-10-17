// ===============================================
//  Aurora Shell - Main Host Application
//  (Final, Corrected, Simplified)
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
} WidgetState;

typedef struct {
    GtkApplication *app;
    GHashTable *widgets;
} AuroraShell;

typedef GtkWidget* (*CreateWidgetFunc)(const char *config_string);

typedef struct {
    GtkCssProvider *provider;
    char *path;
} CssReloadData;

// --- XDG Configuration Management ---
static void ensure_user_config_exists() {
    g_autofree gchar *user_config_dir_path = g_build_filename(g_get_user_config_dir(), "aurora-shell", NULL);
    g_autofree gchar *user_config_file_path = g_build_filename(user_config_dir_path, "config.json", NULL);

    if (g_file_test(user_config_file_path, G_FILE_TEST_EXISTS)) {
        return; // Config already exists, our job is done.
    }

    g_print("First run: User config not found. Creating default configuration at %s\n", user_config_dir_path);

    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) user_config_dir = g_file_new_for_path(user_config_dir_path);
    if (!g_file_make_directory_with_parents(user_config_dir, NULL, &error)) {
        g_warning("Failed to create user config directory: %s", error->message);
        return;
    }

    // Copy the default config.json
    g_autoptr(GFile) default_config_file = g_file_new_for_path("/usr/local/share/aurora-shell/config.json");
    g_autoptr(GFile) user_config_file = g_file_new_for_path(user_config_file_path);
    if (!g_file_copy(default_config_file, user_config_file, G_FILE_COPY_NONE, NULL, NULL, NULL, &error)) {
        g_warning("Failed to copy default config.json: %s", error->message);
    }

    // Use a simple, robust 'cp' command to recursively copy the templates directory.
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

static void load_all_widgets(AuroraShell *shell) {
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *user_config_file_path = g_build_filename(g_get_user_config_dir(), "aurora-shell", "config.json", NULL);
    if (!json_parser_load_from_file(parser, user_config_file_path, NULL)) {
        g_warning("Failed to load or parse user config: %s", user_config_file_path);
        return;
    }

    JsonArray *widget_array = json_node_get_array(json_parser_get_root(parser));
    for (guint i = 0; i < json_array_get_length(widget_array); i++) {
        JsonObject *widget_obj = json_array_get_object_element(widget_array, i);
        const char *name = json_object_get_string_member(widget_obj, "name");
        if (!name || !json_object_has_member(widget_obj, "plugin")) {
            continue;
        }

        GtkWindow *window = GTK_WINDOW(gtk_application_window_new(shell->app));
        GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(window));
        
        GtkCssProvider *transparency_provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(transparency_provider, "window { background: transparent; }");
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(transparency_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(transparency_provider);

        gtk_layer_init_for_window(window);
        
        if (json_object_has_member(widget_obj, "size")) {
            JsonObject *size_obj = json_object_get_object_member(widget_obj, "size");
            gtk_window_set_default_size(window, json_object_get_int_member_with_default(size_obj, "width", -1), json_object_get_int_member_with_default(size_obj, "height", -1));
        }

        const char *plugin_path = json_object_get_string_member(widget_obj, "plugin");
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
        json_node_set_object(widget_node, json_object_ref(widget_obj));
        g_autoptr(JsonGenerator) generator = json_generator_new();
        json_generator_set_root(generator, widget_node);
        config_string_to_pass = json_generator_to_data(generator, NULL);
        json_node_free(widget_node);
        
        GtkWidget *widget = create_widget(config_string_to_pass);
        gtk_window_set_child(window, widget);

        if (json_object_get_boolean_member_with_default(widget_obj, "exclusive", FALSE)) {
            gtk_layer_auto_exclusive_zone_enable(window);
        }

        gtk_layer_set_keyboard_mode(window, json_object_get_boolean_member_with_default(widget_obj, "interactive", FALSE) ? GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND : GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_layer_set_layer(window, parse_layer_string(json_object_get_string_member_with_default(widget_obj, "layer", "top")));
        apply_anchor_and_margins(window, widget, widget_obj);

        if (json_object_has_member(widget_obj, "margins")) {
            JsonObject *margins_obj = json_object_get_object_member(widget_obj, "margins");
            gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_TOP, json_object_get_int_member_with_default(margins_obj, "top", 0));
            gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_BOTTOM, json_object_get_int_member_with_default(margins_obj, "bottom", 0));
            gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_LEFT, json_object_get_int_member_with_default(margins_obj, "left", 0));
            gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_RIGHT, json_object_get_int_member_with_default(margins_obj, "right", 0));
        }
        
        if (json_object_has_member(widget_obj, "stylesheet")) {
            const char *stylesheet_name = json_object_get_string_member(widget_obj, "stylesheet");
            g_autofree gchar *stylesheet_path = NULL;
            
            g_autofree gchar *user_templates_dir = g_build_filename(g_get_user_config_dir(), "aurora-shell", "templates", name, NULL);
            stylesheet_path = g_build_filename(user_templates_dir, stylesheet_name, NULL);

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
        
        if (json_object_get_boolean_member_with_default(widget_obj, "visible_on_start", TRUE)) {
            gtk_window_present(window);
        }
        
        WidgetState *state = g_new0(WidgetState, 1);
        state->window = window;
        state->widget = widget;
        state->is_interactive = json_object_get_boolean_member_with_default(widget_obj, "interactive", FALSE);

        if (state->is_interactive) {
            GtkEventController *key_controller = gtk_event_controller_key_new();
            g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_pressed), state);
            gtk_widget_add_controller(GTK_WIDGET(window), key_controller);
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
            // It's our special command! Execute wallpaper.sh and exit.
            g_spawn_command_line_async("wallpaper.sh", NULL);
        } else {
            // It's a normal widget toggle.
            WidgetState *state = g_hash_table_lookup(shell->widgets, argv[2]);
            if (state) {
                gboolean is_visible = gtk_widget_get_visible(GTK_WIDGET(state->window));
                gtk_widget_set_visible(GTK_WIDGET(state->window), !is_visible);
                if (!is_visible) {
                    gtk_window_present(state->window);
                }
            } else {
                g_warning("Command line: No widget named '%s' found.", argv[2]);
            }
        }
    } else {
        // No arguments were passed, so do the normal startup.
        g_application_activate(app);
    }
    
    g_strfreev(argv);
    return 0;
}

// THE FIX IS HERE: This logic is now simpler and guaranteed to run.
static void activate_handler(GApplication *app, gpointer user_data) {
    (void)app;
    AuroraShell *shell = (AuroraShell *)user_data;
    
    // Always check for user config on startup.
    ensure_user_config_exists();

    // Always load the widgets on startup.
    // The GApplication logic ensures this only runs once for the main process.
    load_all_widgets(shell);
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
    //g_application_hold(G_APPLICATION(shell_data.app));
    int status = g_application_run(G_APPLICATION(shell_data.app), argc, argv);
    
    g_hash_table_destroy(shell_data.widgets);
    g_object_unref(shell_data.app);
    return status;
}