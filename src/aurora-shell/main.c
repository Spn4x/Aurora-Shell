// ===============================================
//  Aurora Shell - Main Host Application
//  (with CSS Hot Reloading)
// ===============================================

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

// --- NEW: Data structure for CSS Hot Reloading ---
typedef struct {
    GtkCssProvider *provider;
    char *path;
} CssReloadData;

static gchar* resolve_asset_path(const char *path_from_config, const char *project_root) {
    if (g_str_has_prefix(path_from_config, "{project_root}")) {
        // Skip past the "{project_root}" part of the string
        const char *relative_path = path_from_config + strlen("{project_root}");
        // g_build_filename correctly handles the '/' between path components.
        return g_build_filename(project_root, relative_path, NULL);
    }
    // If the placeholder isn't there, return a copy of the original path.
    return g_strdup(path_from_config);
}

// This function is called when the CSS file monitor detects a change.
static void on_stylesheet_changed(GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event_type, gpointer user_data) {
    // We only care when the file's contents are finished changing.
    (void)monitor; (void)file; (void)other_file;
    if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT) {
        CssReloadData *data = (CssReloadData *)user_data;
        g_print("CSS file changed, reloading: %s\n", data->path);
        gtk_css_provider_load_from_path(data->provider, data->path);
    }
}

// This function is called to clean up the reload data when the monitor is destroyed.
static void free_css_reload_data(gpointer data) {
    CssReloadData *reload_data = (CssReloadData *)data;
    g_free(reload_data->path);
    // The provider is owned by the GdkDisplay now, so we don't unref it here.
    g_free(reload_data);
}


// --- Event Handlers ---

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


// --- Static Helper Functions ---

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

