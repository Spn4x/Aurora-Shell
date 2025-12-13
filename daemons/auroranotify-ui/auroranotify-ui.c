#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <gio/gio.h>
#include "island_widget.h"
#include <glib/gstdio.h>

const guint PILL_STATE_DURATION_MS = 4000;
const guint EXPANDED_STATE_DURATION_S = 8;
const guint ANIMATION_DURATION = 400;
const guint ANIMATION_FINISH_DELAY_MS = ANIMATION_DURATION + 100;

const char* UI_BUS_NAME = "com.meismeric.auranotify.UI";
const char* UI_OBJECT_PATH = "/com/meismeric/auranotify/UI";

typedef struct {
    gchar *icon;
    gchar *summary;
    gchar *body;
} NotificationData;

typedef struct {
    gchar *id;
    gchar *text;
} PersistentStatus;

typedef struct {
    GtkCssProvider *provider;
    gchar *path;
} CssReloadData;

// --- NEW STRUCT FOR GLOBAL THEME MONITORING ---
typedef struct {
    GtkCssProvider *provider;
    char *path;
} GlobalThemeData;

static GtkApplication *app = NULL;
static GtkWindow *main_window = NULL;
static IslandWidget *island = NULL;
static GQueue notification_queue = G_QUEUE_INIT;
static GHashTable *persistent_statuses = NULL;
static gboolean is_busy = FALSE;
static guint current_timeout_id = 0;
static gboolean is_expanded = FALSE;
static gboolean is_transitioning = FALSE;
static NotificationData *current_notification_data = NULL;

static void create_main_window();
static gboolean dismiss_or_transition(gpointer user_data);
static void update_island_to_persistent_state();
static void show_next_notification();
static gboolean process_initial_notification(gpointer user_data);
static gboolean add_dot_class_cb(gpointer user_data);
static gboolean add_pill_class_cb(gpointer user_data);
static gboolean remove_dot_class_cb(gpointer user_data);
static gboolean populate_expanded_content_cb(gpointer user_data);
static void on_island_enter(GtkEventControllerMotion *controller, double x, double y, gpointer user_data);
static void on_island_leave(GtkEventControllerMotion *controller, gpointer user_data);
static gboolean outro_finished_callback(gpointer user_data);


static void free_notification_data(gpointer data) {
    if (!data) return;
    NotificationData *notif = (NotificationData *)data;
    g_free(notif->icon);
    g_free(notif->summary);
    g_free(notif->body);
    g_free(notif);
}

static void free_persistent_status(gpointer data) {
    PersistentStatus *status = data;
    g_free(status->id);
    g_free(status->text);
    g_free(status);
}

static void update_island_to_persistent_state() {
    if (g_hash_table_size(persistent_statuses) > 0) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, persistent_statuses);
        g_hash_table_iter_next(&iter, &key, &value);
        PersistentStatus *status = value;
        
        GtkWidget *pill_content = gtk_label_new(status->text);
        gtk_widget_add_css_class(pill_content, "summary");
        island_widget_transition_to_pill_child(island, pill_content);
        
        is_busy = TRUE;
        if(main_window) gtk_widget_set_visible(GTK_WIDGET(main_window), TRUE);
        gtk_widget_add_css_class(GTK_WIDGET(island), "pill");
        gtk_widget_add_css_class(GTK_WIDGET(island), "dot");

    } else {
        if (island) {
            gtk_widget_remove_css_class(GTK_WIDGET(island), "pill");
            gtk_widget_remove_css_class(GTK_WIDGET(island), "dot");
        }
        is_busy = FALSE;
        if (main_window) gtk_widget_set_visible(GTK_WIDGET(main_window), FALSE);
    }
}

static GtkWidget* create_expanded_content_widget(NotificationData *data) {
    GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(content_box, GTK_ALIGN_START);
    
    GtkWidget *summary_label = gtk_label_new(data->summary);
    gtk_widget_set_halign(summary_label, GTK_ALIGN_START);
    gtk_widget_add_css_class(summary_label, "summary");
    gtk_label_set_wrap(GTK_LABEL(summary_label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(summary_label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(summary_label), 45);

    GtkWidget *body_label = gtk_label_new(data->body);
    gtk_widget_set_halign(body_label, GTK_ALIGN_START);
    gtk_widget_add_css_class(body_label, "body");
    gtk_label_set_wrap(GTK_LABEL(body_label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(body_label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(body_label), 45);

    gtk_box_append(GTK_BOX(content_box), summary_label);
    gtk_box_append(GTK_BOX(content_box), body_label);

    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scrolled_window, "expanded-scrolled-window");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled_window), FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), content_box);
    
    return scrolled_window;
}

