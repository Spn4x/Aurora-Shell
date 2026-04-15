// FILE: ./src/wallpaper_chooser.c
#include <gtk/gtk.h>
#include <adwaita.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <math.h> 
#include "globals.h"
#include "wallpaper_chooser.h"
#include "storage.h"
#include "theme_manager.h" 

// --- GLOBALS ---
static GtkWidget *shelf_scroller = NULL;
static GtkWidget *shelf_box = NULL;
static char *pending_preview_path = NULL; 
static guint preview_debounce_id = 0;
static gint64 last_scroll_time = 0;
static int center_retries = 0;

// CACHE SYSTEM
static GHashTable *wallpaper_cache = NULL; 

// --- CONSTANTS ---
#define SHELF_WIDTH 1000          
#define ITEM_WIDTH 200            
#define ITEM_SPACING 20           
#define SCROLL_COOLDOWN_MS 150 
#define SIDE_PADDING 400 

// --- CACHE MANAGEMENT ---

static void clear_wallpaper_cache(void) {
    if (wallpaper_cache) {
        g_hash_table_destroy(wallpaper_cache);
        wallpaper_cache = NULL;
    }
}

static void ensure_cache_exists(void) {
    if (!wallpaper_cache) {
        wallpaper_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    }
}

static void preload_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *c) {
    char *path = (char *)task_data;
    if (g_cancellable_is_cancelled(c)) return;
    GError *err = NULL;
    GdkPixbuf *pix = gdk_pixbuf_new_from_file(path, &err);
    if (err) { g_task_return_error(task, err); return; }
    g_task_return_pointer(task, pix, g_object_unref);
}

static void on_preload_ready(GObject *source, GAsyncResult *res, gpointer user_data) {
    char *path = (char *)user_data; 
    GError *err = NULL;
    GdkPixbuf *pix = g_task_propagate_pointer(G_TASK(res), &err);
    if (pix && !err) {
        if (wallpaper_cache) {
            G_GNUC_BEGIN_IGNORE_DEPRECATIONS
            GdkTexture *texture = gdk_texture_new_for_pixbuf(pix);
            G_GNUC_END_IGNORE_DEPRECATIONS
            if (!g_hash_table_lookup(wallpaper_cache, path)) {
                g_hash_table_insert(wallpaper_cache, g_strdup(path), texture);
            } else {
                g_object_unref(texture);
            }
        }
        g_object_unref(pix);
    }
    if (err) g_error_free(err);
}

static void start_preload(const char *path) {
    ensure_cache_exists();
    if (g_hash_table_contains(wallpaper_cache, path)) return;
    GTask *task = g_task_new(NULL, NULL, on_preload_ready, g_strdup(path));
    g_task_set_task_data(task, g_strdup(path), g_free);
    g_task_set_priority(task, G_PRIORITY_LOW); 
    g_task_run_in_thread(task, preload_thread);
    g_object_unref(task);
}

// --- CONFIG & UTILS ---

static gchar* get_wallpaper_dir_from_config(void) {
    g_autofree gchar *config_path = g_build_filename(g_get_user_config_dir(), "aurora-shell", "config.json", NULL);
    if (!g_file_test(config_path, G_FILE_TEST_IS_REGULAR)) {
        return g_build_filename(g_get_home_dir(), "Pictures", "Wallpapers", NULL);
    }
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_file(parser, config_path, NULL)) return NULL;
    JsonNode *root = json_parser_get_root(parser);
    JsonArray *root_array = json_node_get_array(root);
    for (guint i = 0; i < json_array_get_length(root_array); i++) {
        JsonObject *obj = json_array_get_object_element(root_array, i);
        if (json_object_has_member(obj, "type") && 
            g_strcmp0(json_object_get_string_member(obj, "type"), "themer-config") == 0) {
            const char* dir = json_object_get_string_member_with_default(obj, "wallpaper_dir", NULL);
            if (dir) return g_strdup(g_str_has_prefix(dir, "~/") ? g_build_filename(g_get_home_dir(), &dir[2], NULL) : dir);
        }
    }
    return NULL;
}

static void on_thumb_loaded(GObject *s, GAsyncResult *res, gpointer user_data) {
    GtkPicture *pic = GTK_PICTURE(user_data); 
    GError *err = NULL; 
    GdkPixbuf *pix = g_task_propagate_pointer(G_TASK(res), &err);
    if (!err && pix) { 
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        GdkTexture *texture = gdk_texture_new_for_pixbuf(pix);
        G_GNUC_END_IGNORE_DEPRECATIONS
        gtk_picture_set_paintable(pic, GDK_PAINTABLE(texture));
        g_object_unref(texture); g_object_unref(pix); 
    }
    if (err) g_error_free(err);
}

