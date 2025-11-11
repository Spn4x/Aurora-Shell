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

// --- NEW: Struct for CSS Hot-Reloading ---
typedef struct {
    GtkCssProvider *provider;
    gchar *path;
} CssReloadData;


static GtkApplication *app = NULL;
static GtkWindow *main_window = NULL;
static IslandWidget *island = NULL;
static GQueue notification_queue = G_QUEUE_INIT;
static gboolean is_busy = FALSE;
static guint current_timeout_id = 0;
static gboolean is_expanded = FALSE;
static gboolean is_transitioning = FALSE;

static NotificationData *current_notification_data = NULL;

static void create_main_window();
static gboolean dismiss_or_transition(gpointer user_data);
static gboolean process_initial_notification(gpointer user_data);

static gboolean add_dot_class_cb(gpointer user_data);
static gboolean add_pill_class_cb(gpointer user_data);
static gboolean remove_dot_class_cb(gpointer user_data);
static gboolean populate_expanded_content_cb(gpointer user_data);

static void on_island_enter(GtkEventControllerMotion *controller, double x, double y, gpointer user_data);
static void on_island_leave(GtkEventControllerMotion *controller, gpointer user_data);

static void free_notification_data(gpointer data) {
    if (!data) return;
    NotificationData *notif = (NotificationData *)data;
    g_free(notif->icon);
    g_free(notif->summary);
    g_free(notif->body);
    g_free(notif);
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
    
    if (is_expanded) {
        current_timeout_id = g_timeout_add_seconds(EXPANDED_STATE_DURATION_S, dismiss_or_transition, NULL);
    } else {
        current_timeout_id = g_timeout_add(PILL_STATE_DURATION_MS, dismiss_or_transition, NULL);
    }
}

static gboolean outro_finished_callback(gpointer user_data G_GNUC_UNUSED) {
    if (!g_queue_is_empty(&notification_queue)) {
        g_print("UI: Outro complete, but new items are queued. Restarting cycle.\n");
        g_timeout_add(50, add_dot_class_cb, island);
        g_timeout_add(ANIMATION_FINISH_DELAY_MS, process_initial_notification, NULL);
    } else {
        g_print("UI: Outro complete. UI is now idle.\n");
        is_busy = FALSE;
        if (main_window) {
            gtk_widget_set_visible(GTK_WIDGET(main_window), FALSE);
        }
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

static void handle_show_notification(GDBusConnection *c G_GNUC_UNUSED, const gchar *s G_GNUC_UNUSED, const gchar *o G_GNUC_UNUSED, const gchar *i G_GNUC_UNUSED, const gchar *m G_GNUC_UNUSED, GVariant *p, GDBusMethodInvocation *inv, gpointer ud G_GNUC_UNUSED) {
    NotificationData *data = g_new(NotificationData, 1);
    g_variant_get(p, "(&s&s&s)", &data->icon, &data->summary, &data->body);
    data->icon = g_strdup(data->icon);
    data->summary = g_strdup(data->summary);
    data->body = g_strdup(data->body);
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
    
    g_print("\n--- DEBUG: WIDGET HIERARCHY --- \n");
    g_print("Created wrapper box, name: '%s'\n", gtk_widget_get_name(wrapper_box));

    island = ISLAND_WIDGET(island_widget_new());
    g_print("Created island widget, name: '%s'\n", gtk_widget_get_name(GTK_WIDGET(island)));

    gtk_box_append(GTK_BOX(wrapper_box), GTK_WIDGET(island));
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(island));
    g_print("Appended island to wrapper. Island's parent is now: '%s'\n", parent ? gtk_widget_get_name(parent) : "(null)");
    g_print("--- END DEBUG ---\n");
    
    gtk_window_set_child(main_window, wrapper_box);

    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_island_clicked), NULL);
    gtk_widget_add_controller(GTK_WIDGET(island), GTK_EVENT_CONTROLLER(click));

    GtkEventController *hover_controller = gtk_event_controller_motion_new();
    g_signal_connect(hover_controller, "enter", G_CALLBACK(on_island_enter), NULL);
    g_signal_connect(hover_controller, "leave", G_CALLBACK(on_island_leave), NULL);
    gtk_widget_add_controller(GTK_WIDGET(island), hover_controller);
    
    gtk_layer_init_for_window(main_window);
    gtk_layer_set_layer(main_window, GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_anchor(main_window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_margin(main_window, GTK_LAYER_SHELL_EDGE_TOP, 10);
}

static const GDBusInterfaceVTable interface_vtable = { .method_call = handle_show_notification };
static void on_bus_acquired(GDBusConnection *c, const gchar *n G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
    const gchar* xml = "<node><interface name='com.meismeric.auranotify.UI'><method name='ShowNotification'><arg type='s' name='icon' direction='in'/><arg type='s' name='summary' direction='in'/><arg type='s' name='body' direction='in'/></method></interface></node>";
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, NULL);
    g_dbus_connection_register_object(c, UI_OBJECT_PATH, node->interfaces[0], &interface_vtable, NULL, NULL, NULL);
    g_dbus_node_info_unref(node);
    g_print("UI: Headless service is running.\n");
}
static void on_name_acquired(GDBusConnection *c G_GNUC_UNUSED, const gchar *n G_GNUC_UNUSED, gpointer ud) { g_application_hold(G_APPLICATION(ud)); }
static void on_name_lost(GDBusConnection *c G_GNUC_UNUSED, const gchar *n G_GNUC_UNUSED, gpointer ud) { g_application_release(G_APPLICATION(ud)); }
static void activate(GtkApplication *a G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {}

// --- NEW: Callback that reloads the CSS when the file changes ---
static void on_stylesheet_changed(GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event_type, gpointer user_data) {
    (void)monitor; (void)file; (void)other_file;
    if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT) {
        CssReloadData *data = user_data;
        g_print("UI CSS Hot-Reload: File '%s' changed, reloading styles.\n", data->path);
        gtk_css_provider_load_from_path(data->provider, data->path);
    }
}

