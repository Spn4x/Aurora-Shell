#include "workspaces.h"
#include <adwaita.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <math.h>

#define WIDTH_PER_WORKSPACE 45

typedef struct {
    guint64 id;
    int idx;
} NiriWorkspace;

typedef struct {
    GtkWidget *container; 
    GtkWidget *drawing_area;
    GSubprocess *niri_proc;
    GDataInputStream *event_stream;
    GCancellable *cancellable;
    
    GList *workspaces; // List of NiriWorkspace*
    int max_workspace_idx;
    int active_workspace_idx;

    gboolean is_initialized;
    double current_animated_index; 
    guint animation_timer_id;
    
    int current_container_width;
    AdwAnimation *resize_anim;
} WorkspacesModule;

// --- Helper Functions ---

static void free_niri_workspace(gpointer data) {
    g_free(data);
}

static int get_idx_for_id(WorkspacesModule *module, guint64 id) {
    for (GList *l = module->workspaces; l != NULL; l = l->next) {
        NiriWorkspace *ws = l->data;
        if (ws->id == id) return ws->idx;
    }
    return -1;
}

static void on_resize_anim_value_changed(double value, gpointer user_data) {
    WorkspacesModule *module = (WorkspacesModule *)user_data;
    gtk_widget_set_size_request(module->drawing_area, (int)value, 28);
    gtk_widget_queue_draw(module->drawing_area); 
}

static void workspaces_module_cleanup(gpointer data) {
    WorkspacesModule *module = (WorkspacesModule *)data;
    if (module->cancellable) {
        g_cancellable_cancel(module->cancellable); 
    }
    if (module->animation_timer_id > 0) g_source_remove(module->animation_timer_id);
    if (module->resize_anim) {
        adw_animation_pause(module->resize_anim);
        g_object_unref(module->resize_anim);
    }
    if (module->niri_proc) {
        g_subprocess_force_exit(module->niri_proc);
        g_object_unref(module->niri_proc);
    }
    g_list_free_full(module->workspaces, free_niri_workspace);
    module->drawing_area = NULL; 
}

// --- Animation & Drawing ---

static gboolean animation_tick(gpointer user_data) {
    WorkspacesModule *module = user_data;
    if (!module->drawing_area || !GTK_IS_WIDGET(module->drawing_area)) {
        module->animation_timer_id = 0; return G_SOURCE_REMOVE;
    }
    if (module->max_workspace_idx == 0) return G_SOURCE_CONTINUE; 
    
    if (fabs(module->current_animated_index - module->active_workspace_idx) < 0.01) {
        module->current_animated_index = module->active_workspace_idx;
        module->animation_timer_id = 0;
        gtk_widget_queue_draw(module->drawing_area);
        return G_SOURCE_REMOVE;
    }
    
    module->current_animated_index += (module->active_workspace_idx - module->current_animated_index) * 0.2;
    gtk_widget_queue_draw(module->drawing_area);
    return G_SOURCE_CONTINUE;
}

static void start_workspace_animation(WorkspacesModule *module) {
    if (module->animation_timer_id == 0) {
        module->animation_timer_id = g_timeout_add(16, animation_tick, module);
    }
}

static void update_workspace_display(WorkspacesModule *module) {
    if (!module->drawing_area || !GTK_IS_WIDGET(module->drawing_area)) return;
    
    module->max_workspace_idx = 0;
    for (GList *l = module->workspaces; l != NULL; l = l->next) {
        NiriWorkspace *ws = l->data;
        if (ws->idx > module->max_workspace_idx) module->max_workspace_idx = ws->idx;
    }
    if (module->max_workspace_idx < 1) module->max_workspace_idx = 1;

    int target_width = module->max_workspace_idx * WIDTH_PER_WORKSPACE;

    if (module->current_container_width != target_width) {
        if (module->current_container_width == 0) {
            gtk_widget_set_size_request(module->drawing_area, target_width, 28);
        } else {
            if (module->resize_anim) {
                adw_animation_pause(module->resize_anim);
                g_object_unref(module->resize_anim);
                module->resize_anim = NULL;
            }
            
            int actual_w;
            gtk_widget_get_size_request(module->drawing_area, &actual_w, NULL);
            if (actual_w <= 0) actual_w = module->current_container_width;

            AdwAnimationTarget *target = adw_callback_animation_target_new(on_resize_anim_value_changed, module, NULL);
            module->resize_anim = adw_timed_animation_new(module->drawing_area, actual_w, target_width, 300, target);
            adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(module->resize_anim), ADW_EASE_OUT_CUBIC);
            adw_animation_play(module->resize_anim);
        }
        module->current_container_width = target_width;
    }

    gtk_widget_queue_draw(module->drawing_area);
    start_workspace_animation(module);
}

