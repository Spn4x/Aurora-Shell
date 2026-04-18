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
    GtkCssProvider *provider;
    gchar *path;
} CssReloadData;

typedef struct {
    GtkCssProvider *provider;
    char *path;
} GlobalThemeData;

static GtkApplication *app = NULL;
static GtkWindow *main_window = NULL;
static IslandWidget *island = NULL;
static GQueue notification_queue = G_QUEUE_INIT;
static gboolean is_busy = FALSE;
static guint current_timeout_id = 0;
static gboolean is_expanded = FALSE;
static gboolean is_transitioning = FALSE;
static NotificationData *current_notification_data = NULL;

// --- PRIVACY & OSD STATE GLOBALS ---
static gboolean privacy_mic_active = FALSE;
static gboolean privacy_cam_active = FALSE;

static gboolean is_osd_active = FALSE;
static GtkWidget *current_osd_level_bar = NULL;
static GtkWidget *current_osd_icon = NULL;
static GtkWidget *current_osd_overvol_label = NULL; // NEW TRACKER

static void create_main_window();
static gboolean dismiss_or_transition(gpointer user_data);
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

// --- OSD PILL BUILDER ---
static GtkWidget* create_osd_pill(const gchar *icon_name, double level) {
    GtkWidget *overlay = gtk_overlay_new();

    // MATCH STANDARD PILL WIDTH using the dummy label trick
    GtkWidget *dummy_label = gtk_label_new(" ");
    gtk_widget_add_css_class(dummy_label, "summary");
    gtk_label_set_width_chars(GTK_LABEL(dummy_label), 25);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), dummy_label);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(box, GTK_ALIGN_FILL);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(box, 14);
    gtk_widget_set_margin_end(box, 14);

    current_osd_icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(current_osd_icon), 18);

    current_osd_level_bar = gtk_level_bar_new();
    gtk_level_bar_set_min_value(GTK_LEVEL_BAR(current_osd_level_bar), 0.0);
    gtk_level_bar_set_max_value(GTK_LEVEL_BAR(current_osd_level_bar), 1.0);
    gtk_widget_set_valign(current_osd_level_bar, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(current_osd_level_bar, TRUE); // Dynamically fill the remaining space
    gtk_widget_set_size_request(current_osd_level_bar, -1, 6); // Lock height only
    gtk_widget_add_css_class(current_osd_level_bar, "osd-bar");

    current_osd_overvol_label = gtk_label_new("");
    gtk_widget_add_css_class(current_osd_overvol_label, "osd-overvol");

    // Check if over 100% volume
    if (level > 1.0) {
        gtk_level_bar_set_value(GTK_LEVEL_BAR(current_osd_level_bar), 1.0);
        g_autofree gchar *text = g_strdup_printf("+%.0f%%", (level - 1.0) * 100.0);
        gtk_label_set_text(GTK_LABEL(current_osd_overvol_label), text);
        gtk_widget_set_visible(current_osd_overvol_label, TRUE);
    } else {
        gtk_level_bar_set_value(GTK_LEVEL_BAR(current_osd_level_bar), level);
        gtk_widget_set_visible(current_osd_overvol_label, FALSE);
    }

    gtk_box_append(GTK_BOX(box), current_osd_icon);
    gtk_box_append(GTK_BOX(box), current_osd_level_bar);
    gtk_box_append(GTK_BOX(box), current_osd_overvol_label);

    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), box);

    return overlay;
}