static gboolean unlock_transition_callback(gpointer user_data G_GNUC_UNUSED) {
    is_transitioning = FALSE;
    return G_SOURCE_REMOVE;
}

static void show_next_notification() {
    is_transitioning = TRUE;

    if (current_notification_data) {
        free_notification_data(current_notification_data);
        current_notification_data = NULL;
    }
    
    current_notification_data = g_queue_pop_head(&notification_queue);
    if (!current_notification_data) {
        is_transitioning = FALSE;
        dismiss_or_transition(NULL);
        return;
    }
    
    g_print("UI: Processing notification: %s\n", current_notification_data->summary);

    GtkWidget *pill_summary = gtk_label_new(current_notification_data->summary);
    gtk_widget_add_css_class(pill_summary, "summary");
    gtk_label_set_ellipsize(GTK_LABEL(pill_summary), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(pill_summary), 25);
    island_widget_transition_to_pill_child(island, pill_summary);
    
    if (is_expanded) {
        GtkWidget *expanded_widget = create_expanded_content_widget(current_notification_data);
        island_widget_transition_to_expanded_child(island, expanded_widget);
    } else {
        GtkWidget *placeholder = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        island_widget_transition_to_expanded_child(island, placeholder);
    }
    
    g_timeout_add(ANIMATION_FINISH_DELAY_MS, unlock_transition_callback, NULL);
    
    current_timeout_id = g_timeout_add(is_expanded ? (EXPANDED_STATE_DURATION_S * 1000) : PILL_STATE_DURATION_MS, dismiss_or_transition, NULL);
}

static gboolean outro_finished_callback(gpointer user_data G_GNUC_UNUSED) {
    if (!g_queue_is_empty(&notification_queue)) {
        g_print("UI: Outro complete, but new items are queued. Restarting cycle.\n");
        g_timeout_add(50, add_dot_class_cb, island);
        g_timeout_add(ANIMATION_FINISH_DELAY_MS, process_initial_notification, NULL);
    } else {
        g_print("UI: Outro complete. Reverting to persistent state check.\n");
        update_island_to_persistent_state();
    }
    return G_SOURCE_REMOVE;
}

static gboolean dismiss_or_transition(gpointer user_data G_GNUC_UNUSED) {
    if (is_transitioning) return G_SOURCE_REMOVE;
    is_transitioning = TRUE;

    if (current_timeout_id > 0) {
        g_source_remove(current_timeout_id);
        current_timeout_id = 0;
    }

    if (!g_queue_is_empty(&notification_queue)) {
        g_print("UI: Queue has items. Transitioning content.\n");
        show_next_notification();
    } 
    else {
        g_print("UI: Queue is empty. Starting dot->nothing outro.\n");

        if (current_notification_data) {
            free_notification_data(current_notification_data);
            current_notification_data = NULL;
        }
        
        if (is_expanded) {
            is_expanded = FALSE;
            island_widget_set_expanded(island, FALSE);
        }
        
        if (island) {
            gtk_widget_remove_css_class(GTK_WIDGET(island), "pill");
        }
        
        g_timeout_add(250, remove_dot_class_cb, island);
        g_timeout_add(ANIMATION_FINISH_DELAY_MS, outro_finished_callback, NULL);
    }
    return G_SOURCE_REMOVE;
}

static gboolean populate_expanded_content_cb(gpointer user_data G_GNUC_UNUSED) {
    if (current_notification_data) {
        GtkWidget *expanded_widget = create_expanded_content_widget(current_notification_data);
        island_widget_transition_to_expanded_child(island, expanded_widget);
    }
    return G_SOURCE_REMOVE;
}

