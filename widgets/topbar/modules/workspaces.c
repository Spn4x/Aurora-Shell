#include <gtk/gtk.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <math.h>
#include "workspaces.h"

// INCREASED WIDTH: Changed from ~30 to 45 to make the bar wider
#define WIDTH_PER_WORKSPACE 45

typedef struct {
    GtkWidget *container; 
    GtkWidget *drawing_area;
    GSocketConnection *event_connection;
    GDataInputStream *event_stream;
    GCancellable *cancellable;
    GList *workspace_ids;
    gint max_workspace_id;
    gint active_workspace_id;

    // --- Animation State ---
    gboolean is_initialized;
    double current_animated_width;
    guint animation_timer_id;
} WorkspacesModule;

// --- Forward Declarations ---
static void on_socket_connected(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_hyprland_event(GObject *source, GAsyncResult *res, gpointer user_data);
static void workspaces_module_cleanup(gpointer data);
static void draw_workspaces(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);
static void start_workspace_animation(WorkspacesModule *module);

static gboolean animation_tick(gpointer user_data) {
    WorkspacesModule *module = user_data;
    if (!GTK_IS_WIDGET(module->drawing_area)) {
        module->animation_timer_id = 0;
        return G_SOURCE_REMOVE;
    }

    int widget_width = gtk_widget_get_width(module->drawing_area);
    if (widget_width == 0 || module->max_workspace_id == 0) {
        return G_SOURCE_CONTINUE; 
    }

    // Calculate target width for the active bar
    double target_width = ((double)widget_width / module->max_workspace_id) * module->active_workspace_id;

    if (fabs(module->current_animated_width - target_width) < 0.5) {
        module->current_animated_width = target_width;
        module->animation_timer_id = 0;
        gtk_widget_queue_draw(module->drawing_area);
        return G_SOURCE_REMOVE;
    }

    // Ease towards target
    module->current_animated_width += (target_width - module->current_animated_width) * 0.2;
    gtk_widget_queue_draw(module->drawing_area);

    return G_SOURCE_CONTINUE;
}

static void start_workspace_animation(WorkspacesModule *module) {
    if (module->animation_timer_id == 0) {
        module->animation_timer_id = g_timeout_add(16, animation_tick, module);
    }
}

// Standard rounded rectangle helper
static void cairo_rounded_rectangle(cairo_t *cr, double x, double y, double width, double height, double radius) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, 1.5 * G_PI);
    cairo_arc(cr, x + width - radius, y + radius, radius, 1.5 * G_PI, 2.0 * G_PI);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, 0.5 * G_PI);
    cairo_arc(cr, x + radius, y + height - radius, radius, 0.5 * G_PI, G_PI);
    cairo_close_path(cr);
}

static void draw_workspaces(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    WorkspacesModule *module = user_data;
    if (module->max_workspace_id == 0) return;

    if (!module->is_initialized && width > 0) {
        module->current_animated_width = ((double)width / module->max_workspace_id) * module->active_workspace_id;
        module->is_initialized = TRUE;
    }

    GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(area));
    GdkRGBA inactive_bg, active_bg, active_fg, inactive_fg;
    
    gtk_style_context_lookup_color(context, "theme_unfocused_color", &inactive_bg);
    gtk_style_context_lookup_color(context, "theme_selected_bg_color", &active_bg);
    gtk_style_context_lookup_color(context, "theme_bg_color", &active_fg);
    gtk_style_context_lookup_color(context, "theme_fg_color", &inactive_fg);

    // 1. Draw Inactive Background (The Continuous Pill)
    cairo_set_source_rgba(cr, inactive_bg.red, inactive_bg.green, inactive_bg.blue, inactive_bg.alpha);
    cairo_rounded_rectangle(cr, 0, 0, width, height, 8.0);
    cairo_fill(cr);

    // 2. Draw Active Indicator (Animated Bar sliding over the background)
    double active_width = module->current_animated_width;
    cairo_save(cr);
    cairo_rounded_rectangle(cr, 0, 0, width, height, 8.0);
    cairo_clip(cr);
    cairo_set_source_rgba(cr, active_bg.red, active_bg.green, active_bg.blue, active_bg.alpha);
    cairo_rectangle(cr, 0, 0, active_width, height);
    cairo_fill(cr);
    cairo_restore(cr);
    
    // 3. Draw Numbers
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12.0);
    
    for (gint id = 1; id <= module->max_workspace_id; ++id) {
        g_autofree gchar *id_str = g_strdup_printf("%d", id);
        cairo_text_extents_t extents;
        cairo_text_extents(cr, id_str, &extents);
        
        double slot_width = (double)width / module->max_workspace_id;
        double x_pos = (id - 1) * slot_width + (slot_width / 2.0) - (extents.width / 2.0);
        double y_pos = (height / 2.0) + (extents.height / 2.0);
        
        // Color logic: if the center of the text is covered by the active bar, make it active color
        double text_center_x = (id - 1) * slot_width + (slot_width / 2.0);
        
        if (text_center_x <= active_width) {
            cairo_set_source_rgba(cr, active_fg.red, active_fg.green, active_fg.blue, active_fg.alpha);
        } else {
            cairo_set_source_rgba(cr, inactive_fg.red, inactive_fg.green, inactive_fg.blue, inactive_fg.alpha);
        }
        cairo_move_to(cr, x_pos, y_pos);
        cairo_show_text(cr, id_str);
    }
}

