#include <gtk/gtk.h>
#include <adwaita.h>
#include <gtk4-layer-shell.h>
#include <math.h>
#include <gdk/gdk.h>

#include "globals.h"
#include "drawer.h"
#include "widget_template.h"
#include "storage.h"
#include "wallpaper_chooser.h"
#include "../scenes/registry.h"
#include "theme_manager.h"
#include "editor.h"

SurfaceDeskApp app_state;
static GtkWidget *dim_layer = NULL;
static guint dbus_owner_id = 0;

static void load_css() {
    GtkCssProvider *provider = gtk_css_provider_new();
    g_autofree char *css = g_strdup_printf(
        "@define-color default_accent #78aeed;"
        "window { color: @window_fg_color; }"

        /* Defeat aurora-shell's forced padding/borders for widgets */
        "#surfacedesk-root.module {"
        "   background: transparent !important; border: none !important; box-shadow: none !important;"
        "   margin: 0 !important; padding: 0 !important; border-radius: 0 !important;"
        "}"

        ".desktop-container {"
        "   transition: transform 250ms cubic-bezier(0.25, 0.46, 0.45, 0.94);"
        "   transform: scale(1.0);"
        "   transform-origin: center center;"
        "}"
        ".desktop-container.editing {"
        "   transform: scale(%.2f);"
        "}"
        ".dim-layer { background-color: rgba(0,0,0,0); transition: background-color 250ms ease; }"
        ".dim-layer.dimmed { background-color: rgba(0,0,0,0.65); }"
        
        ".tile-container { background: none; border: none; padding: 0; margin: 0; overflow: visible; }"
        "window .card {"
        "   background-color: @window_bg_color; color: @fg_color;"
        "   border: 1px solid @borders; border-radius: 12px;"
        "   box-shadow: 0 4px 12px rgba(0,0,0,0.15);"
        "   transition: background-color 200ms ease, transform 200ms ease;"
        "   margin: 0; padding: 0;"
        "}"
        "window .card:hover { background-color: shade(@window_bg_color, 1.05); box-shadow: 0 8px 20px rgba(0,0,0,0.25); }"
        "window .card.transparent { background-color: transparent; border: none; box-shadow: none; backdrop-filter: none; }"
        ".wallpaper-card { transition: transform 200ms cubic-bezier(0.25, 0.46, 0.45, 0.94), opacity 200ms ease, filter 200ms ease; transform: scale(0.85); transform-origin: center center; opacity: 0.6; filter: grayscale(30%%); outline: none; margin: 10px; }"
        ".wallpaper-card:hover, .wallpaper-card:focus { transform: scale(1.15); opacity: 1.0; filter: grayscale(0%%); }"
        ".wallpaper-card:active { transform: scale(1.05); }"
        ".wallpaper-img { border-radius: 12px; border: 2px solid rgba(255,255,255,0.1); transition: all 200ms ease; box-shadow: 0 5px 15px rgba(0,0,0,0.3); }"
        ".wallpaper-card:focus .wallpaper-img, .wallpaper-card:hover .wallpaper-img { border-color: #ffffff; box-shadow: 0 10px 30px rgba(0,0,0,0.5); }"
        ".caption { color: white; text-shadow: 0 2px 3px rgba(0,0,0,0.8); font-weight: 700; font-size: 13px; opacity: 0.5; transition: opacity 200ms; }"
        ".wallpaper-card:focus .caption, .wallpaper-card:hover .caption { opacity: 1.0; }"
        ".drawer-bg { background-color: @popover_bg_color; color: @popover_fg_color; border-left: 1px solid alpha(@popover_fg_color, 0.1); border-radius: 20px; box-shadow: 0 10px 40px rgba(0,0,0,0.3); overflow: hidden; }"
        ".drawer-header { font-weight: 900; font-size: 24px; opacity: 0.9; }"
        ".drawer-preview { border-radius: 8px; transition: background 0.2s; }"
        ".drawer-preview:hover { background-color: alpha(@popover_fg_color, 0.08); }"
        ".display-large { font-size: 64px; font-weight: 800; line-height: 0.8; letter-spacing: -1px; }"
        ".title-1 { font-size: 32px; font-weight: 800; letter-spacing: -0.5px; line-height: 1.0; }"
        ".title-2 { font-size: 24px; font-weight: 700; line-height: 1.0; }"
        ".title-3 { font-size: 16px; font-weight: 600; color: alpha(@fg_color, 0.7); }"
        ".trash-zone { background-color: @error_bg_color; color: @error_fg_color; border-radius: 12px; border: 1px solid @error_fg_color; }"
        ".trash-zone.hover { filter: brightness(1.2); }",
        EDIT_SCALE
    );
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    theme_manager_init();
}