static void on_island_clicked(GtkGestureClick *g G_GNUC_UNUSED, int n G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, gpointer u G_GNUC_UNUSED) {
    if (is_transitioning) return;

    if (!is_expanded) {
        if (!current_notification_data) return;
        is_transitioning = TRUE;
        if (current_timeout_id > 0) { g_source_remove(current_timeout_id); current_timeout_id = 0; }
        
        is_expanded = TRUE;
        island_widget_set_expanded(island, TRUE);
        
        g_timeout_add(50, populate_expanded_content_cb, NULL);

        g_print("UI: Clicked to expand.\n");
        
        g_timeout_add(ANIMATION_FINISH_DELAY_MS, unlock_transition_callback, NULL);
        current_timeout_id = g_timeout_add_seconds(EXPANDED_STATE_DURATION_S, dismiss_or_transition, NULL);
    } 
    else {
        dismiss_or_transition(NULL);
    }
}

static void on_island_right_clicked(GtkGestureClick *g G_GNUC_UNUSED, int n G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, gpointer u G_GNUC_UNUSED) {
    if (is_transitioning) return;
    
    if (current_notification_data || is_busy) {
        g_print("UI: Right-click detected. Dismissing notification immediately.\n");
        dismiss_or_transition(NULL);
    }
}

static void on_island_enter(GtkEventControllerMotion *controller G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED) {
    if (is_expanded && current_timeout_id > 0) {
        g_print("UI: Pointer entered, pausing dismissal timer.\n");
        g_source_remove(current_timeout_id);
        current_timeout_id = 0;
    }
}

static void on_island_leave(GtkEventControllerMotion *controller G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED) {
    if (is_expanded && current_timeout_id == 0) {
        g_print("UI: Pointer left, restarting dismissal timer.\n");
        current_timeout_id = g_timeout_add_seconds(EXPANDED_STATE_DURATION_S, dismiss_or_transition, NULL);
    }
}

static gboolean process_initial_notification(gpointer user_data G_GNUC_UNUSED) {
    show_next_notification();
    return G_SOURCE_REMOVE;
}

static gboolean add_pill_class_cb(gpointer user_data) {
    gtk_widget_add_css_class(GTK_WIDGET(user_data), "pill");
    return G_SOURCE_REMOVE;
}

static gboolean add_dot_class_cb(gpointer user_data) {
    gtk_widget_add_css_class(GTK_WIDGET(user_data), "dot");
    g_timeout_add(100, add_pill_class_cb, user_data);
    return G_SOURCE_REMOVE;
}

static gboolean remove_dot_class_cb(gpointer user_data) {
    gtk_widget_remove_css_class(GTK_WIDGET(user_data), "dot");
    return G_SOURCE_REMOVE;
}