static void cairo_rounded_rectangle(cairo_t *cr, double x, double y, double width, double height, double radius) {
    cairo_new_sub_path(cr); cairo_arc(cr, x + radius, y + radius, radius, G_PI, 1.5 * G_PI); cairo_arc(cr, x + width - radius, y + radius, radius, 1.5 * G_PI, 2.0 * G_PI); cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, 0.5 * G_PI); cairo_arc(cr, x + radius, y + height - radius, radius, 0.5 * G_PI, G_PI); cairo_close_path(cr);
}

static void draw_workspaces(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    WorkspacesModule *module = user_data;
    if (module->max_workspace_idx == 0) return;
    
    if (!module->is_initialized && width > 0) {
        module->current_animated_index = module->active_workspace_idx;
        module->is_initialized = TRUE;
    }
    
    GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(area));
    GdkRGBA inactive_bg, active_bg, active_fg, inactive_fg;
    gtk_style_context_lookup_color(context, "theme_unfocused_color", &inactive_bg);
    gtk_style_context_lookup_color(context, "theme_selected_bg_color", &active_bg);
    gtk_style_context_lookup_color(context, "theme_bg_color", &active_fg);
    gtk_style_context_lookup_color(context, "theme_fg_color", &inactive_fg);
    
    cairo_set_source_rgba(cr, inactive_bg.red, inactive_bg.green, inactive_bg.blue, inactive_bg.alpha);
    cairo_rounded_rectangle(cr, 0, 0, width, height, 8.0); 
    cairo_fill(cr);
    
    double slot_width = WIDTH_PER_WORKSPACE; 
    double active_width = module->current_animated_index * slot_width;
    
    cairo_save(cr); 
    cairo_rounded_rectangle(cr, 0, 0, width, height, 8.0); 
    cairo_clip(cr); 
    cairo_set_source_rgba(cr, active_bg.red, active_bg.green, active_bg.blue, active_bg.alpha); 
    cairo_rectangle(cr, 0, 0, active_width, height); 
    cairo_fill(cr); 
    cairo_restore(cr);
    
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD); 
    cairo_set_font_size(cr, 12.0);
    
    for (gint idx = 1; idx <= module->max_workspace_idx; ++idx) {
        g_autofree gchar *id_str = g_strdup_printf("%d", idx);
        cairo_text_extents_t extents; cairo_text_extents(cr, id_str, &extents);
        
        double x_pos = (idx - 1) * slot_width + (slot_width / 2.0) - (extents.width / 2.0);
        double y_pos = (height / 2.0) + (extents.height / 2.0);
        double text_center_x = (idx - 1) * slot_width + (slot_width / 2.0);
        
        double slot_start_x = (idx - 1) * slot_width;
        double visible_pixels = width - slot_start_x;
        double alpha_multiplier = 1.0;
        
        if (visible_pixels < slot_width) {
            alpha_multiplier = visible_pixels / slot_width;
            if (alpha_multiplier < 0.0) alpha_multiplier = 0.0;
        }

        if (text_center_x <= active_width + (slot_width * 0.1)) {
            cairo_set_source_rgba(cr, active_fg.red, active_fg.green, active_fg.blue, active_fg.alpha * alpha_multiplier);
        } else {
            cairo_set_source_rgba(cr, inactive_fg.red, inactive_fg.green, inactive_fg.blue, inactive_fg.alpha * alpha_multiplier);
        }
        
        cairo_move_to(cr, x_pos, y_pos); cairo_show_text(cr, id_str);
    }
}

// --- Interaction ---

static void on_level_bar_clicked(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; (void)y; WorkspacesModule *module = (WorkspacesModule *)user_data;
    if (module->max_workspace_idx == 0) return;
    
    int clicked_idx = (int)((x / (double)WIDTH_PER_WORKSPACE)) + 1; 
    
    g_autofree gchar *command = g_strdup_printf("niri msg action focus-workspace %d", clicked_idx);
    g_spawn_command_line_async(command, NULL);
}

// --- Niri JSON Parsing ---