static void set_edit_mode(gboolean enable);

void update_ui_visibility(void) {
    gboolean dragging = app_state.physics.is_dragging;

    if (app_state.drawer_revealer) gtk_revealer_set_reveal_child(GTK_REVEALER(app_state.drawer_revealer), FALSE);
    if (app_state.trash_revealer) gtk_revealer_set_reveal_child(GTK_REVEALER(app_state.trash_revealer), FALSE);
    if (app_state.wallpaper_revealer) gtk_revealer_set_reveal_child(GTK_REVEALER(app_state.wallpaper_revealer), FALSE);
    if (app_state.show_drawer_btn) gtk_widget_set_visible(app_state.show_drawer_btn, FALSE);

    if (app_state.is_editing) {
        if (dragging) {
            if (app_state.trash_revealer) gtk_revealer_set_reveal_child(GTK_REVEALER(app_state.trash_revealer), TRUE);
        } else {
            if (app_state.drawer_revealer) gtk_revealer_set_reveal_child(GTK_REVEALER(app_state.drawer_revealer), TRUE);
        }
    } else if (app_state.is_picking_wallpaper) {
        if (app_state.wallpaper_revealer) gtk_revealer_set_reveal_child(GTK_REVEALER(app_state.wallpaper_revealer), TRUE);
        wallpaper_chooser_grab_focus();
    }
}

static void on_desktop_clicked(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    if (app_state.window) gtk_window_present(app_state.window);
    if (app_state.is_editing) editor_on_click_select(gesture, n_press, x, y, user_data);
}

// THE FIX: Scroll zoom disabled entirely!
static gboolean on_desktop_scroll(GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data) {
    return FALSE; 
}

// THE FIX: Properly capture Escape to untoggle, without letting it leak.
static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    if (keyval == GDK_KEY_Escape) {
        if (app_state.is_editing) set_edit_mode(FALSE);
        else if (app_state.is_picking_wallpaper) { wallpaper_chooser_cancel(); app_set_wallpaper_mode(FALSE); }
        return TRUE; 
    }
    if ((state & GDK_SUPER_MASK) && (keyval == GDK_KEY_w || keyval == GDK_KEY_W)) {
        if (app_state.is_picking_wallpaper) { wallpaper_chooser_cancel(); app_set_wallpaper_mode(FALSE); }
        else { if (app_state.is_editing) set_edit_mode(FALSE); app_set_wallpaper_mode(TRUE); }
        return TRUE;
    }
    return FALSE;
}

