#include "audio_internal.h"
#include "audio.h"

// --- Utilities ---

void cairo_rounded_rectangle(cairo_t *cr, double x, double y, double width, double height, double radius) {
    if (width <= 0 || height <= 0) return;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, 1.5 * G_PI);
    cairo_arc(cr, x + width - radius, y + radius, radius, 1.5 * G_PI, 2.0 * G_PI);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, 0.5 * G_PI);
    cairo_arc(cr, x + radius, y + height - radius, radius, 0.5 * G_PI, G_PI);
    cairo_close_path(cr);
}

static gboolean on_banner_timeout(gpointer user_data) {
    AudioModule *module = user_data;
    if (module) module->banner_timer_id = 0;
    update_combined_state(module);
    return G_SOURCE_REMOVE;
}

void trigger_temporary_view(AudioModule *module, const char *view_name) {
    if (module->banner_timer_id > 0) {
        g_source_remove(module->banner_timer_id);
        module->banner_timer_id = 0;
    }
    gtk_stack_set_visible_child_name(GTK_STACK(module->main_stack), view_name);
    gtk_widget_set_visible(module->main_stack, TRUE);
    module->banner_timer_id = g_timeout_add(BANNER_DURATION_MS, on_banner_timeout, module);
}

void update_combined_state(AudioModule *module) {
    if (!module || module->banner_timer_id > 0) return;

    if (!module->is_media_active && !module->is_connected) {
        gtk_widget_set_visible(module->main_stack, FALSE);
        g_clear_pointer(&module->preferred_view, g_free);
        return;
    }

    gtk_widget_set_visible(module->main_stack, TRUE);

    if (g_strcmp0(module->preferred_view, "media_view") == 0 && !module->is_media_active) {
        g_clear_pointer(&module->preferred_view, g_free);
    } else if (g_strcmp0(module->preferred_view, "bluetooth_view") == 0 && !module->is_connected) {
        g_clear_pointer(&module->preferred_view, g_free);
    }

    const char *target = module->preferred_view ? module->preferred_view : 
                        (module->is_media_active ? "media_view" : "bluetooth_view");

    if (g_strcmp0(target, "media_view") == 0) update_mpris_view(module);
    gtk_stack_set_visible_child_name(GTK_STACK(module->main_stack), target);
}

static gboolean on_scroll(GtkEventControllerScroll* controller, double dx, double dy, gpointer user_data) {
    (void)controller; (void)dx;
    AudioModule *module = user_data;
    
    if (module->banner_timer_id > 0) {
        g_source_remove(module->banner_timer_id);
        module->banner_timer_id = 0;
    }

    if (dy > 0 && module->is_connected) { 
        g_free(module->preferred_view);
        module->preferred_view = g_strdup("bluetooth_view");
        update_combined_state(module);
    } else if (dy < 0 && module->is_media_active) { 
        g_free(module->preferred_view);
        module->preferred_view = g_strdup("media_view");
        update_mpris_view(module);
        update_combined_state(module);
    }
    return TRUE;
}

// --- Bluetooth UI & Logic ---

static void draw_audio_bt(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    AudioModule *module = (AudioModule *)user_data;
    GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(area));
    GdkRGBA bg_color, fg_color, accent_color;

    gdk_rgba_parse(&bg_color, "#3E3E41");
    gdk_rgba_parse(&fg_color, "#ffffff");
    gdk_rgba_parse(&accent_color, "#8aadf4");

    gtk_style_context_lookup_color(context, "theme_unfocused_color", &bg_color);
    gtk_style_context_lookup_color(context, "theme_fg_color", &fg_color);
    if (!gtk_style_context_lookup_color(context, "custom-accent", &accent_color)) {
        gtk_style_context_lookup_color(context, "theme_selected_bg_color", &accent_color);
    }

    gdk_cairo_set_source_rgba(cr, &bg_color); 
    cairo_rounded_rectangle(cr, 0, 0, width, height, 8.0); 
    cairo_fill(cr);

    if (module->is_connected && module->battery_percentage >= 0) {
        cairo_save(cr); 
        cairo_rounded_rectangle(cr, 0, 0, width, height, 8.0); 
        cairo_clip(cr);
        double bar_width = width * ((double)module->battery_percentage / 100.0);
        gdk_cairo_set_source_rgba(cr, &accent_color); 
        cairo_rectangle(cr, 0, 0, bar_width, height); 
        cairo_fill(cr);
        cairo_restore(cr);
    }
    
    PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(area), NULL);
    gchar *text = (!module->is_powered) ? g_strdup("󰂲 Off") : 
                  (module->is_connected ? (module->battery_percentage >= 0 ? g_strdup_printf("󰋋 %d%% %s", module->battery_percentage, module->device_name) : g_strdup_printf("󰋋 %s", module->device_name)) : g_strdup("󰂯 Disconnected"));

    pango_layout_set_text(layout, text, -1); g_free(text);
    int text_w, text_h; pango_layout_get_pixel_size(layout, &text_w, &text_h);
    gdk_cairo_set_source_rgba(cr, &fg_color); 
    cairo_move_to(cr, (width - text_w) / 2.0, (height - text_h) / 2.0);
    pango_cairo_show_layout(cr, layout); g_object_unref(layout);
}