static void update_workspace_display(WorkspacesModule *module) {
    if (!GTK_IS_WIDGET(module->drawing_area)) return;
    
    module->max_workspace_id = 0;
    for (GList *l = module->workspace_ids; l != NULL; l = l->next) {
        gint id = GPOINTER_TO_INT(l->data);
        if (id > module->max_workspace_id) {
            module->max_workspace_id = id;
        }
    }
    if (module->max_workspace_id < 1) module->max_workspace_id = 1;

    // Calculate width based on the LARGER constant
    int new_width = module->max_workspace_id * WIDTH_PER_WORKSPACE;
    gtk_widget_set_size_request(module->drawing_area, new_width, 28);

    gtk_widget_queue_draw(module->drawing_area);
    start_workspace_animation(module);
}

static void on_level_bar_clicked(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; 
    (void)y;
    
    WorkspacesModule *module = (WorkspacesModule *)user_data;
    
    // Safety check: ensure we have workspaces to switch to
    if (module->max_workspace_id == 0) return;

    // Get the widget width to calculate relative click position
    GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    int width = gtk_widget_get_width(widget);
    
    // Prevent division by zero if widget hasn't realized size yet
    if (width <= 0) return;

    // Calculate the clicked ID based on X position relative to total width
    int clicked_id = (int)((x / (double)width) * module->max_workspace_id) + 1;

    // Construct the Hyprland command
    g_autofree gchar *command = g_strdup_printf("hyprctl dispatch workspace %d", clicked_id);

    // FIX: Use async spawn instead of system() to prevent UI freezing
    GError *error = NULL;
    if (!g_spawn_command_line_async(command, &error)) {
        g_warning("Workspaces Module: Failed to switch to workspace %d: %s", clicked_id, error->message);
        g_error_free(error);
    }
}

static void on_hyprland_event(GObject *source, GAsyncResult *res, gpointer user_data) {
    WorkspacesModule *module = (WorkspacesModule *)user_data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), res, NULL, &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return;
    if (error || !line) return;
    
    gboolean needs_update = FALSE;

    if (g_str_has_prefix(line, "workspacev2>>")) {
        gchar **parts = g_strsplit(line, ">>", 2);
        if (parts[1]) {
            gchar **data_parts = g_strsplit(parts[1], ",", 2);
            if (data_parts[0]) {
                gint new_id = atoi(data_parts[0]);
                if (module->active_workspace_id != new_id) {
                    module->active_workspace_id = new_id;
                    start_workspace_animation(module);
                }
            }
            g_strfreev(data_parts);
        }
        g_strfreev(parts);
    } 
    else if (g_str_has_prefix(line, "createworkspacev2>>")) {
        gchar **parts = g_strsplit(line, ">>", 2);
        if (parts[1]) {
            gchar **data_parts = g_strsplit(parts[1], ",", 2);
            if (data_parts[0]) {
                gint id = atoi(data_parts[0]);
                if (!g_list_find(module->workspace_ids, GINT_TO_POINTER(id))) {
                    module->workspace_ids = g_list_append(module->workspace_ids, GINT_TO_POINTER(id));
                    needs_update = TRUE;
                }
            }
            g_strfreev(data_parts);
        }
        g_strfreev(parts);
    }
    else if (g_str_has_prefix(line, "destroyworkspacev2>>")) {
        gchar **parts = g_strsplit(line, ">>", 2);
        if (parts[1]) {
            gchar **data_parts = g_strsplit(parts[1], ",", 2);
            if (data_parts[0]) {
                gint id = atoi(data_parts[0]);
                module->workspace_ids = g_list_remove(module->workspace_ids, GINT_TO_POINTER(id));
                needs_update = TRUE;
            }
            g_strfreev(data_parts);
        }
        g_strfreev(parts);
    }

    if (needs_update) {
        update_workspace_display(module);
    }

    g_data_input_stream_read_line_async(G_DATA_INPUT_STREAM(source), G_PRIORITY_DEFAULT, module->cancellable, on_hyprland_event, module);
}