void app_set_wallpaper_mode(gboolean enable) {
    if (app_state.is_picking_wallpaper == enable) return;
    if (enable) {
        app_state.is_editing = FALSE; app_state.is_picking_wallpaper = TRUE;
        if (app_state.original_wallpaper_path) g_free(app_state.original_wallpaper_path);
        app_state.original_wallpaper_path = g_strdup(app_state.current_wallpaper_path);
        gtk_widget_add_css_class(app_state.desktop_container, "editing");
        gtk_widget_set_can_target(app_state.physics.overlay_draw, FALSE);
        
        // THE FIX: Force window to grab focus so Esc key events register instantly
        if (app_state.window) {
            gtk_layer_set_keyboard_mode(app_state.window, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
            gtk_window_present(app_state.window);
            gtk_widget_grab_focus(app_state.desktop_container);
        }
        
        if (dim_layer) gtk_widget_add_css_class(dim_layer, "dimmed");

        recalculate_targets(-1, -1);
        app_state.physics.current_offset_x = app_state.physics.target_offset_x;
        app_state.physics.current_offset_y = app_state.physics.target_offset_y;
    } else {
        app_state.is_picking_wallpaper = FALSE;
        if (!app_state.is_editing) {
            gtk_widget_remove_css_class(app_state.desktop_container, "editing");
            gtk_widget_set_can_target(app_state.physics.overlay_draw, FALSE);
            
            // THE FIX: Release keyboard focus so Esc drops cleanly and returns focus to other apps
            if (app_state.window) {
                gtk_layer_set_keyboard_mode(app_state.window, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
            }
            if (dim_layer) gtk_widget_remove_css_class(dim_layer, "dimmed");
        }
    }
    update_ui_visibility();
    gtk_widget_queue_draw(app_state.physics.overlay_draw);
}

static void set_edit_mode(gboolean enable) {
    if (app_state.is_editing == enable) return;
    if (enable) app_state.is_picking_wallpaper = FALSE;
    app_state.is_editing = enable;
    if (enable) {
        gtk_widget_add_css_class(app_state.desktop_container, "editing");
        gtk_widget_set_visible(app_state.show_drawer_btn, FALSE);
        gtk_widget_set_can_target(app_state.physics.overlay_draw, TRUE);
        
        // THE FIX: Force window to grab focus so Esc key events register instantly
        if (app_state.window) {
            gtk_layer_set_keyboard_mode(app_state.window, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
            gtk_window_present(app_state.window);
            gtk_widget_grab_focus(app_state.desktop_container);
        }
        
        if (dim_layer) gtk_widget_add_css_class(dim_layer, "dimmed");

        drawer_show_library();
        recalculate_targets(-1, -1);
        app_state.physics.current_offset_x = app_state.physics.target_offset_x;
        app_state.physics.current_offset_y = app_state.physics.target_offset_y;
        for (GList *l = app_state.physics.boxes; l != NULL; l = l->next) update_widget_geometry_safe((Box*)l->data);
    } else {
        recalculate_targets(-1, -1);
        if (dim_layer) gtk_widget_remove_css_class(dim_layer, "dimmed");
        if (!app_state.is_picking_wallpaper) {
            gtk_widget_remove_css_class(app_state.desktop_container, "editing");
            gtk_widget_set_can_target(app_state.physics.overlay_draw, FALSE);
            
            // THE FIX: Release keyboard focus so Esc drops cleanly and returns focus to other apps
            if (app_state.window) {
                gtk_layer_set_keyboard_mode(app_state.window, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
            }
        }
        app_state.physics.active_box = NULL;
        app_state.physics.selected_box = NULL;
        if (app_state.window) {
            GtkNative *native = GTK_NATIVE(app_state.window);
            GdkSurface *surface = gtk_native_get_surface(native);
            if (surface) gdk_surface_set_cursor(surface, NULL);
        }
    }
    update_ui_visibility();
    gtk_widget_queue_draw(app_state.physics.overlay_draw);
}

static GtkWidget* create_trash_bin(void) {
    GtkWidget *revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), 250);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "trash-zone");
    gtk_widget_set_size_request(box, 300, 80);
    gtk_widget_set_margin_bottom(box, 30);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_END);
    GtkWidget *label = gtk_label_new("Remove");
    gtk_widget_add_css_class(label, "title-2");
    gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), label);
    gtk_revealer_set_child(GTK_REVEALER(revealer), box);
    app_state.trash_box = box;
    return revealer;
}

static void on_show_drawer_clicked(GtkButton *btn, gpointer user_data) {
    gtk_widget_set_visible(app_state.show_drawer_btn, FALSE);
    gtk_revealer_set_reveal_child(GTK_REVEALER(app_state.drawer_revealer), TRUE);
}

static void on_change_wallpaper_activate(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action; (void)parameter; (void)user_data;
    if (app_state.is_picking_wallpaper) {
        wallpaper_chooser_cancel();
    }
    app_set_wallpaper_mode(!app_state.is_picking_wallpaper);
}

static void on_edit_scenes_activate(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action; (void)parameter; (void)user_data;
    set_edit_mode(!app_state.is_editing);
}

static void on_right_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; (void)user_data;
    if (!app_state.is_editing && !app_state.is_picking_wallpaper) {
        GMenu *menu = g_menu_new();
        g_menu_append(menu, "Edit Desktop", "desktop.edit_scenes");
        g_menu_append(menu, "Change Wallpaper", "desktop.change_wallpaper");

        GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
        gtk_widget_set_parent(popover, app_state.desktop_container);
        GdkRectangle rect = { (int)x, (int)y, 1, 1 };
        gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
        gtk_popover_popup(GTK_POPOVER(popover));
        g_object_unref(menu);
    }
}