static void load_thumb_thread(GTask *t, gpointer s, gpointer d, GCancellable *c) {
    char *path = d; 
    GError *err = NULL; 
    if (g_cancellable_is_cancelled(c)) return;
    GdkPixbuf *final = gdk_pixbuf_new_from_file_at_scale(path, 300, 168, TRUE, &err);
    if (err) { g_task_return_error(t, err); return; }
    g_task_return_pointer(t, final, g_object_unref);
}

// --- PREVIEW LOGIC ---

static void perform_preview_now(const char* path) {
    if (!path) return;
    if (pending_preview_path && g_strcmp0(pending_preview_path, path) == 0) return;
    
    if (pending_preview_path) g_free(pending_preview_path);
    pending_preview_path = g_strdup(path);
    
    if (wallpaper_cache) {
        GdkPaintable *cached = g_hash_table_lookup(wallpaper_cache, path);
        if (cached) {
            gtk_picture_set_paintable(GTK_PICTURE(app_state.wallpaper_image), cached);
            return;
        }
    }

    GFile *file = g_file_new_for_path(path);
    gtk_picture_set_file(GTK_PICTURE(app_state.wallpaper_image), file);
    g_object_unref(file);
    start_preload(path);
}

static gboolean on_preview_timer(gpointer user_data) {
    char *path = (char *)user_data;
    perform_preview_now(path);
    preview_debounce_id = 0; 
    return G_SOURCE_REMOVE;
}

static void request_preview(const char* path, gboolean immediate) {
    if (preview_debounce_id > 0) { g_source_remove(preview_debounce_id); preview_debounce_id = 0; }
    if (immediate) { perform_preview_now(path); } 
    else { preview_debounce_id = g_timeout_add_full(G_PRIORITY_DEFAULT, 100, on_preview_timer, g_strdup(path), g_free); }
}

// --- COMMIT SYSTEM ---

static gboolean task_step_3_backend(gpointer user_data) {
    save_layout();
    theme_manager_apply(app_state.current_wallpaper_path, NULL);
    clear_wallpaper_cache();
    return G_SOURCE_REMOVE;
}

static gboolean task_step_2_visuals(gpointer user_data) {
    app_set_wallpaper_mode(FALSE);
    g_timeout_add(600, task_step_3_backend, NULL);
    return G_SOURCE_REMOVE;
}

static void commit_wallpaper(void) {
    if (!pending_preview_path) return;
    if (app_state.current_wallpaper_path) g_free(app_state.current_wallpaper_path);
    app_state.current_wallpaper_path = g_strdup(pending_preview_path);

    if (preview_debounce_id > 0) { 
        g_source_remove(preview_debounce_id); 
        preview_debounce_id = 0; 
    }
    g_timeout_add(50, task_step_2_visuals, NULL);
}

void wallpaper_chooser_cancel(void) {
    if (preview_debounce_id > 0) { g_source_remove(preview_debounce_id); preview_debounce_id = 0; }
    if (app_state.original_wallpaper_path) {
        GFile *file = g_file_new_for_path(app_state.original_wallpaper_path);
        gtk_picture_set_file(GTK_PICTURE(app_state.wallpaper_image), file); g_object_unref(file);
    }
    if (pending_preview_path) { g_free(pending_preview_path); pending_preview_path = NULL; }
    clear_wallpaper_cache();
}

// --- VISUALS ---

static void update_active_item_visuals(void) {
    if (!shelf_box) return;
    
    GtkWidget *focus = gtk_root_get_focus(gtk_widget_get_root(GTK_WIDGET(shelf_box)));
    
    GtkWidget *child = gtk_widget_get_first_child(shelf_box);
    while (child) {
        if (GTK_IS_LABEL(child)) { child = gtk_widget_get_next_sibling(child); continue; }

        if (child == focus || gtk_widget_is_ancestor(focus, child)) {
            // ACTIVE: Normal Opacity, Scaled Up
            gtk_widget_set_opacity(child, 1.0);
            gtk_widget_add_css_class(child, "active-item");
        } else {
            // INACTIVE: Dimmed
            gtk_widget_set_opacity(child, 0.5);
            gtk_widget_remove_css_class(child, "active-item");
        }
        child = gtk_widget_get_next_sibling(child);
    }
}