static void update_bluetooth_status(AudioModule *module) {
    if (!module->bluez_manager) return;

    gboolean was_connected = module->is_connected; 
    module->is_connected = FALSE;
    module->is_powered = FALSE;
    module->battery_percentage = -1;
    g_free(module->device_name);
    module->device_name = g_strdup("Unknown");

    g_autoptr(GDBusInterface) adapter = g_dbus_object_manager_get_interface(module->bluez_manager, "/org/bluez/hci0", "org.bluez.Adapter1");
    if (adapter) {
        g_autoptr(GVariant) p_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(adapter), "Powered");
        if (p_var) module->is_powered = g_variant_get_boolean(p_var);
    }

    if (module->is_powered) {
        g_autoptr(GList) objects = g_dbus_object_manager_get_objects(module->bluez_manager);
        for (GList *l = objects; l != NULL; l = l->next) {
            GDBusObject *obj = G_DBUS_OBJECT(l->data);
            g_autoptr(GDBusInterface) device = g_dbus_object_get_interface(obj, "org.bluez.Device1");
            
            if (device) {
                g_autoptr(GVariant) c_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(device), "Connected");
                if (c_var && g_variant_get_boolean(c_var)) {
                    module->is_connected = TRUE;
                    g_autoptr(GVariant) n_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(device), "Alias");
                    if (!n_var) n_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(device), "Name");
                    if (n_var) { 
                        g_free(module->device_name); 
                        module->device_name = g_variant_dup_string(n_var, NULL); 
                    }
                    g_autoptr(GVariant) b_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(device), "BatteryPercentage");
                    if (b_var) module->battery_percentage = g_variant_get_byte(b_var); 
                    break; 
                }
            }
        }
    }

    gtk_widget_queue_draw(module->bt_drawing_area);
    if (!was_connected && module->is_connected) trigger_temporary_view(module, "bluetooth_view");
    else update_combined_state(module);
}

static void on_bluez_properties_changed(GDBusObjectManagerClient *m, GDBusObjectProxy *o, GDBusProxy *i, GVariant *c, const gchar *const *iv, gpointer d) { 
    (void)m; (void)o; (void)i; (void)c; (void)iv; 
    update_bluetooth_status((AudioModule*)d); 
}

static void on_bluez_manager_created(GObject *s, GAsyncResult *r, gpointer d) { 
    (void)s; AudioModule *m = d; g_autoptr(GError) e = NULL; 
    m->bluez_manager = g_dbus_object_manager_client_new_for_bus_finish(r, &e); 
    if (e) return; 
    update_bluetooth_status(m); 
    g_signal_connect(m->bluez_manager, "interface-proxy-properties-changed", G_CALLBACK(on_bluez_properties_changed), m); 
}

// --- Device Helpers & Popover Handlers ---

typedef struct {
    uint32_t id;
    gchar *name;
    gchar *description;
    gboolean is_source;
} AudioDevice;

static void audio_device_free(gpointer data) {
    AudioDevice *dev = (AudioDevice*)data;
    g_free(dev->name);
    g_free(dev->description);
    g_free(dev);
}

