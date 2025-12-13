#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <json-glib/json-glib.h>

typedef struct {
    GtkApplication *gtk_app;
    GtkWindow *window;
    GtkBox *hbox;
    GtkScrolledWindow *scrolled_window;
    GList *previews;
    int selected_index;
    GCancellable *cancellable;
} Application;

static void app_update_view(Application *app);
static void app_select_and_quit(GtkWidget *preview_widget, Application *app);
static void on_image_loaded_cb(GObject *source_object, GAsyncResult *res, gpointer user_data);
static void load_image_thread_func(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
static GtkWidget* ui_create_wallpaper_preview(Application *app, const char* path_str, int index);
static void app_populate_from_list(Application *app, GList *paths);
static void free_string_list(gpointer data);

// ===================================================================
//  Global Theme Loading (The Fix)
// ===================================================================
static void load_global_theme(GApplication *app, gpointer user_data) {
    (void)app; (void)user_data;
    g_autofree gchar *colors_path = g_build_filename(g_get_user_config_dir(), "aurora-shell", "aurora-colors.css", NULL);
    
    if (g_file_test(colors_path, G_FILE_TEST_EXISTS)) {
        GtkCssProvider *provider = gtk_css_provider_new();
        gtk_css_provider_load_from_path(provider, colors_path);
        
        // Load with USER priority to define variables globally for this process
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), 
            GTK_STYLE_PROVIDER(provider), 
            GTK_STYLE_PROVIDER_PRIORITY_USER
        );
        g_object_unref(provider);
    }
}

// ===================================================================
//  Core Application Logic
// ===================================================================

static void free_string_list(gpointer data) {
    g_list_free_full((GList *)data, g_free);
}

static void app_select_and_quit(GtkWidget *preview_widget, Application *app) {
    const char* path = g_object_get_data(G_OBJECT(preview_widget), "wallpaper-path");
    if (path) { g_print("%s\n", path); fflush(stdout); }
    g_application_quit(G_APPLICATION(app->gtk_app));
}

static void ui_center_selected_item(Application *app) {
    if (g_list_length(app->previews) == 0) return;
    GtkWidget *selected_widget = g_list_nth_data(app->previews, app->selected_index);
    if (!selected_widget) return;
    
    GtkAdjustment *hadjustment = gtk_scrolled_window_get_hadjustment(app->scrolled_window);
    
    double item_x, item_y; 
    gtk_widget_translate_coordinates(selected_widget, GTK_WIDGET(app->hbox), 0, 0, &item_x, &item_y);

    double item_width = gtk_widget_get_width(selected_widget);
    double viewport_width = gtk_adjustment_get_page_size(hadjustment);
    double new_scroll_value = item_x + item_width / 2.0 - viewport_width / 2.0;

    double lower = gtk_adjustment_get_lower(hadjustment);
    double upper = gtk_adjustment_get_upper(hadjustment) - viewport_width;
    if (upper < lower) upper = lower;
    
    if (new_scroll_value < lower) new_scroll_value = lower;
    if (new_scroll_value > upper) new_scroll_value = upper;
    
    gtk_adjustment_set_value(hadjustment, new_scroll_value);
}