static void handle_show_notification(GDBusMethodInvocation *inv, NotificationData *data) {
    g_print("UI: Received and queued: %s\n", data->summary);
    g_queue_push_tail(&notification_queue, data);
    
    if (!is_busy) {
        is_busy = TRUE;
        g_print("UI: UI is idle. Starting display cycle.\n");
        
        if (main_window == NULL) {
            create_main_window();
        }

        gtk_widget_set_visible(GTK_WIDGET(main_window), TRUE);
        g_timeout_add(50, add_dot_class_cb, island);
        g_timeout_add(ANIMATION_FINISH_DELAY_MS, process_initial_notification, NULL);
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}

static void handle_set_persistent_status(GDBusMethodInvocation *inv, gchar *id, gboolean active, gchar *text) {
    if (active) {
        PersistentStatus *status = g_new0(PersistentStatus, 1);
        status->id = g_strdup(id);
        status->text = g_strdup(text);
        g_hash_table_replace(persistent_statuses, g_strdup(id), status);
        g_print("UI: Added/Updated persistent status '%s'\n", id);
    } else {
        g_hash_table_remove(persistent_statuses, id);
        g_print("UI: Removed persistent status '%s'\n", id);
    }

    if (!is_busy && g_queue_is_empty(&notification_queue)) {
        update_island_to_persistent_state();
    }
    
    g_dbus_method_invocation_return_value(inv, NULL);
}

static void dbus_method_dispatcher(GDBusConnection *c, const gchar *s, const gchar *o, const gchar *i, const gchar *m, GVariant *p, GDBusMethodInvocation *inv, gpointer ud) {
    (void)c; (void)s; (void)o; (void)i; (void)ud;
    if (g_strcmp0(m, "ShowNotification") == 0) {
        NotificationData *data = g_new0(NotificationData, 1);
        g_variant_get(p, "(&s&s&s)", &data->icon, &data->summary, &data->body);
        data->icon = g_strdup(data->icon);
        data->summary = g_strdup(data->summary);
        data->body = g_strdup(data->body);
        handle_show_notification(inv, data);
    } else if (g_strcmp0(m, "SetPersistentStatus") == 0) {
        gchar *id, *text;
        gboolean active;
        g_variant_get(p, "(&s&b&s)", &id, &active, &text);
        handle_set_persistent_status(inv, g_strdup(id), active, g_strdup(text));
    } else {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method '%s'", m);
    }
}

static void on_window_destroyed(GtkWidget *w G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
    g_print("UI: Window destroyed.\n");
    g_queue_clear_full(&notification_queue, free_notification_data);
    if (current_notification_data) {
        free_notification_data(current_notification_data);
        current_notification_data = NULL;
    }
    if (current_timeout_id > 0) g_source_remove(current_timeout_id);
    main_window = NULL;
    island = NULL;
}

void create_main_window() {
    main_window = GTK_WINDOW(gtk_application_window_new(app));
    g_signal_connect(main_window, "destroy", G_CALLBACK(on_window_destroyed), NULL);
    
    GtkWidget *wrapper_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    
    island = ISLAND_WIDGET(island_widget_new());

    gtk_box_append(GTK_BOX(wrapper_box), GTK_WIDGET(island));
    
    gtk_window_set_child(main_window, wrapper_box);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_island_clicked), NULL);
    gtk_widget_add_controller(GTK_WIDGET(island), GTK_EVENT_CONTROLLER(click));

    GtkGesture *right_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
    g_signal_connect(right_click, "pressed", G_CALLBACK(on_island_right_clicked), NULL);
    gtk_widget_add_controller(GTK_WIDGET(island), GTK_EVENT_CONTROLLER(right_click));

    GtkEventController *hover_controller = gtk_event_controller_motion_new();
    g_signal_connect(hover_controller, "enter", G_CALLBACK(on_island_enter), NULL);
    g_signal_connect(hover_controller, "leave", G_CALLBACK(on_island_leave), NULL);
    gtk_widget_add_controller(GTK_WIDGET(island), hover_controller);
    
    gtk_layer_init_for_window(main_window);
    gtk_layer_set_layer(main_window, GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_anchor(main_window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_margin(main_window, GTK_LAYER_SHELL_EDGE_TOP, 10);
}

static void on_bus_acquired(GDBusConnection *c, const gchar *n G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
    const gchar* xml = 
        "<node>"
        "  <interface name='com.meismeric.auranotify.UI'>"
        "    <method name='ShowNotification'>"
        "      <arg type='s' name='icon' direction='in'/>"
        "      <arg type='s' name='summary' direction='in'/>"
        "      <arg type='s' name='body' direction='in'/>"
        "    </method>"
        "    <method name='SetPersistentStatus'>"
        "      <arg type='s' name='id' direction='in'/>"
        "      <arg type='b' name='active' direction='in'/>"
        "      <arg type='s' name='text' direction='in'/>"
        "    </method>"
        "  </interface>"
        "</node>";
    
    static const GDBusInterfaceVTable vtable = { .method_call = dbus_method_dispatcher };

    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, NULL);
    guint registration_id = g_dbus_connection_register_object(c, UI_OBJECT_PATH, node->interfaces[0], &vtable, NULL, NULL, NULL);
    g_dbus_node_info_unref(node);
    
    if (registration_id > 0)
        g_print("UI: Headless service is running with persistent status support.\n");
    else
        g_warning("UI: Failed to register D-Bus object.\n");
}

static void on_name_acquired(GDBusConnection *c G_GNUC_UNUSED, const gchar *n G_GNUC_UNUSED, gpointer ud) { g_application_hold(G_APPLICATION(ud)); }
static void on_name_lost(GDBusConnection *c G_GNUC_UNUSED, const gchar *n G_GNUC_UNUSED, gpointer ud) { g_application_release(G_APPLICATION(ud)); }
static void activate(GtkApplication *a G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {}

static void on_stylesheet_changed(GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event_type, gpointer user_data) {
    (void)monitor; (void)file; (void)other_file;
    if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT) {
        CssReloadData *data = user_data;
        g_print("UI CSS Hot-Reload: File '%s' changed, reloading styles.\n", data->path);
        gtk_css_provider_load_from_path(data->provider, data->path);
    }
}

static void free_css_reload_data(gpointer data) {
    CssReloadData *reload_data = data;
    g_free(reload_data->path);
    g_free(reload_data);
}