static const char* get_glyph_for_device(const gchar* description, gboolean is_source) {
    if (!description) return is_source ? "󰍬" : "󰗟"; 
    g_autofree gchar *lower_desc = g_ascii_strdown(description, -1);
    
    if (is_source) {
        if (strstr(lower_desc, "bluez") || strstr(lower_desc, "headset")) return "󰋎";
        if (strstr(lower_desc, "usb")) return "󰍬"; 
        if (strstr(lower_desc, "internal") || strstr(lower_desc, "built-in")) return "󰍬"; 
        return "󰍬";
    } else {
        if (strstr(lower_desc, "hdmi")) return "󰡁";
        if (strstr(lower_desc, "usb")) return "󰘳";
        if (strstr(lower_desc, "bluez") || strstr(lower_desc, "headphone") || strstr(lower_desc, "headset") || strstr(lower_desc, "buds")) return "󰋋";
        if (strstr(lower_desc, "speaker") || strstr(lower_desc, "built-in")) return "󰕾";
        return "󰗟";
    }
}

static gchar* get_friendly_player_name(const gchar* bus_name) {
    const char *prefix = "org.mpris.MediaPlayer2.";
    if (!g_str_has_prefix(bus_name, prefix)) return g_strdup(bus_name);
    const char *start = bus_name + strlen(prefix);
    char *dot = strchr(start, '.');
    gchar *name = dot ? g_strndup(start, dot - start) : g_strdup(start);
    if (name[0]) name[0] = g_ascii_toupper(name[0]);
    return name;
}

static const char* get_glyph_for_player(const gchar* name) {
    g_autofree gchar *lower = g_ascii_strdown(name, -1);
    if (strstr(lower, "spotify")) return "";
    if (strstr(lower, "firefox") || strstr(lower, "chrome") || strstr(lower, "brave") || strstr(lower, "edge") || strstr(lower, "opera")) return "󰈹";
    if (strstr(lower, "vlc")) return "󰕼";
    return "󰎆"; 
}

static void on_device_button_clicked(GtkButton *button, gpointer data) {
    AudioDevice *dev = (AudioDevice*)data;
    g_autofree gchar *command = NULL;
    if (dev->is_source) {
        command = g_strdup_printf("pactl set-default-source '%s'", dev->name);
    } else {
        command = g_strdup_printf("pactl set-default-sink '%s'", dev->name);
    }
    system(command);
    GtkPopover* popover = GTK_POPOVER(gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER));
    if (popover) gtk_popover_popdown(popover);
}

static void update_device_list_ui(AudioModule *module, gboolean is_source) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(module->popover_list_box))) { 
        gtk_box_remove(GTK_BOX(module->popover_list_box), child); 
    }
    
    GtkWidget *title = gtk_label_new(is_source ? "Select Microphone" : "Select Speaker");
    gtk_widget_add_css_class(title, "title-3");
    gtk_widget_set_margin_bottom(title, 8);
    gtk_widget_set_margin_top(title, 4);
    gtk_box_append(GTK_BOX(module->popover_list_box), title);

    char default_dev[256] = {0};
    const char *cmd_default = is_source ? "pactl get-default-source" : "pactl get-default-sink";
    FILE *fp_def = popen(cmd_default, "r");
    if (fp_def) {
        if (fgets(default_dev, sizeof(default_dev), fp_def)) {
            g_strstrip(default_dev);
        }
        pclose(fp_def);
    }
    
    const char *cmd_list = is_source ? 
        "pactl list sources | grep -E 'Name:|Description:' | awk 'NR%2{printf $2 \"|\"} NR%2==0{$1=\"\"; print substr($0,2)}'" : 
        "pactl list sinks | grep -E 'Name:|Description:' | awk 'NR%2{printf $2 \"|\"} NR%2==0{$1=\"\"; print substr($0,2)}'";

    FILE *fp = popen(cmd_list, "r");
    if (!fp) return;
    
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *name = strtok(line, "|");
        char *desc = strtok(NULL, "\n");
        
        if (name && desc) {
            g_strstrip(name);
            g_strstrip(desc);
            if (is_source && g_str_has_suffix(name, ".monitor")) continue;
            
            AudioDevice *dev = g_new0(AudioDevice, 1);
            dev->name = g_strdup(name);
            dev->description = g_strdup(desc);
            dev->is_source = is_source;
            gboolean is_default = (g_strcmp0(name, default_dev) == 0);

            GtkWidget *button = gtk_button_new(); 
            gtk_widget_add_css_class(button, "sink-button"); 
            gtk_widget_add_css_class(button, "flat");
            
            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); 
            gtk_button_set_child(GTK_BUTTON(button), box);
            
            GtkWidget *glyph_label = gtk_label_new(get_glyph_for_device(dev->description, is_source)); 
            gtk_widget_add_css_class(glyph_label, "glyph-label");
            
            GtkWidget *desc_label = gtk_label_new(dev->description); 
            gtk_label_set_xalign(GTK_LABEL(desc_label), 0.0); 
            gtk_widget_set_hexpand(desc_label, TRUE);
            
            gtk_box_append(GTK_BOX(box), glyph_label); 
            gtk_box_append(GTK_BOX(box), desc_label);
            
            if (is_default) gtk_widget_add_css_class(button, "active-sink");
            
            g_object_set_data_full(G_OBJECT(button), "device-data", dev, audio_device_free);
            g_signal_connect(button, "clicked", G_CALLBACK(on_device_button_clicked), dev);

            gtk_box_append(GTK_BOX(module->popover_list_box), button);
        }
    }
    pclose(fp);
}