static void on_socket_connected(GObject *source, GAsyncResult *res, gpointer user_data) {
    WorkspacesModule *module = (WorkspacesModule *)user_data;
    g_autoptr(GError) error = NULL;
    if (g_cancellable_is_cancelled(module->cancellable)) return;
    module->event_connection = g_socket_client_connect_finish(G_SOCKET_CLIENT(source), res, &error);
    if (error) return;
    g_autoptr(GInputStream) istream = g_io_stream_get_input_stream(G_IO_STREAM(module->event_connection));
    module->event_stream = g_data_input_stream_new(istream);
    g_data_input_stream_read_line_async(module->event_stream, G_PRIORITY_DEFAULT, module->cancellable, on_hyprland_event, module);
}

static void connect_to_event_socket(WorkspacesModule *module) {
    const gchar *instance_signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const gchar *xdg_runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!instance_signature || !xdg_runtime_dir) return;
    g_autofree gchar *socket_path = g_build_filename(xdg_runtime_dir, "hypr", instance_signature, ".socket2.sock", NULL);
    g_autoptr(GSocketClient) client = g_socket_client_new();
    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(socket_path);
    g_socket_client_connect_async(client, G_SOCKET_CONNECTABLE(address), module->cancellable, on_socket_connected, module);
}

static gchar* run_command_and_get_output(const char* command) {
    FILE *fp = popen(command, "r");
    if (!fp) return NULL;
    gchar buffer[256]; GString *output_str = g_string_new("");
    while (fgets(buffer, sizeof(buffer), fp) != NULL) g_string_append(output_str, buffer);
    pclose(fp);
    return g_string_free(output_str, FALSE);
}

static void populate_initial_workspaces(WorkspacesModule *module) {
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *workspaces_json = run_command_and_get_output("hyprctl -j workspaces");
    if (workspaces_json && json_parser_load_from_data(parser, workspaces_json, -1, NULL)) {
        JsonArray *workspaces_array = json_node_get_array(json_parser_get_root(parser));
        for (guint i = 0; i < json_array_get_length(workspaces_array); i++) {
            JsonObject *ws = json_array_get_object_element(workspaces_array, i);
            gint id = json_object_get_int_member(ws, "id");
            if (id > 0) module->workspace_ids = g_list_append(module->workspace_ids, GINT_TO_POINTER(id));
        }
    }
    g_autofree gchar *active_json = run_command_and_get_output("hyprctl -j activeworkspace");
    if (active_json && json_parser_load_from_data(parser, active_json, -1, NULL)) {
        JsonObject *active_ws = json_node_get_object(json_parser_get_root(parser));
        module->active_workspace_id = json_object_get_int_member(active_ws, "id");
    }
    update_workspace_display(module);
}

static void workspaces_module_cleanup(gpointer data) {
    WorkspacesModule *module = (WorkspacesModule *)data;
    if (module->cancellable) g_cancellable_cancel(module->cancellable);
    if (module->animation_timer_id > 0) {
        g_source_remove(module->animation_timer_id);
    }
    g_list_free(module->workspace_ids);
    if (module->event_stream) g_object_unref(module->event_stream);
    if (module->event_connection) g_object_unref(module->event_connection);
    if (module->cancellable) g_object_unref(module->cancellable);
    g_free(module);
}

GtkWidget* create_workspaces_module() {
    WorkspacesModule *module = g_new0(WorkspacesModule, 1);
    module->cancellable = g_cancellable_new();
    
    // Create wrapper box (for CSS padding/margins)
    module->container = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(module->container, "workspace-module"); 
    gtk_widget_add_css_class(module->container, "module");

    // Create Drawing Area (for content)
    module->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(module->drawing_area, WIDTH_PER_WORKSPACE, 28);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(module->drawing_area), draw_workspaces, module, NULL);
    
    gtk_widget_add_css_class(module->drawing_area, "workspace-drawing");
    
    gtk_box_append(GTK_BOX(module->container), module->drawing_area);

    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_level_bar_clicked), module);
    gtk_widget_add_controller(module->drawing_area, GTK_EVENT_CONTROLLER(click));

    g_object_set_data_full(G_OBJECT(module->container), "module-state", module, workspaces_module_cleanup);
    
    populate_initial_workspaces(module);
    connect_to_event_socket(module);
    
    return module->container; 
}