// --- PRIVACY PILL BUILDER ---
static GtkWidget* create_privacy_pill(gboolean mic, gboolean cam) {
    GtkWidget *overlay = gtk_overlay_new();

    GtkWidget *dummy_label = gtk_label_new(" ");
    gtk_widget_add_css_class(dummy_label, "summary");
    gtk_label_set_width_chars(GTK_LABEL(dummy_label), 25);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), dummy_label);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    GtkWidget *dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(dot, 12, 12);
    gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(dot, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(dot, "privacy-indicator");
    
    const char *text = "";
    if (mic && cam) {
        gtk_widget_add_css_class(dot, "both");
        text = "Mic & Screen/Camera"; 
    } else if (mic) {
        gtk_widget_add_css_class(dot, "mic");
        text = "Microphone in use";
    } else if (cam) {
        gtk_widget_add_css_class(dot, "cam");
        text = "Screen/Camera in use";
    }

    GtkWidget *label = gtk_label_new(text);
    gtk_widget_add_css_class(label, "summary");

    gtk_box_append(GTK_BOX(box), dot);
    gtk_box_append(GTK_BOX(box), label);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), box);

    return overlay;
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
        g_timeout_add(50, add_dot_class_cb, island);
        g_timeout_add(ANIMATION_FINISH_DELAY_MS, process_initial_notification, NULL);
    } else {
        if (!privacy_mic_active && !privacy_cam_active) {
            is_busy = FALSE;
            if (main_window) gtk_widget_set_visible(GTK_WIDGET(main_window), FALSE);
        } else {
            is_busy = FALSE; 
        }
    }
    is_transitioning = FALSE;
    return G_SOURCE_REMOVE;
}