static void on_player_button_clicked(GtkButton *button, gpointer data) {
    (void)data;
    gchar *bus_name = g_object_get_data(G_OBJECT(button), "bus-name");
    AudioModule *module = g_object_get_data(G_OBJECT(button), "module-ref");
    
    if (module && module->media_manager_proxy) {
        g_dbus_proxy_call(module->media_manager_proxy, "SelectPlayer", 
            g_variant_new("(s)", bus_name ? bus_name : ""), 
            G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
    GtkPopover* popover = GTK_POPOVER(gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER));
    if (popover) gtk_popover_popdown(popover);
}

static void update_player_list_ui(AudioModule *module) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(module->popover_list_box))) { 
        gtk_box_remove(GTK_BOX(module->popover_list_box), child); 
    }
    
    GtkWidget *title = gtk_label_new("Select Media Player");
    gtk_widget_add_css_class(title, "title-3");
    gtk_widget_set_margin_bottom(title, 8);
    gtk_widget_set_margin_top(title, 4);
    gtk_box_append(GTK_BOX(module->popover_list_box), title);

    g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    g_autoptr(GVariant) result = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames", NULL, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    
    if (result) {
        g_autoptr(GVariantIter) iter;
        g_variant_get(result, "(as)", &iter);
        gchar *name;
        gboolean found_any = FALSE;
        
        const gchar *current_active = NULL;
        if (module->mpris_proxy) current_active = g_dbus_proxy_get_name(module->mpris_proxy);

        while (g_variant_iter_loop(iter, "s", &name)) {
            if (g_str_has_prefix(name, "org.mpris.MediaPlayer2.") && !strstr(name, "playerctld")) {
                found_any = TRUE;
                gchar *friendly_name = get_friendly_player_name(name);
                gboolean is_active = (current_active && g_strcmp0(current_active, name) == 0);

                GtkWidget *button = gtk_button_new(); 
                gtk_widget_add_css_class(button, "sink-button"); 
                gtk_widget_add_css_class(button, "flat");
                
                GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); 
                gtk_button_set_child(GTK_BUTTON(button), box);
                
                GtkWidget *glyph_label = gtk_label_new(get_glyph_for_player(friendly_name)); 
                gtk_widget_add_css_class(glyph_label, "glyph-label");
                
                GtkWidget *desc_label = gtk_label_new(friendly_name); 
                gtk_label_set_xalign(GTK_LABEL(desc_label), 0.0); 
                gtk_widget_set_hexpand(desc_label, TRUE);
                
                gtk_box_append(GTK_BOX(box), glyph_label); 
                gtk_box_append(GTK_BOX(box), desc_label);
                
                if (is_active) gtk_widget_add_css_class(button, "active-sink");
                
                g_object_set_data_full(G_OBJECT(button), "bus-name", g_strdup(name), g_free);
                g_object_set_data(G_OBJECT(button), "module-ref", module);
                g_signal_connect(button, "clicked", G_CALLBACK(on_player_button_clicked), NULL);

                gtk_box_append(GTK_BOX(module->popover_list_box), button);
                g_free(friendly_name);
            }
        }
        
        if (!found_any) {
            GtkWidget *none_lbl = gtk_label_new("No players running");
            gtk_widget_set_margin_bottom(none_lbl, 4);
            gtk_widget_set_opacity(none_lbl, 0.7);
            gtk_box_append(GTK_BOX(module->popover_list_box), none_lbl);
        }

        GtkWidget *clear_btn = gtk_button_new_with_label("Auto-Select");
        gtk_widget_add_css_class(clear_btn, "flat");
        gtk_widget_add_css_class(clear_btn, "sink-button");
        g_object_set_data_full(G_OBJECT(clear_btn), "bus-name", g_strdup(""), g_free);
        g_object_set_data(G_OBJECT(clear_btn), "module-ref", module);
        g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_player_button_clicked), NULL);
        gtk_box_append(GTK_BOX(module->popover_list_box), clear_btn);
    }
}