// --- Core Logic ---
static void load_all_widgets(AuroraShell *shell) {
    // Determine the project root path at runtime
    g_autofree gchar *project_root = NULL;
    g_autofree gchar *exe_path = g_file_read_link("/proc/self/exe", NULL);
    if (exe_path) {
        g_autofree gchar *exe_dir = g_path_get_dirname(exe_path);
        // Assuming a layout like: ./build/src/aurora-shell/aurora-shell
        g_autofree gchar *src_dir = g_path_get_dirname(exe_dir);
        g_autofree gchar *build_dir = g_path_get_dirname(src_dir);
        project_root = g_path_get_dirname(build_dir);
        g_print("Detected project root: %s\n", project_root);
    } else {
        g_warning("Could not determine executable path. Stylesheet paths may be incorrect.");
        project_root = g_get_current_dir(); // Fallback
    }

    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_file(parser, "config.json", NULL)) {
        g_warning("Failed to load or parse config.json");
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_ARRAY(root)) {
        g_warning("config.json root is not a JSON array.");
        return;
    }
    JsonArray *widget_array = json_node_get_array(root);

    for (guint i = 0; i < json_array_get_length(widget_array); i++) {
        JsonObject *widget_obj = json_array_get_object_element(widget_array, i);
        const char *name = json_object_get_string_member(widget_obj, "name");
        if (!name) {
            g_warning("Skipping widget at index %u due to missing 'name'", i);
            continue;
        }

        GtkWindow *window = GTK_WINDOW(gtk_application_window_new(shell->app));
        GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(window));
        
        // Make window background transparent so CSS styles (like rounded corners) can show through.
        GtkCssProvider *transparency_provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(transparency_provider, "window { background: transparent; }");
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(transparency_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(transparency_provider);

        // Initialize layer shell for this window. Must be done before showing the window.
        gtk_layer_init_for_window(window);
        
        // Handle widget size directly on the GtkWindow (still a good practice for non-plugin-sized widgets)
        if (json_object_has_member(widget_obj, "size")) {
            JsonObject *size_obj = json_object_get_object_member(widget_obj, "size");
            int width = json_object_get_int_member_with_default(size_obj, "width", -1);
            int height = json_object_get_int_member_with_default(size_obj, "height", -1);

            g_print("Orchestrator: Setting default size for '%s' to %dx%d\n", name, width, height);
            gtk_window_set_default_size(window, width, height);
        }

        // --- Load the plugin from the specified shared object file ---
        const char *plugin_path = json_object_get_string_member(widget_obj, "plugin");
        if (!plugin_path) {
            g_warning("Widget '%s' has no 'plugin' path. Skipping.", name);
            gtk_window_destroy(window);
            continue;
        }

        void* handle = dlopen(plugin_path, RTLD_LAZY);
        if (!handle) {
            g_warning("Failed to open plugin '%s' for widget '%s': %s", plugin_path, name, dlerror());
            gtk_window_destroy(window);
            continue;
        }

        CreateWidgetFunc create_widget = (CreateWidgetFunc)dlsym(handle, "create_widget");
        if (!create_widget) {
            g_warning("No 'create_widget' function found in plugin '%s' for widget '%s'.", plugin_path, name);
            dlclose(handle);
            gtk_window_destroy(window);
            continue;
        }

        // =========================================================================
        // <<< THE BUG FIX IS HERE >>>
        // =========================================================================
        // OLD CODE: Only checked for "config" or "text", ignoring "size", "animation", etc.
        // NEW CODE: Serializes the ENTIRE widget object from config.json and passes
        //           it as a string, making the system far more flexible.

        g_autofree gchar *config_string_to_pass = NULL;

        JsonNode *widget_node = json_node_new(JSON_NODE_OBJECT);
        // We must ref the object because json_node_set_object takes full ownership,
        // and we don't want the parser's object to be freed prematurely.
        json_node_set_object(widget_node, json_object_ref(widget_obj));

        g_autoptr(JsonGenerator) generator = json_generator_new();
        json_generator_set_root(generator, widget_node);
        config_string_to_pass = json_generator_to_data(generator, NULL);
        
        json_node_free(widget_node); // Free the temporary node wrapper
        
        // config_string_to_pass now contains the full JSON for the widget
        GtkWidget *widget = create_widget(config_string_to_pass);
        // =========================================================================

        gtk_window_set_child(window, widget);

        // --- Configure Layer Shell Properties ---
        if (json_object_get_boolean_member_with_default(widget_obj, "exclusive", FALSE)) {
            g_print("Widget '%s' is setting an exclusive zone.\n", name);
            gtk_layer_auto_exclusive_zone_enable(window);
        }

        gboolean is_interactive = json_object_get_boolean_member_with_default(widget_obj, "interactive", FALSE);
        if (is_interactive) {
            gtk_layer_set_keyboard_mode(window, GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
        } else {
            gtk_layer_set_keyboard_mode(window, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        }

        const gchar *layer_str = json_object_get_string_member_with_default(widget_obj, "layer", "top");
        gtk_layer_set_layer(window, parse_layer_string(layer_str));
        
        apply_anchor_and_margins(window, widget, widget_obj);

        if (json_object_has_member(widget_obj, "margins")) {
            JsonObject *margins_obj = json_object_get_object_member(widget_obj, "margins");
            int top    = json_object_get_int_member_with_default(margins_obj, "top", 0);
            int bottom = json_object_get_int_member_with_default(margins_obj, "bottom", 0);
            int left   = json_object_get_int_member_with_default(margins_obj, "left", 0);
            int right  = json_object_get_int_member_with_default(margins_obj, "right", 0);

            gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_TOP, top);
            gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_BOTTOM, bottom);
            gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_LEFT, left);
            gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_RIGHT, right);
        }
        
        // --- Setup CSS Provider and Hot Reloading ---
        if (json_object_has_member(widget_obj, "stylesheet")) {
            const char *stylesheet_path_from_config = json_object_get_string_member(widget_obj, "stylesheet");
            g_autofree gchar *stylesheet_path = resolve_asset_path(stylesheet_path_from_config, project_root);
            if (stylesheet_path && g_file_test(stylesheet_path, G_FILE_TEST_IS_REGULAR)) {
                g_print("Orchestrator: Loading and monitoring stylesheet '%s' for widget '%s'\n", stylesheet_path, name);
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
                g_warning("Stylesheet not found for widget '%s' at resolved path: %s", name, stylesheet_path);
            }
        }
        
        if (json_object_get_boolean_member_with_default(widget_obj, "visible_on_start", TRUE)) {
            gtk_window_present(window);
        }
        
        WidgetState *state = g_new0(WidgetState, 1);
        state->window = window;
        state->widget = widget;
        state->is_interactive = is_interactive;

        if (is_interactive) {
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
    g_application_activate(app);
    gchar **argv;
    gint argc;
    argv = g_application_command_line_get_arguments(cmdline, &argc);
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--toggle") == 0 && i + 1 < argc) {
            WidgetState *state = g_hash_table_lookup(shell->widgets, argv[i + 1]);
            if (!state) { g_warning("Command line: No widget named '%s' found.", argv[i + 1]); continue; }
            if (gtk_widget_get_visible(GTK_WIDGET(state->window))) {
                hide_widget(state);
            } else {
                gtk_window_present(state->window);
            }
            i++;
        }
    }
    g_strfreev(argv);
    return 0;
}

static void activate_handler(GApplication *app, gpointer user_data) {
    (void)app;
    AuroraShell *shell = (AuroraShell *)user_data;
    if (g_hash_table_size(shell->widgets) == 0) {
        load_all_widgets(shell);
    }
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
    g_hash_table_destroy(shell_data.widgets);
    g_object_unref(shell_data.app);
    return status;
}