static void parse_workspaces_array(WorkspacesModule *module, JsonArray *workspaces_array) {
    gboolean needs_update = FALSE;
    
    g_list_free_full(module->workspaces, free_niri_workspace);
    module->workspaces = NULL;

    for (guint i = 0; i < json_array_get_length(workspaces_array); i++) {
        JsonObject *ws_obj = json_array_get_object_element(workspaces_array, i);
        if (!ws_obj) continue;

        NiriWorkspace *ws = g_new0(NiriWorkspace, 1);
        ws->id = json_object_get_int_member(ws_obj, "id");
        ws->idx = json_object_get_int_member(ws_obj, "idx");
        module->workspaces = g_list_append(module->workspaces, ws);

        if (json_object_has_member(ws_obj, "is_active") && json_object_get_boolean_member(ws_obj, "is_active")) {
            if (module->active_workspace_idx != ws->idx) {
                module->active_workspace_idx = ws->idx;
                needs_update = TRUE;
            }
        } else if (json_object_has_member(ws_obj, "is_focused") && json_object_get_boolean_member(ws_obj, "is_focused")) {
            if (module->active_workspace_idx != ws->idx) {
                module->active_workspace_idx = ws->idx;
                needs_update = TRUE;
            }
        }
    }
    
    if (needs_update || TRUE) {
        update_workspace_display(module);
    }
}

static void on_niri_event(GObject *source, GAsyncResult *res, gpointer user_data) {
    WorkspacesModule *module = (WorkspacesModule *)user_data;
    g_autoptr(GError) error = NULL;
    gsize length;
    
    char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), res, &length, &error);
    
    if (error || !line) {
        if (error) g_warning("Niri workspace stream error: %s", error->message);
        return; 
    }
    
    g_autoptr(JsonParser) parser = json_parser_new();
    if (json_parser_load_from_data(parser, line, length, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *root_obj = json_node_get_object(root);

            if (json_object_has_member(root_obj, "WorkspacesChanged")) {
                JsonObject *wc = json_object_get_object_member(root_obj, "WorkspacesChanged");
                if (json_object_has_member(wc, "workspaces")) {
                    parse_workspaces_array(module, json_object_get_array_member(wc, "workspaces"));
                }
            } 
            else if (json_object_has_member(root_obj, "WorkspaceActivated")) {
                JsonObject *wa = json_object_get_object_member(root_obj, "WorkspaceActivated");
                guint64 id = json_object_get_int_member(wa, "id");
                
                int new_idx = get_idx_for_id(module, id);
                if (new_idx != -1 && module->active_workspace_idx != new_idx) {
                    module->active_workspace_idx = new_idx;
                    start_workspace_animation(module);
                }
            }
        }
    }
    
    g_free(line);
    g_data_input_stream_read_line_async(G_DATA_INPUT_STREAM(source), G_PRIORITY_DEFAULT, module->cancellable, on_niri_event, module);
}

// --- Initialization ---

static gchar* run_command_and_get_output(const char* command) {
    FILE *fp = popen(command, "r"); if (!fp) return NULL;
    gchar buffer[1024]; GString *output_str = g_string_new("");
    while (fgets(buffer, sizeof(buffer), fp) != NULL) g_string_append(output_str, buffer);
    pclose(fp); return g_string_free(output_str, FALSE);
}

static void populate_initial_workspaces(WorkspacesModule *module) {
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *workspaces_json = run_command_and_get_output("niri msg -j workspaces");
    
    if (workspaces_json && json_parser_load_from_data(parser, workspaces_json, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_ARRAY(root)) {
            parse_workspaces_array(module, json_node_get_array(root));
        }
    }
}

static void connect_to_event_stream(WorkspacesModule *module) {
    g_autoptr(GError) error = NULL;
    
    module->niri_proc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error, 
        "niri", "msg", "--json", "event-stream", NULL
    );

    if (!module->niri_proc) {
        g_warning("Failed to start Niri event stream: %s", error->message);
        return;
    }

    GInputStream *stdout_stream = g_subprocess_get_stdout_pipe(module->niri_proc);
    module->event_stream = g_data_input_stream_new(stdout_stream);
    
    g_data_input_stream_read_line_async(module->event_stream, G_PRIORITY_DEFAULT, module->cancellable, on_niri_event, module);
}

GtkWidget* create_workspaces_module() {
    WorkspacesModule *module = g_new0(WorkspacesModule, 1);
    module->cancellable = g_cancellable_new();
    
    module->container = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(module->container, "workspace-module"); 

    module->drawing_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(module->drawing_area), draw_workspaces, module, NULL);
    
    gtk_box_append(GTK_BOX(module->container), module->drawing_area);

    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_level_bar_clicked), module);
    gtk_widget_add_controller(module->drawing_area, GTK_EVENT_CONTROLLER(click));

    g_object_set_data_full(G_OBJECT(module->container), "module-state", module, workspaces_module_cleanup);
    
    populate_initial_workspaces(module);
    connect_to_event_stream(module);
    
    return module->container; 
}