// --- NEW: Cleanup function for the reload data struct ---
static void free_css_reload_data(gpointer data) {
    CssReloadData *reload_data = data;
    g_free(reload_data->path);
    g_free(reload_data);
}

// --- MODIFIED: The startup handler now sets up the file monitor ---
static void on_app_startup(GApplication *a, gpointer ud G_GNUC_UNUSED) {
    g_print("\n--- DEBUG: CSS LOADING --- \n");
    GtkCssProvider *p = gtk_css_provider_new();
    gchar *loaded_css_path = NULL;
    
    g_autofree gchar *user_css_path = g_build_filename(g_get_user_config_dir(), "aurora-shell", "templates", "organizer", "organizer.css", NULL);
    g_print("Attempting to load user CSS from: %s\n", user_css_path);

    if (g_file_test(user_css_path, G_FILE_TEST_EXISTS)) {
        g_print("User CSS file found. Loading...\n");
        gtk_css_provider_load_from_path(p, user_css_path);
        loaded_css_path = g_strdup(user_css_path);
    } else {
        g_print("User CSS not found. Checking system path...\n");
        g_autofree gchar *system_css_path = NULL;
        #ifdef DATADIR
            system_css_path = g_build_filename(DATADIR, "templates", "organizer", "organizer.css", NULL);
        #else
            system_css_path = g_build_filename("/usr/local/share/aurora-shell/templates/organizer", "organizer.css", NULL);
        #endif
        g_print("Attempting to load system CSS from: %s\n", system_css_path);
        
        if(g_file_test(system_css_path, G_FILE_TEST_EXISTS)) {
            g_print("System CSS file found. Loading...\n");
            gtk_css_provider_load_from_path(p, system_css_path);
            loaded_css_path = g_strdup(system_css_path);
        } else {
            g_warning("ERROR: No CSS file found at user or system path.\n");
        }
    }

    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_object_unref(p); // The display holds its own reference

    // --- NEW: Set up the file monitor for hot-reloading ---
    if (loaded_css_path) {
        g_print("Setting up CSS hot-reload for: %s\n", loaded_css_path);
        g_autoptr(GFile) css_file = g_file_new_for_path(loaded_css_path);
        g_autoptr(GFileMonitor) monitor = g_file_monitor_file(css_file, G_FILE_MONITOR_NONE, NULL, NULL);

        if (monitor) {
            CssReloadData *reload_data = g_new(CssReloadData, 1);
            reload_data->provider = p; // The provider is still valid here
            reload_data->path = g_strdup(loaded_css_path);

            g_signal_connect_data(monitor, "changed", G_CALLBACK(on_stylesheet_changed), reload_data, (GClosureNotify)free_css_reload_data, 0);

            // Keep the monitor alive by attaching it to the application object
            g_object_set_data_full(G_OBJECT(a), "css-file-monitor", g_object_ref(monitor), g_object_unref);
        }
    }

    g_free(loaded_css_path);
    g_print("--- END DEBUG ---\n\n");
}


int main(int argc, char **argv) {
    app = gtk_application_new("com.meismeric.auranotify.ui", G_APPLICATION_IS_SERVICE);
    g_signal_connect(app, "startup", G_CALLBACK(on_app_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_bus_own_name(G_BUS_TYPE_SESSION, UI_BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, on_bus_acquired, on_name_acquired, on_name_lost, app, NULL);
    return g_application_run(G_APPLICATION(app), argc, argv);
}