static gboolean dismiss_or_transition(gpointer user_data G_GNUC_UNUSED) {
    if (is_transitioning) return G_SOURCE_REMOVE;
    is_transitioning = TRUE;

    if (current_timeout_id > 0) {
        g_source_remove(current_timeout_id);
        current_timeout_id = 0;
    }

    // Reset OSD state
    is_osd_active = FALSE;
    current_osd_level_bar = NULL;
    current_osd_icon = NULL;
    current_osd_overvol_label = NULL;

    if (!g_queue_is_empty(&notification_queue)) {
        show_next_notification();
    } 
    else {
        if (current_notification_data) {
            free_notification_data(current_notification_data);
            current_notification_data = NULL;
        }
        
        if (is_expanded) {
            is_expanded = FALSE;
            island_widget_set_expanded(island, FALSE);
        }
        
        // --- PRIVACY CHECK FALLBACK ---
        if (privacy_mic_active || privacy_cam_active) {
            GtkWidget *pill = create_privacy_pill(privacy_mic_active, privacy_cam_active);
            island_widget_transition_to_pill_child(island, pill);
            
            gtk_widget_add_css_class(GTK_WIDGET(island), "pill");
            gtk_widget_add_css_class(GTK_WIDGET(island), "dot");
            
            g_timeout_add(ANIMATION_FINISH_DELAY_MS, outro_finished_callback, NULL);
        } else {
            if (island) gtk_widget_remove_css_class(GTK_WIDGET(island), "pill");
            g_timeout_add(250, remove_dot_class_cb, island);
            g_timeout_add(ANIMATION_FINISH_DELAY_MS, outro_finished_callback, NULL);
        }
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
    if (is_transitioning || is_osd_active) return;

    if (!is_expanded) {
        if (!current_notification_data) return; 
        is_transitioning = TRUE;
        if (current_timeout_id > 0) { g_source_remove(current_timeout_id); current_timeout_id = 0; }
        
        is_expanded = TRUE;
        island_widget_set_expanded(island, TRUE);
        g_timeout_add(50, populate_expanded_content_cb, NULL);
        
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
        dismiss_or_transition(NULL);
    }
}

static void on_island_enter(GtkEventControllerMotion *controller G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED) {
    if (is_expanded && current_timeout_id > 0) {
        g_source_remove(current_timeout_id);
        current_timeout_id = 0;
    }
}

static void on_island_leave(GtkEventControllerMotion *controller G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED) {
    if (is_expanded && current_timeout_id == 0) {
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

// --- D-BUS HANDLERS ---
static void handle_show_notification(GDBusMethodInvocation *inv, NotificationData *data) {
    g_queue_push_tail(&notification_queue, data);
    
    if (!is_busy) {
        is_busy = TRUE;
        if (main_window == NULL) create_main_window();

        gtk_widget_set_visible(GTK_WIDGET(main_window), TRUE);
        g_timeout_add(50, add_dot_class_cb, island);
        g_timeout_add(ANIMATION_FINISH_DELAY_MS, process_initial_notification, NULL);
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}

static void handle_set_privacy_status(GDBusMethodInvocation *inv, gboolean mic, gboolean cam) {
    privacy_mic_active = mic;
    privacy_cam_active = cam;

    if (!is_busy && g_queue_is_empty(&notification_queue)) {
        if (mic || cam) {
            if (main_window == NULL) create_main_window();
            
            GtkWidget *pill = create_privacy_pill(mic, cam);
            island_widget_transition_to_pill_child(island, pill);
            
            gboolean was_visible = gtk_widget_get_visible(GTK_WIDGET(main_window));
            gtk_widget_set_visible(GTK_WIDGET(main_window), TRUE);
            
            if (!was_visible) {
                gtk_widget_add_css_class(GTK_WIDGET(island), "dot");
                g_timeout_add(100, add_pill_class_cb, island);
            } else {
                gtk_widget_add_css_class(GTK_WIDGET(island), "dot");
                gtk_widget_add_css_class(GTK_WIDGET(island), "pill");
            }
        } else {
            if (island && gtk_widget_get_visible(GTK_WIDGET(main_window))) {
                is_transitioning = TRUE;
                gtk_widget_remove_css_class(GTK_WIDGET(island), "pill");
                g_timeout_add(250, remove_dot_class_cb, island);
                g_timeout_add(ANIMATION_FINISH_DELAY_MS, outro_finished_callback, NULL);
            }
        }
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}

static void handle_show_osd(GDBusMethodInvocation *inv, const gchar *icon, double level) {
    if (is_osd_active && current_osd_level_bar && current_osd_icon && current_osd_overvol_label) {
        // Just update existing if it's already showing
        gtk_image_set_from_icon_name(GTK_IMAGE(current_osd_icon), icon);
        
        if (level > 1.0) {
            gtk_level_bar_set_value(GTK_LEVEL_BAR(current_osd_level_bar), 1.0);
            g_autofree gchar *text = g_strdup_printf("+%.0f%%", (level - 1.0) * 100.0);
            gtk_label_set_text(GTK_LABEL(current_osd_overvol_label), text);
            gtk_widget_set_visible(current_osd_overvol_label, TRUE);
        } else {
            gtk_level_bar_set_value(GTK_LEVEL_BAR(current_osd_level_bar), level);
            gtk_widget_set_visible(current_osd_overvol_label, FALSE);
        }
        
        if (current_timeout_id > 0) g_source_remove(current_timeout_id);
        current_timeout_id = g_timeout_add(1500, dismiss_or_transition, NULL);

    } else {
        // Force takeover of the island
        is_busy = TRUE;
        is_osd_active = TRUE;
        if (main_window == NULL) create_main_window();

        GtkWidget *pill = create_osd_pill(icon, level);
        island_widget_transition_to_pill_child(island, pill);

        gboolean was_visible = gtk_widget_get_visible(GTK_WIDGET(main_window));
        gtk_widget_set_visible(GTK_WIDGET(main_window), TRUE);

        if (!was_visible) {
            gtk_widget_add_css_class(GTK_WIDGET(island), "dot");
            g_timeout_add(100, add_pill_class_cb, island);
        } else {
            if (is_expanded) {
                island_widget_set_expanded(island, FALSE);
                is_expanded = FALSE;
            }
            gtk_widget_add_css_class(GTK_WIDGET(island), "dot");
            gtk_widget_add_css_class(GTK_WIDGET(island), "pill");
        }
        
        if (current_timeout_id > 0) g_source_remove(current_timeout_id);
        current_timeout_id = g_timeout_add(1500, dismiss_or_transition, NULL);
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
    } else if (g_strcmp0(m, "SetPrivacyStatus") == 0) {
        gboolean mic, cam;
        g_variant_get(p, "(bb)", &mic, &cam);
        handle_set_privacy_status(inv, mic, cam);
    } else if (g_strcmp0(m, "ShowOSD") == 0) {
        gchar *icon; double level;
        g_variant_get(p, "(&sd)", &icon, &level);
        handle_show_osd(inv, icon, level);
    } else {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method '%s'", m);
    }
}

static void on_window_destroyed(GtkWidget *w G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {
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
    // Buffer space to allow the bouncy animation to not get clipped by Wayland margins
    gtk_widget_set_margin_start(wrapper_box, 40);
    gtk_widget_set_margin_end(wrapper_box, 40);
    gtk_widget_set_margin_bottom(wrapper_box, 40);
    
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
        "    <method name='SetPrivacyStatus'>"
        "      <arg type='b' name='mic' direction='in'/>"
        "      <arg type='b' name='cam' direction='in'/>"
        "    </method>"
        "    <method name='ShowOSD'>"
        "      <arg type='s' name='icon' direction='in'/>"
        "      <arg type='d' name='level' direction='in'/>"
        "    </method>"
        "  </interface>"
        "</node>";
    
    static const GDBusInterfaceVTable vtable = { .method_call = dbus_method_dispatcher };

    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, NULL);
    g_dbus_connection_register_object(c, UI_OBJECT_PATH, node->interfaces[0], &vtable, NULL, NULL, NULL);
    g_dbus_node_info_unref(node);
}

static void on_name_acquired(GDBusConnection *c G_GNUC_UNUSED, const gchar *n G_GNUC_UNUSED, gpointer ud) { g_application_hold(G_APPLICATION(ud)); }
static void on_name_lost(GDBusConnection *c G_GNUC_UNUSED, const gchar *n G_GNUC_UNUSED, gpointer ud) { g_application_release(G_APPLICATION(ud)); }
static void activate(GtkApplication *a G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) {}

static void on_stylesheet_changed(GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event_type, gpointer user_data) {
    (void)monitor; (void)file; (void)other_file;
    if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT) {
        CssReloadData *data = user_data;
        gtk_css_provider_load_from_path(data->provider, data->path);
    }
}

static void free_css_reload_data(gpointer data, GClosure *closure) {
    (void)closure;
    CssReloadData *reload_data = data;
    g_free(reload_data->path);
    g_free(reload_data);
}

static void on_global_theme_changed(GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event_type, gpointer user_data) {
    (void)monitor; (void)file; (void)other_file;
    if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT) {
        GlobalThemeData *data = (GlobalThemeData *)user_data;
        gtk_css_provider_load_from_path(data->provider, data->path);
    }
}

static void load_global_theme_colors(GApplication *app_instance) {
    g_autofree gchar *colors_path = g_build_filename(g_get_user_config_dir(), "aurora-shell", "aurora-colors.css", NULL);
    
    if (g_file_test(colors_path, G_FILE_TEST_EXISTS)) {
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
            
            g_signal_connect(monitor, "changed", G_CALLBACK(on_global_theme_changed), data);
            g_object_set_data_full(G_OBJECT(app_instance), "global-theme-monitor", monitor, g_object_unref);
        }
        g_object_unref(file);
    }
}

static void on_app_startup(GApplication *a, gpointer ud G_GNUC_UNUSED) {
    load_global_theme_colors(a);
    
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
        }
    }

    // --- Inject OSD LevelBar CSS Internally ---
    const char *osd_css = 
        ".osd-bar { margin: 0; padding: 0; transition: min-width 0.2s cubic-bezier(0.25, 0.46, 0.45, 0.94); }"
        ".osd-bar trough { background-color: rgba(255,255,255,0.2); border-radius: 99px; min-height: 6px; margin: 0; }"
        ".osd-bar block.filled { background-color: #ffffff; border-radius: 99px; border: none; box-shadow: none; }"
        ".osd-bar block.empty { background-color: transparent; border: none; box-shadow: none; }"
        ".osd-overvol { font-size: 12px; font-weight: 900; color: #ffffff; margin-left: 2px; }";
    GtkCssProvider *osd_p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(osd_p, osd_css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(osd_p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(osd_p);
    // ------------------------------------------

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