static void on_module_clicked(GtkGestureClick *gesture, int n, double x, double y, gpointer user_data) {
    (void)n; (void)x; (void)y;
    AudioModule *module = (AudioModule *)user_data;
    
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    
    // Right Click (Secondary)
    if (button == GDK_BUTTON_SECONDARY || button == 3) {
        update_player_list_ui(module);
    } else {
        // Middle Click or Left Click
        gboolean is_source = (button == GDK_BUTTON_MIDDLE || button == 2);
        update_device_list_ui(module, is_source);
    }
    
    gtk_popover_popup(GTK_POPOVER(module->popover));
}

// --- Lifecycle ---

static void audio_module_cleanup(gpointer data) {
    AudioModule *module = (AudioModule *)data;
    if (module->banner_timer_id > 0) g_source_remove(module->banner_timer_id);
    cleanup_media_module(module);
    g_clear_object(&module->bluez_manager);
    g_free(module->device_name);
    g_free(module->preferred_view);
    g_free(module);
}

GtkWidget* create_audio_module() {
    AudioModule *module = g_new0(AudioModule, 1);
    module->device_name = g_strdup("...");
    module->battery_percentage = -1;

    module->main_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(module->main_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_UP_DOWN);
    gtk_stack_set_transition_duration(GTK_STACK(module->main_stack), 400);
    gtk_widget_add_css_class(module->main_stack, "audio-module");
    gtk_widget_add_css_class(module->main_stack, "module");

    // Bluetooth Side
    module->bt_drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(module->bt_drawing_area, 220, 28);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(module->bt_drawing_area), draw_audio_bt, module, NULL);
    gtk_stack_add_named(GTK_STACK(module->main_stack), module->bt_drawing_area, "bluetooth_view");
    
    // Media Side (Call external setup)
    init_media_ui(module);
    gtk_stack_add_named(GTK_STACK(module->main_stack), module->media_overlay, "media_view");
    
    // Popover
    module->popover = gtk_popover_new();
    gtk_widget_set_parent(module->popover, module->main_stack);
    module->popover_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_popover_set_child(GTK_POPOVER(module->popover), module->popover_list_box);

    // CLICK HANDLER RESTORED
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0); // Captures left, middle, AND right click
    g_signal_connect(click, "pressed", G_CALLBACK(on_module_clicked), module);
    gtk_widget_add_controller(module->main_stack, GTK_EVENT_CONTROLLER(click));

    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), module);
    gtk_widget_add_controller(module->main_stack, scroll);

    setup_media_dbus(module);
    g_dbus_object_manager_client_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, "org.bluez", "/", NULL, NULL, NULL, NULL, (GAsyncReadyCallback)on_bluez_manager_created, module);
    
    gtk_widget_set_visible(module->main_stack, FALSE);
    g_object_set_data_full(G_OBJECT(module->main_stack), "module-state", module, audio_module_cleanup);
    
    return module->main_stack;
}