// ===================================================================
//  Global Theme Loading & Monitoring (The Fix)
// ===================================================================

static void on_global_theme_changed(GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event_type, gpointer user_data) {
    (void)monitor; (void)file; (void)other_file;
    if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT) {
        GlobalThemeData *data = (GlobalThemeData *)user_data;
        g_print("Daemon Global theme colors changed. Reloading from: %s\n", data->path);
        gtk_css_provider_load_from_path(data->provider, data->path);
    }
}

static void load_global_theme_colors(GApplication *app_instance) {
    g_autofree gchar *colors_path = g_build_filename(g_get_user_config_dir(), "aurora-shell", "aurora-colors.css", NULL);
    
    if (g_file_test(colors_path, G_FILE_TEST_EXISTS)) {
        GtkCssProvider *provider = gtk_css_provider_new();
        gtk_css_provider_load_from_path(provider, colors_path);
        
        // Add with USER priority
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), 
            GTK_STYLE_PROVIDER(provider), 
            GTK_STYLE_PROVIDER_PRIORITY_USER
        );

        // SETUP MONITORING
        GFile *file = g_file_new_for_path(colors_path);
        GFileMonitor *monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, NULL);
        if (monitor) {
            GlobalThemeData *data = g_new(GlobalThemeData, 1);
            data->provider = provider; // keep ref
            data->path = g_strdup(colors_path);
            
            g_signal_connect(monitor, "changed", G_CALLBACK(on_global_theme_changed), data);
            
            // Attach monitor to the app to keep it alive
            g_object_set_data_full(G_OBJECT(app_instance), "global-theme-monitor", monitor, g_object_unref);
        }
        g_object_unref(file);
    }
}

static void on_app_startup(GApplication *a, gpointer ud G_GNUC_UNUSED) {
    // --- CALL IT HERE, FIRST THING ---
    load_global_theme_colors(a);

    persistent_statuses = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_persistent_status);
    
    GtkCssProvider *p = gtk_css_provider_new();
    gchar *loaded_css_path = NULL;
    
    g_autofree gchar *user_css_path = g_build_filename(g_get_user_config_dir(), "aurora-shell", "templates", "organizer", "organizer.css", NULL);

    if (g_file_test(user_css_path, G_FILE_TEST_EXISTS)) {
        gtk_css_provider_load_from_path(p, user_css_path);
        loaded_css_path = g_strdup(user_css_path);
    } else {
        g_autofree gchar *system_css_path = NULL;
        #ifdef DATADIR
            system_css_path = g_build_filename(DATADIR, "templates", "organizer", "organizer.css", NULL);
        #else
            system_css_path = g_build_filename("/usr/local/share/aurora-shell/templates/organizer", "organizer.css", NULL);
        #endif
        
        if(g_file_test(system_css_path, G_FILE_TEST_EXISTS)) {
            gtk_css_provider_load_from_path(p, system_css_path);
            loaded_css_path = g_strdup(system_css_path);
        } else {
            g_warning("ERROR: No CSS file found at user or system path.\n");
        }
    }

    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_USER);
    
    if (loaded_css_path) {
        g_autoptr(GFile) css_file = g_file_new_for_path(loaded_css_path);
        g_autoptr(GFileMonitor) monitor = g_file_monitor_file(css_file, G_FILE_MONITOR_NONE, NULL, NULL);

        if (monitor) {
            CssReloadData *reload_data = g_new(CssReloadData, 1);
            reload_data->provider = p;
            reload_data->path = g_strdup(loaded_css_path);

            g_signal_connect_data(monitor, "changed", G_CALLBACK(on_stylesheet_changed), reload_data, (GClosureNotify)free_css_reload_data, 0);
            g_object_set_data_full(G_OBJECT(a), "css-file-monitor", g_object_ref(monitor), g_object_unref);
        }
    }
    
    g_object_unref(p);
    g_free(loaded_css_path);
}


int main(int argc, char **argv) {
    app = gtk_application_new("com.meismeric.auranotify.ui", G_APPLICATION_IS_SERVICE);
    g_signal_connect(app, "startup", G_CALLBACK(on_app_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_bus_own_name(G_BUS_TYPE_SESSION, UI_BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, on_bus_acquired, on_name_acquired, on_name_lost, app, NULL);
    return g_application_run(G_APPLICATION(app), argc, argv);
}