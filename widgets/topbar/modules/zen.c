#include <gtk/gtk.h>
#include <gio/gio.h>
#include <math.h>
#include "zen.h"

const char* NOTIFY_DAEMON_BUS_NAME = "org.freedesktop.Notifications";
const char* NOTIFY_DAEMON_OBJECT_PATH = "/org/freedesktop/Notifications";
const char* NOTIFY_DAEMON_INTERFACE = "org.freedesktop.Notifications";

// Physics Constants for "Heavy Shutter" feel
const double SPRING_STIFFNESS = 0.15; // Lower = Looser, Higher = Snappier
const double FRICTION = 0.75;         // Lower = Bouncy, Higher = Heavy/Sluggish

typedef struct {
    GtkWidget *drawing_area;
    guint signal_subscription_id;
    
    gboolean is_dnd_active;
    
    // Physics State
    double progress;       // 0.0 -> 1.0 (Current position)
    double target;         // 0.0 or 1.0 (Target position)
    double velocity;       // Speed of movement
    guint animation_timer_id;
    
    // Cached Layouts
    PangoLayout *layout_active;
    PangoLayout *layout_inactive;
} ZenModule;

// --- Helper: Rounded Rectangle ---
static void cairo_rounded_rectangle(cairo_t *cr, double x, double y, double width, double height, double radius) {
    if (width <= 0 || height <= 0) return;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, 1.5 * G_PI);
    cairo_arc(cr, x + width - radius, y + radius, radius, 1.5 * G_PI, 2.0 * G_PI);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, 0.5 * G_PI);
    cairo_arc(cr, x + radius, y + height - radius, radius, 0.5 * G_PI, G_PI);
    cairo_close_path(cr);
}

// --- Physics Animation Logic (Spring) ---
static gboolean animation_tick(gpointer user_data) {
    ZenModule *mod = user_data;
    
    // 1. Calculate Spring Force
    // F = -k * x (Hooke's Law relative to target)
    double force = (mod->target - mod->progress) * SPRING_STIFFNESS;
    
    // 2. Apply Force to Velocity
    mod->velocity += force;
    
    // 3. Apply Friction (Damping)
    mod->velocity *= FRICTION;
    
    // 4. Apply Velocity to Position
    mod->progress += mod->velocity;
    
    // 5. Stop condition: Roughly at target and stopped moving
    if (fabs(mod->progress - mod->target) < 0.001 && fabs(mod->velocity) < 0.001) {
        mod->progress = mod->target;
        mod->velocity = 0;
        mod->animation_timer_id = 0;
        gtk_widget_queue_draw(mod->drawing_area);
        return G_SOURCE_REMOVE;
    }
    
    gtk_widget_queue_draw(mod->drawing_area);
    return G_SOURCE_CONTINUE;
}

static void set_dnd_visual_state(ZenModule *module, gboolean active) {
    module->is_dnd_active = active;
    module->target = active ? 1.0 : 0.0;
    
    // Start physics loop if not running
    if (module->animation_timer_id == 0) {
        module->animation_timer_id = g_timeout_add(16, animation_tick, module);
    }
}

// --- Drawing ---
static void draw_zen_toggle(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    ZenModule *mod = (ZenModule *)user_data;
    GtkStyleContext *ctx = gtk_widget_get_style_context(GTK_WIDGET(area));
    
    GdkRGBA bg_inactive, bg_active, fg_inactive, fg_active;
    
    // Colors
    if (!gtk_style_context_lookup_color(ctx, "theme_unfocused_color", &bg_inactive)) gdk_rgba_parse(&bg_inactive, "#3E3E41");
    if (!gtk_style_context_lookup_color(ctx, "theme_fg_color", &fg_inactive)) gdk_rgba_parse(&fg_inactive, "#ffffff");
    
    if (!gtk_style_context_lookup_color(ctx, "custom-accent", &bg_active)) {
        if (!gtk_style_context_lookup_color(ctx, "theme_selected_bg_color", &bg_active)) gdk_rgba_parse(&bg_active, "#8aadf4");
    }
    if (!gtk_style_context_lookup_color(ctx, "theme_bg_color", &fg_active)) gdk_rgba_parse(&fg_active, "#000000");

    // Initialize Fonts
    if (!mod->layout_inactive) {
        mod->layout_inactive = gtk_widget_create_pango_layout(GTK_WIDGET(area), "󰂚 Alerts");
        mod->layout_active = gtk_widget_create_pango_layout(GTK_WIDGET(area), "󰂛 Zen");
        
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        pango_layout_set_attributes(mod->layout_inactive, attrs);
        pango_layout_set_attributes(mod->layout_active, attrs);
        pango_attr_list_unref(attrs);
    }

    int text_w, text_h;

    // 1. Draw Base (Inactive) Background
    gdk_cairo_set_source_rgba(cr, &bg_inactive);
    cairo_rounded_rectangle(cr, 0, 0, width, height, 8.0);
    cairo_fill(cr);

    // 2. Draw "The Shutter" (Active Background)
    // The shutter slides in from the left based on physics 'progress'
    if (mod->progress > 0.01) {
        cairo_save(cr);
        // Clip to main rounded shape
        cairo_rounded_rectangle(cr, 0, 0, width, height, 8.0);
        cairo_clip(cr);
        
        double shutter_width = width * mod->progress;
        
        gdk_cairo_set_source_rgba(cr, &bg_active);
        cairo_rectangle(cr, 0, 0, shutter_width, height);
        cairo_fill(cr);
        
        // Optional: Draw a "Leading Edge" highlight for tactile feeling
        // A thin white line at the edge of the shutter
        if (mod->progress < 0.99) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.3);
            cairo_rectangle(cr, shutter_width - 2, 0, 2, height);
            cairo_fill(cr);
        }
        
        cairo_restore(cr);
    }

    // 3. Draw Text
    // We switch text color based on whether the shutter covers the center
    // Center point of text:
    double center_x = width / 2.0;
    double shutter_pos = width * mod->progress;
    
    // Choose layout and color based on DND state (logical) and Shutter pos (visual)
    PangoLayout *layout_to_draw = mod->is_dnd_active ? mod->layout_active : mod->layout_inactive;
    
    pango_layout_get_pixel_size(layout_to_draw, &text_w, &text_h);
    double txt_x = (width - text_w) / 2.0;
    double txt_y = (height - text_h) / 2.0;

    // If shutter covers the middle, use Active Color (Black/Bg), else Inactive Color (White/Fg)
    if (shutter_pos > center_x) {
        gdk_cairo_set_source_rgba(cr, &fg_active);
    } else {
        gdk_cairo_set_source_rgba(cr, &fg_inactive);
    }

    cairo_move_to(cr, txt_x, txt_y);
    pango_cairo_show_layout(cr, layout_to_draw);
}