static void app_update_view(Application *app) {
    if (g_list_length(app->previews) == 0) return;
    for (GList *l = app->previews; l != NULL; l = g_list_next(l)) {
        GtkWidget *widget = l->data;
        int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "widget-index"));
        if (index == app->selected_index) gtk_widget_add_css_class(widget, "selected");
        else gtk_widget_remove_css_class(widget, "selected");
    }
    GtkWidget *selected_widget = g_list_nth_data(app->previews, app->selected_index);
    if (selected_widget) gtk_widget_grab_focus(selected_widget);
    ui_center_selected_item(app);
}

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval, guint k, GdkModifierType s, gpointer user_data) {
    (void)c; (void)k; (void)s; Application *app = user_data; int count = g_list_length(app->previews);
    if (count == 0) return GDK_EVENT_PROPAGATE;
    switch (keyval) {
        case GDK_KEY_Left: case GDK_KEY_h: app->selected_index = (app->selected_index - 1 + count) % count; app_update_view(app); return GDK_EVENT_STOP;
        case GDK_KEY_Right: case GDK_KEY_l: app->selected_index = (app->selected_index + 1) % count; app_update_view(app); return GDK_EVENT_STOP;
        case GDK_KEY_Return: case GDK_KEY_KP_Enter: { GtkWidget *sel = g_list_nth_data(app->previews, app->selected_index); if (sel) app_select_and_quit(sel, app); return GDK_EVENT_STOP; }
        case GDK_KEY_Escape: case GDK_KEY_q: g_application_quit(G_APPLICATION(app->gtk_app)); return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

static void on_item_clicked(GtkGestureClick *gesture, int n, double x, double y, gpointer user_data) {
    (void)n; (void)x; (void)y; Application *app = user_data;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    app_select_and_quit(w, app);
}

static void on_image_loaded_cb(GObject *s, GAsyncResult *res, gpointer user_data) {
    (void)s; 
    GtkPicture *pic = GTK_PICTURE(user_data); 
    GError *err = NULL; 
    GdkPixbuf *pix = g_task_propagate_pointer(G_TASK(res), &err);
    if(err) { 
        if(!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) g_warning("Async fail: %s", err->message);
        g_error_free(err); 
        return; 
    }
    if(pix) { 
        gtk_picture_set_pixbuf(pic, pix); 
        g_object_unref(pix); 
    }
}

static void load_image_thread_func(GTask *t, gpointer s, gpointer d, GCancellable *c) {
    (void)s; char *p=d; GError *err=NULL; if(g_cancellable_is_cancelled(c)) { g_task_return_error(t, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "X")); return; }
    GdkPixbuf *final=NULL; gboolean is_u=FALSE; char *mime = g_content_type_guess(p, NULL, 0, &is_u);
    if(mime && g_content_type_equals(mime,"image/gif")){ GdkPixbufAnimation *a = gdk_pixbuf_animation_new_from_file(p, &err); if(a) { GdkPixbuf *f = gdk_pixbuf_animation_get_static_image(a); if(f) final = gdk_pixbuf_scale_simple(f, 220, 124, GDK_INTERP_BILINEAR); g_object_unref(a);}} g_free(mime);
    if(final==NULL){ g_clear_error(&err); final=gdk_pixbuf_new_from_file_at_scale(p,220,124,TRUE,&err);}
    if(err){ g_task_return_error(t,g_error_copy(err)); g_error_free(err); return; }
    g_task_return_pointer(t,final,g_object_unref);
}

// ===================================================================
//  UI Construction
// ===================================================================
static GtkWidget* ui_create_wallpaper_preview(Application *app, const char* path_str, int index) {
    GtkWidget *pic = gtk_picture_new();
    gtk_widget_add_css_class(pic, "preview-image");
    gtk_picture_set_can_shrink(GTK_PICTURE(pic), FALSE);
    gtk_picture_set_keep_aspect_ratio(GTK_PICTURE(pic), TRUE);
    gtk_widget_set_vexpand(pic, TRUE);
    gtk_widget_set_valign(pic, GTK_ALIGN_FILL);

    GTask *t = g_task_new(NULL, app->cancellable, on_image_loaded_cb, pic);
    g_task_set_task_data(t, g_strdup(path_str), g_free);
    g_task_run_in_thread(t, (GTaskThreadFunc)load_image_thread_func);
    g_object_unref(t);

    GtkWidget *lbl = gtk_label_new(g_path_get_basename(path_str));
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(lbl, "filename-label");

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(vbox), pic);
    gtk_box_append(GTK_BOX(vbox), lbl);
    
    GtkWidget *container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(container, 220, -1);
    gtk_box_append(GTK_BOX(container), vbox);
    gtk_widget_set_can_focus(container, TRUE);
    gtk_widget_add_css_class(container, "preview-item");

    g_object_set_data_full(G_OBJECT(container), "wallpaper-path", g_strdup(path_str), g_free);
    g_object_set_data(G_OBJECT(container), "widget-index", GINT_TO_POINTER(index));

    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_item_clicked), app);
    gtk_widget_add_controller(container, GTK_EVENT_CONTROLLER(click));

    return container;
}

static void app_populate_from_list(Application *app, GList *paths) {
    int index = 0;
    for (GList *l = paths; l != NULL; l = g_list_next(l)) {
        const char *path = l->data;
        GtkWidget *preview = ui_create_wallpaper_preview(app, path, index++);
        gtk_box_append(GTK_BOX(app->hbox), preview);
        app->previews = g_list_append(app->previews, preview);
    }
}