static void trigger_snap_on_widget(GtkWidget *target_widget) {
    if (!shelf_scroller || !target_widget) return;

    graphene_rect_t bounds;
    // IMPORTANT: Wait for GTK to physically allocate the box.
    if (!gtk_widget_compute_bounds(target_widget, shelf_box, &bounds)) return;
    if (bounds.size.width <= 0) return;

    double item_center_box = bounds.origin.x + (bounds.size.width / 2.0);

    double viewport_w = (double)gtk_widget_get_width(shelf_scroller);
    if (viewport_w < 1) viewport_w = SHELF_WIDTH;

    double target_x = (item_center_box + SIDE_PADDING) - (viewport_w / 2.0);
    
    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(shelf_scroller));
    double max = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj);
    
    if (target_x < 0) target_x = 0;
    if (max > 0 && target_x > max) target_x = max; 

    gtk_adjustment_set_value(adj, target_x);
    update_active_item_visuals();
}

// --- INPUT & NAVIGATION ---

static void focus_and_center(GtkWidget *target) {
    gtk_widget_grab_focus(target);
    trigger_snap_on_widget(target);
    
    const char* path = g_object_get_data(G_OBJECT(target), "wallpaper-path");
    request_preview(path, FALSE);
}

static gboolean on_item_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    GtkWidget *wrapper = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_space || keyval == GDK_KEY_KP_Enter) {
        commit_wallpaper(); 
        return TRUE;
    }
    
    GtkWidget *target = NULL;
    if (keyval == GDK_KEY_Left) target = gtk_widget_get_prev_sibling(wrapper);
    if (keyval == GDK_KEY_Right) target = gtk_widget_get_next_sibling(wrapper);

    if (target) {
        focus_and_center(target);
        return TRUE;
    }
    return FALSE;
}

static void on_item_clicked(GtkGestureClick *gesture, int n, double x, double y, gpointer user_data) {
    GtkWidget *wrapper = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    focus_and_center(wrapper);
    
    const char* path = g_object_get_data(G_OBJECT(wrapper), "wallpaper-path");
    request_preview(path, TRUE);
    commit_wallpaper(); 
}

static gboolean on_scroll_event(GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data) {
    gint64 now = g_get_monotonic_time() / 1000;
    if (now - last_scroll_time < SCROLL_COOLDOWN_MS) return TRUE;
    last_scroll_time = now;
    
    GtkWidget *focus = gtk_root_get_focus(gtk_widget_get_root(GTK_WIDGET(shelf_scroller)));
    if (!focus || !gtk_widget_is_ancestor(focus, shelf_box)) return FALSE;

    GtkWidget *target = NULL;
    if (dy > 0 || dx > 0) target = gtk_widget_get_next_sibling(focus);
    else if (dy < 0 || dx < 0) target = gtk_widget_get_prev_sibling(focus);

    if (target) {
        focus_and_center(target);
        return TRUE;
    }
    return FALSE; 
}

// --- CREATION ---

static GtkWidget* create_preview_item(const char* path_str, int index) {
    GtkWidget *wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(wrapper, ITEM_WIDTH, -1);
    gtk_widget_set_focusable(wrapper, TRUE);
    gtk_widget_add_css_class(wrapper, "wallpaper-card");
    g_object_set_data(G_OBJECT(wrapper), "item-index", GINT_TO_POINTER(index));
    gtk_widget_set_valign(wrapper, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(wrapper, 10); 
    gtk_widget_set_margin_bottom(wrapper, 10);

    GtkWidget *pic = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_size_request(pic, ITEM_WIDTH, (int)(ITEM_WIDTH * 0.5625)); 
    gtk_widget_add_css_class(pic, "wallpaper-img");
    gtk_box_append(GTK_BOX(wrapper), pic);

    GtkWidget *lbl = gtk_label_new(g_path_get_basename(path_str));
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(lbl, "caption");
    gtk_widget_set_margin_top(lbl, 8);
    gtk_box_append(GTK_BOX(wrapper), lbl);

    g_object_set_data_full(G_OBJECT(wrapper), "wallpaper-path", g_strdup(path_str), g_free);
    
    GTask *t = g_task_new(NULL, NULL, on_thumb_loaded, pic);
    g_task_set_task_data(t, g_strdup(path_str), g_free);
    g_task_run_in_thread(t, (GTaskThreadFunc)load_thumb_thread); g_object_unref(t);

    start_preload(path_str);

    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_item_clicked), NULL);
    gtk_widget_add_controller(wrapper, GTK_EVENT_CONTROLLER(click));

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_item_key_pressed), NULL);
    gtk_widget_add_controller(wrapper, key);

    return wrapper;
}