// --- D-Bus Handlers ---

static void on_dnd_state_changed(GDBusConnection *connection, const gchar *sender_name, const gchar *object_path, const gchar *interface_name, const gchar *signal_name, GVariant *parameters, gpointer user_data) {
    (void)connection; (void)sender_name; (void)object_path; (void)interface_name; (void)signal_name;
    ZenModule *module = (ZenModule*)user_data;
    gboolean is_active;
    g_variant_get(parameters, "(b)", &is_active);
    set_dnd_visual_state(module, is_active);
}

static void on_get_initial_dnd_state(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    ZenModule *module = (ZenModule*)user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source_object), res, &error);
    if (error) {
        g_warning("Zen Module: Could not get initial DND state: %s", error->message);
        return;
    }
    gboolean is_active;
    g_variant_get(result, "(b)", &is_active);
    
    // Initial load: Set instantly, no physics
    module->is_dnd_active = is_active;
    module->target = is_active ? 1.0 : 0.0;
    module->progress = module->target; 
    module->velocity = 0;
    gtk_widget_queue_draw(module->drawing_area);
}

// --- Interaction ---

static void on_zen_clicked(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)gesture; (void)n_press; (void)x; (void)y; (void)user_data;
    g_autoptr(GDBusProxy) proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL, NOTIFY_DAEMON_BUS_NAME, NOTIFY_DAEMON_OBJECT_PATH, NOTIFY_DAEMON_INTERFACE, NULL, NULL);
    if(proxy) { g_dbus_proxy_call_sync(proxy, "ToggleDND", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL); }
}

static gboolean on_zen_scroll(GtkEventControllerScroll* controller, double dx, double dy, gpointer user_data) {
    (void)controller; (void)dx; (void)user_data;
    if (dy != 0) {
        on_zen_clicked(NULL, 0, 0, 0, NULL);
    }
    return TRUE;
}

static void zen_module_cleanup(gpointer data) {
    ZenModule *module = (ZenModule *)data;
    if (module->signal_subscription_id > 0) {
        g_dbus_connection_signal_unsubscribe(g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL), module->signal_subscription_id);
    }
    if (module->animation_timer_id > 0) g_source_remove(module->animation_timer_id);
    if (module->layout_active) g_object_unref(module->layout_active);
    if (module->layout_inactive) g_object_unref(module->layout_inactive);
    g_free(module);
}

// --- Creation ---

GtkWidget* create_zen_module() {
    ZenModule *module = g_new0(ZenModule, 1);
    
    module->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(module->drawing_area, 80, 28); 
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(module->drawing_area), draw_zen_toggle, module, NULL);
    
    gtk_widget_add_css_class(module->drawing_area, "zen-module");
    gtk_widget_add_css_class(module->drawing_area, "module");

    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_zen_clicked), module);
    gtk_widget_add_controller(module->drawing_area, GTK_EVENT_CONTROLLER(click));
    
    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_zen_scroll), module);
    gtk_widget_add_controller(module->drawing_area, scroll);
    
    module->signal_subscription_id = g_dbus_connection_signal_subscribe(g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL), NOTIFY_DAEMON_BUS_NAME, NOTIFY_DAEMON_INTERFACE, "DNDStateChanged", NOTIFY_DAEMON_OBJECT_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_dnd_state_changed, module, NULL);
    
    g_autoptr(GDBusProxy) proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL, NOTIFY_DAEMON_BUS_NAME, NOTIFY_DAEMON_OBJECT_PATH, NOTIFY_DAEMON_INTERFACE, NULL, NULL);
    if(proxy) { g_dbus_proxy_call(proxy, "GetDNDState", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, (GAsyncReadyCallback)on_get_initial_dnd_state, module); }

    g_object_set_data_full(G_OBJECT(module->drawing_area), "module-state", module, zen_module_cleanup);
    return module->drawing_area;
}