// ===================================================================
//  Application Activation and Main Loop
// ===================================================================
static void activate(GtkApplication *gtk_app, JsonObject *config_obj) {
    Application *app = g_object_get_data(G_OBJECT(gtk_app), "app-data");
    GList *paths = g_object_get_data(G_OBJECT(gtk_app), "paths-data");
    const char *config_name = g_object_get_data(G_OBJECT(gtk_app), "config-name");

    // Load widget specific CSS (which will use the global variables loaded in startup)
    const char *css_filename = json_object_get_string_member_with_default(config_obj, "stylesheet", NULL);
    g_autofree gchar *css_path = NULL;
    if (css_filename) {
        css_path = g_build_filename(g_get_user_config_dir(), "aurora-shell", "templates", config_name, css_filename, NULL);
    }

    JsonObject *size_obj = json_object_has_member(config_obj, "size") ? json_object_get_object_member(config_obj, "size") : NULL;
    int width = size_obj ? json_object_get_int_member_with_default(size_obj, "width", 800) : 800;
    int height = size_obj ? json_object_get_int_member_with_default(size_obj, "height", 210) : 210;

    JsonObject *margins_obj = json_object_has_member(config_obj, "margins") ? json_object_get_object_member(config_obj, "margins") : NULL;
    int top_margin = margins_obj ? json_object_get_int_member_with_default(margins_obj, "top", 50) : 50;

    app->window = GTK_WINDOW(gtk_application_window_new(gtk_app));
    gtk_widget_set_name(GTK_WIDGET(app->window), "main-window");
    gtk_layer_init_for_window(app->window);
    gtk_layer_set_layer(app->window, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_keyboard_mode(app->window, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    gtk_layer_set_anchor(app->window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_margin(app->window, GTK_LAYER_SHELL_EDGE_TOP, top_margin);

    app->scrolled_window = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_policy(app->scrolled_window, GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_widget_set_size_request(GTK_WIDGET(app->scrolled_window), width, height);
    gtk_window_set_child(app->window, GTK_WIDGET(app->scrolled_window));

    app->hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20));
    gtk_widget_set_name(GTK_WIDGET(app->hbox), "main-hbox");
    gtk_widget_set_halign(GTK_WIDGET(app->hbox), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(app->hbox), GTK_ALIGN_CENTER);
    gtk_scrolled_window_set_child(app->scrolled_window, GTK_WIDGET(app->hbox));

    // Load the specific widget styling
    GtkCssProvider *provider = gtk_css_provider_new();
    if (css_path && g_file_test(css_path, G_FILE_TEST_IS_REGULAR)) {
        gtk_css_provider_load_from_path(provider, css_path);
    } else {
        if (css_path) g_warning("Failed to find CSS file at '%s'. Using fallback.", css_path);
        else g_warning("No CSS path specified. Using fallback.");
        gtk_css_provider_load_from_string(provider, "#main-window{background-color:rgba(30,30,46,0.85);}");
    }
    // Application Priority allows overriding global defaults if needed, but uses them if not
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    GtkEventController *key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_pressed), app);
    gtk_widget_add_controller(GTK_WIDGET(app->window), key_controller);
    
    app_populate_from_list(app, paths);

    if (g_list_length(app->previews) > 0) {
        gtk_window_present(app->window);
        app->selected_index = 0;
        app_update_view(app);
    } else {
        g_warning("No valid image paths provided via stdin. Exiting.");
        g_application_quit(G_APPLICATION(app->gtk_app));
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) { g_printerr("Usage: %s <config_name>\n", argv[0]); return 1; }
    const char *config_name = argv[1];
    
    GList *paths = NULL;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), stdin)) {
        buffer[strcspn(buffer, "\n")] = 0;
        if (buffer[0] != '\0' && g_file_test(buffer, G_FILE_TEST_EXISTS)) {
            paths = g_list_append(paths, g_strdup(buffer));
        } else {
            if (buffer[0] != '\0') g_warning("Skipping invalid path: %s", buffer);
        }
    }

    Application *app = g_new0(Application, 1);
    app->selected_index = -1;
    app->cancellable = g_cancellable_new();
    app->gtk_app = gtk_application_new(NULL, G_APPLICATION_DEFAULT_FLAGS); 
    
    // THE FIX: Hook up the global theme loader here
    g_signal_connect(app->gtk_app, "startup", G_CALLBACK(load_global_theme), NULL);

    g_object_set_data(G_OBJECT(app->gtk_app), "app-data", app);
    g_object_set_data_full(G_OBJECT(app->gtk_app), "paths-data", paths, free_string_list);
    g_object_set_data(G_OBJECT(app->gtk_app), "config-name", (gpointer)config_name);

    g_autofree gchar *main_config_path = g_build_filename("/usr/local/share/aurora-shell", "config.json", NULL);
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *config_obj = NULL;
    
    if (json_parser_load_from_file(parser, main_config_path, NULL)) {
        JsonArray *root_array = json_node_get_array(json_parser_get_root(parser));
        for (guint i = 0; i < json_array_get_length(root_array); i++) {
            JsonNode *element_node = json_array_get_element(root_array, i);
            if (!element_node || json_node_get_node_type(element_node) != JSON_NODE_OBJECT) {
                continue; 
            }
            JsonObject *widget_obj = json_node_get_object(element_node);
            if (!json_object_has_member(widget_obj, "name")) continue;

            const char *name = json_object_get_string_member(widget_obj, "name");
            if (name && g_strcmp0(name, config_name) == 0) {
                if (json_object_has_member(widget_obj, "config")) {
                    config_obj = json_object_get_object_member(widget_obj, "config");
                }
                break;
            }
        }
    }

    if (!config_obj) { 
        g_printerr("Error: Could not find config block named '%s' or its 'config' object in %s.\n", config_name, main_config_path); 
        return 1; 
    }
    
    g_signal_connect(app->gtk_app, "activate", G_CALLBACK(activate), config_obj);
    int status = g_application_run(G_APPLICATION(app->gtk_app), 0, NULL);
    
    g_cancellable_cancel(app->cancellable); 
    g_list_free(app->previews); 
    g_object_unref(app->cancellable);
    g_object_unref(app->gtk_app); 
    g_free(app);
    return status;
}