GtkWidget *create_wallpaper_shelf(void) {
    ensure_cache_exists(); 

    GtkWidget *revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), 300);

    GtkWidget *container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(container, SHELF_WIDTH, 260); 
    gtk_widget_set_margin_bottom(container, 60);
    gtk_widget_set_hexpand(container, FALSE); 
    gtk_widget_set_halign(container, GTK_ALIGN_CENTER);

    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, 
        ".wallpaper-card { "
        "   transform: scale(0.9); "
        "   transition: transform 0.2s cubic-bezier(0.25, 0.46, 0.45, 0.94); "
        "}"
        ".active-item { "
        "   transform: scale(1.15); " 
        "   z-index: 100; "           
        "}"
        ".active-item .wallpaper-img { "
        "   border: 3px solid white; "
        "}"
    );
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    shelf_scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(shelf_scroller), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    gtk_widget_set_hexpand(shelf_scroller, TRUE);
    gtk_widget_set_vexpand(shelf_scroller, TRUE);
    
    GtkEventController *scroll_ctrl = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
    g_signal_connect(scroll_ctrl, "scroll", G_CALLBACK(on_scroll_event), NULL);
    gtk_widget_add_controller(shelf_scroller, scroll_ctrl);

    shelf_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, ITEM_SPACING);
    gtk_widget_set_valign(shelf_box, GTK_ALIGN_CENTER); 
    gtk_widget_set_halign(shelf_box, GTK_ALIGN_START);
    gtk_widget_set_margin_start(shelf_box, SIDE_PADDING);
    gtk_widget_set_margin_end(shelf_box, SIDE_PADDING);
    
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(shelf_scroller), shelf_box);
    gtk_box_append(GTK_BOX(container), shelf_scroller);

    int idx = 0;
    g_autofree char* wall_dir_path = get_wallpaper_dir_from_config();
    if (wall_dir_path) {
        GDir *dir = g_dir_open(wall_dir_path, 0, NULL);
        if (dir) {
            const char *filename;
            while ((filename = g_dir_read_name(dir))) {
                if (g_str_has_suffix(filename, ".png") || g_str_has_suffix(filename, ".jpg") || g_str_has_suffix(filename, ".jpeg")) {
                    g_autofree char *full_path = g_build_filename(wall_dir_path, filename, NULL);
                    gtk_box_append(GTK_BOX(shelf_box), create_preview_item(full_path, idx++));
                }
            }
            g_dir_close(dir);
        }
    }

    if (idx == 0) {
        GtkWidget *lbl = gtk_label_new("No Wallpapers Found");
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_box_append(GTK_BOX(shelf_box), lbl);
    }
    
    gtk_revealer_set_child(GTK_REVEALER(revealer), container);
    return revealer;
}

// --- POLLING LOOP (FAST) ---
// Retries every 10ms until GTK has laid out the actual widget
static gboolean initial_center_cb(gpointer user_data) {
    if (!shelf_box || !shelf_scroller) return G_SOURCE_REMOVE;

    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(shelf_scroller));
    double max = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj);
    
    // If max is 0, GTK hasn't laid out the scrolled window yet. Retry shortly.
    if (max <= 0 && center_retries < 20) {
        center_retries++;
        return G_SOURCE_CONTINUE;
    }
    center_retries = 0; 
    
    GtkWidget *target = NULL;
    GtkWidget *child = gtk_widget_get_first_child(shelf_box);
    
    while (child) {
        if (GTK_IS_LABEL(child)) {
            child = gtk_widget_get_next_sibling(child);
            continue;
        }
        
        const char *path = g_object_get_data(G_OBJECT(child), "wallpaper-path");
        if (path && app_state.current_wallpaper_path && g_strcmp0(path, app_state.current_wallpaper_path) == 0) {
            target = child;
            break;
        }
        child = gtk_widget_get_next_sibling(child);
    }
    
    if (!target) target = gtk_widget_get_first_child(shelf_box);
    
    if (target && !GTK_IS_LABEL(target)) {
        gtk_widget_grab_focus(target);
        
        graphene_rect_t bounds;
        if (gtk_widget_compute_bounds(target, shelf_box, &bounds) && bounds.size.width > 0) {
            double item_center_box = bounds.origin.x + (bounds.size.width / 2.0);
            double viewport_w = (double)gtk_widget_get_width(shelf_scroller);
            if (viewport_w < 1) viewport_w = SHELF_WIDTH;

            double target_x = (item_center_box + SIDE_PADDING) - (viewport_w / 2.0);
            if (target_x < 0) target_x = 0;
            if (max > 0 && target_x > max) target_x = max; 
            
            gtk_adjustment_set_value(adj, target_x);
            update_active_item_visuals();
        } else {
            center_retries++;
            return G_SOURCE_CONTINUE;
        }
    }
    
    return G_SOURCE_REMOVE;
}

void wallpaper_chooser_grab_focus(void) {
    center_retries = 0;
    // We poll at 10ms until the layout is ready and bounds are computed successfully
    g_timeout_add(10, initial_center_cb, NULL);
}