static gboolean on_startup_snap(gpointer data) {
    if (!app_state.physics.overlay_draw) return G_SOURCE_CONTINUE;
    int h = gtk_widget_get_height(app_state.physics.overlay_draw);
    if (h > 1) { editor_force_snap(); return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

static void handle_method_call(GDBusConnection *c, const gchar *s, const gchar *o, const gchar *i, const gchar *method, GVariant *p, GDBusMethodInvocation *inv, gpointer ud) {
    if (g_strcmp0(method, "ToggleEditMode") == 0) {
        set_edit_mode(!app_state.is_editing);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (g_strcmp0(method, "ToggleWallpaperMode") == 0) {
        if (app_state.is_picking_wallpaper) {
            wallpaper_chooser_cancel();
        }
        app_set_wallpaper_mode(!app_state.is_picking_wallpaper);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method %s", method);
    }
}

static const GDBusInterfaceVTable vtable = { .method_call = handle_method_call };

static void on_bus_acquired(GDBusConnection *c, const gchar *name, gpointer ud) {
    const gchar* xml = "<node><interface name='com.meismeric.SurfaceDesk'>"
                       "<method name='ToggleEditMode'/>"
                       "<method name='ToggleWallpaperMode'/>"
                       "</interface></node>";
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, NULL);
    g_dbus_connection_register_object(c, "/com/meismeric/SurfaceDesk", node->interfaces[0], &vtable, NULL, NULL, NULL);
    g_dbus_node_info_unref(node);
}

static void on_realize(GtkWidget *widget, gpointer data) {
    app_state.window = GTK_WINDOW(gtk_widget_get_root(widget));
    // THE FIX: Set background widget to NONE on boot so it never intercepts key presses
    gtk_layer_set_keyboard_mode(app_state.window, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
}

static void on_destroy(GtkWidget *widget, gpointer data) {
    if (dbus_owner_id > 0) g_bus_unown_name(dbus_owner_id);
}

G_MODULE_EXPORT GtkWidget* create_widget(const char *config_string) {
    memset(&app_state, 0, sizeof(SurfaceDeskApp));
    load_css();

    app_state.physics.cell_size = 80;
    app_state.physics.resize_cursor = gdk_cursor_new_from_name("crosshair", NULL);
    app_state.physics.move_cursor = gdk_cursor_new_from_name("move", NULL);
    app_state.physics.default_cursor = gdk_cursor_new_from_name("default", NULL);

    GtkWidget *root_overlay = gtk_overlay_new();
    gtk_widget_set_name(root_overlay, "surfacedesk-root"); 
    g_signal_connect(root_overlay, "realize", G_CALLBACK(on_realize), NULL);
    g_signal_connect(root_overlay, "destroy", G_CALLBACK(on_destroy), NULL);

    app_state.desktop_container = gtk_overlay_new();
    gtk_widget_add_css_class(app_state.desktop_container, "desktop-container");
    gtk_widget_set_hexpand(app_state.desktop_container, TRUE);
    gtk_widget_set_vexpand(app_state.desktop_container, TRUE);
    
    // THE FIX: Allow GTK to focus the container so our key events fire reliably
    gtk_widget_set_focusable(app_state.desktop_container, TRUE);

    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_desktop_scroll), NULL);
    gtk_widget_add_controller(app_state.desktop_container, scroll);

    gtk_overlay_set_child(GTK_OVERLAY(root_overlay), app_state.desktop_container);

    app_state.wallpaper_image = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(app_state.wallpaper_image), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_can_target(app_state.wallpaper_image, TRUE); 
    
    // THE FIX: Wallpaper goes back INSIDE desktop_container so it zooms down perfectly
    gtk_overlay_set_child(GTK_OVERLAY(app_state.desktop_container), app_state.wallpaper_image);
    
    GtkGesture *wall_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(wall_click), GDK_BUTTON_PRIMARY);
    g_signal_connect(wall_click, "released", G_CALLBACK(on_desktop_clicked), NULL);
    gtk_widget_add_controller(app_state.wallpaper_image, GTK_EVENT_CONTROLLER(wall_click));

    dim_layer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(dim_layer, TRUE);
    gtk_widget_set_vexpand(dim_layer, TRUE);
    gtk_widget_set_can_target(dim_layer, TRUE);
    gtk_widget_add_css_class(dim_layer, "dim-layer");
    
    GtkGesture *shield_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(shield_click), GDK_BUTTON_PRIMARY);
    g_signal_connect(shield_click, "released", G_CALLBACK(on_desktop_clicked), NULL);
    gtk_widget_add_controller(dim_layer, GTK_EVENT_CONTROLLER(shield_click));
    gtk_overlay_add_overlay(GTK_OVERLAY(app_state.desktop_container), dim_layer);

    GtkWidget *fixed = gtk_fixed_new();
    gtk_overlay_add_overlay(GTK_OVERLAY(app_state.desktop_container), fixed);
    app_state.physics.fixed_container = fixed;

    load_layout();
    
    if (app_state.current_wallpaper_path && g_file_test(app_state.current_wallpaper_path, G_FILE_TEST_EXISTS)) {
        GFile *target_bg = g_file_new_for_path(app_state.current_wallpaper_path);
        gtk_picture_set_file(GTK_PICTURE(app_state.wallpaper_image), target_bg);
        g_object_unref(target_bg);
    } else {
        GFile *target_bg = g_file_new_for_path("/usr/share/backgrounds/gnome/blobs-l.svg");
        gtk_picture_set_file(GTK_PICTURE(app_state.wallpaper_image), target_bg);
        g_object_unref(target_bg);
    }

    recalculate_targets(-1, -1);
    for (GList *l = app_state.physics.boxes; l != NULL; l = l->next) update_widget_geometry_safe((Box*)l->data);

    GtkWidget *draw = gtk_drawing_area_new();
    gtk_widget_set_hexpand(draw, TRUE); gtk_widget_set_vexpand(draw, TRUE);
    gtk_widget_set_can_target(draw, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(app_state.desktop_container), draw);
    app_state.physics.overlay_draw = draw;
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(draw), editor_draw_overlay, NULL, NULL);

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(editor_on_drag_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(editor_on_drag_update), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(editor_on_drag_end), NULL);
    gtk_widget_add_controller(draw, GTK_EVENT_CONTROLLER(drag));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(editor_on_motion), NULL);
    gtk_widget_add_controller(draw, motion);

    GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_COPY);
    g_signal_connect(drop, "enter", G_CALLBACK(editor_on_dnd_enter), NULL);
    g_signal_connect(drop, "leave", G_CALLBACK(editor_on_dnd_leave), NULL);
    g_signal_connect(drop, "drop", G_CALLBACK(editor_on_drop), NULL);
    gtk_widget_add_controller(draw, GTK_EVENT_CONTROLLER(drop));

    GtkGesture *rclick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), 3);
    g_signal_connect(rclick, "pressed", G_CALLBACK(on_right_click), NULL);
    gtk_widget_add_controller(app_state.desktop_container, GTK_EVENT_CONTROLLER(rclick));

    GSimpleActionGroup *action_group = g_simple_action_group_new();
    GSimpleAction *act_edit = g_simple_action_new("edit_scenes", NULL);
    g_signal_connect(act_edit, "activate", G_CALLBACK(on_edit_scenes_activate), NULL);
    g_action_map_add_action(G_ACTION_MAP(action_group), G_ACTION(act_edit));

    GSimpleAction *act_wall = g_simple_action_new("change_wallpaper", NULL);
    g_signal_connect(act_wall, "activate", G_CALLBACK(on_change_wallpaper_activate), NULL);
    g_action_map_add_action(G_ACTION_MAP(action_group), G_ACTION(act_wall));
    
    gtk_widget_insert_action_group(root_overlay, "desktop", G_ACTION_GROUP(action_group));
    g_object_unref(action_group);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
    gtk_widget_add_controller(root_overlay, key);

    app_state.drawer_revealer = create_drawer();
    gtk_widget_set_halign(app_state.drawer_revealer, GTK_ALIGN_END);
    gtk_overlay_add_overlay(GTK_OVERLAY(root_overlay), app_state.drawer_revealer);

    app_state.wallpaper_revealer = create_wallpaper_shelf();
    gtk_widget_set_valign(app_state.wallpaper_revealer, GTK_ALIGN_END);
    gtk_widget_set_halign(app_state.wallpaper_revealer, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(app_state.wallpaper_revealer, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(root_overlay), app_state.wallpaper_revealer);

    app_state.show_drawer_btn = gtk_button_new_from_icon_name("view-more-symbolic");
    gtk_widget_add_css_class(app_state.show_drawer_btn, "show-drawer-btn");
    gtk_widget_set_halign(app_state.show_drawer_btn, GTK_ALIGN_END);
    gtk_widget_set_valign(app_state.show_drawer_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(app_state.show_drawer_btn, 20);
    gtk_widget_set_visible(app_state.show_drawer_btn, FALSE);
    g_signal_connect(app_state.show_drawer_btn, "clicked", G_CALLBACK(on_show_drawer_clicked), NULL);
    gtk_overlay_add_overlay(GTK_OVERLAY(root_overlay), app_state.show_drawer_btn);

    app_state.trash_revealer = create_trash_bin();
    gtk_widget_set_halign(app_state.trash_revealer, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(app_state.trash_revealer, GTK_ALIGN_END);
    gtk_overlay_add_overlay(GTK_OVERLAY(root_overlay), app_state.trash_revealer);

    g_timeout_add(50, on_startup_snap, NULL);
    update_ui_visibility();

    dbus_owner_id = g_bus_own_name(G_BUS_TYPE_SESSION, "com.meismeric.SurfaceDesk", G_BUS_NAME_OWNER_FLAGS_NONE, on_bus_acquired, NULL, NULL, NULL, NULL);

    return root_overlay;
}