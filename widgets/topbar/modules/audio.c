#include <gtk/gtk.h>
#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib/gstdio.h>
#include "audio.h"
// REMOVED: #include "popover_anim.h"

#define BANNER_DURATION_MS 5000
#define ART_LOAD_DELAY_MS 500

// --- Type Definitions ---
typedef struct _AudioModule AudioModule;

// FIXED: Added 'name' to struct so we can switch by string instead of ID
typedef struct {
    uint32_t id;
    gchar *name;
    gchar *description;
} AudioSink;

struct _AudioModule {
    GtkWidget *main_stack;
    
    // UI Components
    GtkWidget *bt_drawing_area;
    GtkWidget *media_overlay;
    GtkPicture *album_art_image;
    GtkLabel  *song_title_label;
    GtkWidget *popover;
    GtkWidget *sink_list_box;

    // D-Bus Proxies & Watchers
    GDBusObjectManager *bluez_manager;
    GDBusProxy *mpris_proxy;
    guint mpris_name_watcher_id;

    // State Flags
    gboolean is_connected;
    gboolean is_powered;
    gboolean is_media_active;
    
    // Logic Data
    gchar *device_name;
    int battery_percentage;
    gchar *current_track_signature;
    gchar *last_art_url;
    
    // State Management
    gchar *preferred_view; 
    
    // Timers
    guint banner_timer_id; 
    guint art_timer_id;
};

// --- Forward Declarations ---
static void update_combined_state(AudioModule *module);
static void update_bluetooth_status(AudioModule *module);
static void update_mpris_state(AudioModule *state);
static void update_mpris_view(AudioModule *module);
static void draw_audio_bt(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);

// --- Helpers ---

static void cairo_rounded_rectangle(cairo_t *cr, double x, double y, double width, double height, double radius) {
    if (width <= 0 || height <= 0) return;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, 1.5 * G_PI);
    cairo_arc(cr, x + width - radius, y + radius, radius, 1.5 * G_PI, 2.0 * G_PI);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, 0.5 * G_PI);
    cairo_arc(cr, x + radius, y + height - radius, radius, 0.5 * G_PI, G_PI);
    cairo_close_path(cr);
}

// FIXED: Free the name string
static void audio_sink_free(gpointer data) {
    AudioSink *sink = (AudioSink*)data;
    g_free(sink->name);
    g_free(sink->description);
    g_free(sink);
}

static const char* get_glyph_for_sink(const gchar* description) {
    if (!description) return "󰗟"; 
    g_autofree gchar *lower_desc = g_ascii_strdown(description, -1);
    if (strstr(lower_desc, "hdmi")) return "󰡁";
    if (strstr(lower_desc, "usb")) return "󰘳";
    if (strstr(lower_desc, "bluez") || strstr(lower_desc, "headphone") || strstr(lower_desc, "headset") || strstr(lower_desc, "buds")) return "󰋋";
    if (strstr(lower_desc, "speaker") || strstr(lower_desc, "built-in")) return "󰕾";
    return "󰗟";
}

// --- Logic: Temporary Banners ---

static gboolean on_banner_timeout(gpointer user_data) {
    AudioModule *module = user_data;
    if (module) module->banner_timer_id = 0;
    update_combined_state(module);
    return G_SOURCE_REMOVE;
}

static void trigger_temporary_view(AudioModule *module, const char *view_name) {
    if (module->banner_timer_id > 0) {
        g_source_remove(module->banner_timer_id);
        module->banner_timer_id = 0;
    }
    
    gtk_stack_set_visible_child_name(GTK_STACK(module->main_stack), view_name);
    gtk_widget_set_visible(module->main_stack, TRUE);
    
    module->banner_timer_id = g_timeout_add(BANNER_DURATION_MS, on_banner_timeout, module);
}

// --- Logic: Delayed Art Loading ---

static gboolean delayed_art_update_callback(gpointer user_data) {
    AudioModule *module = user_data;
    module->art_timer_id = 0;
    if (module && module->mpris_proxy) {
        update_mpris_view(module);
    }
    return G_SOURCE_REMOVE;
}

// --- Logic: Remote Art Downloading ---

typedef struct {
    AudioModule *module;
    char *local_path;
} ArtDownloadData;

static void on_curl_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GSubprocess *proc = G_SUBPROCESS(source);
    ArtDownloadData *data = (ArtDownloadData*)user_data;
    
    GError *error = NULL;
    if (g_subprocess_wait_check_finish(proc, res, &error)) {
        if (g_file_test(data->local_path, G_FILE_TEST_EXISTS)) {
            gtk_picture_set_filename(data->module->album_art_image, data->local_path);
        }
    } else {
        if(error) g_error_free(error);
    }
    
    g_free(data->local_path);
    g_free(data);
}

static void ensure_cache_dir_exists(const char *path) {
    g_autofree char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
}

// --- The Core Logic: State Management ---

static void update_combined_state(AudioModule *module) {
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

    const char *target = NULL;
    if (module->preferred_view) {
        target = module->preferred_view;
    } else {
        if (module->is_media_active) target = "media_view";
        else if (module->is_connected) target = "bluetooth_view";
    }

    if (target) {
        if (g_strcmp0(target, "media_view") == 0) update_mpris_view(module);
        gtk_stack_set_visible_child_name(GTK_STACK(module->main_stack), target);
    }
}

// --- MPRIS (Media) Logic ---

static void update_mpris_view(AudioModule *module) {
    if (!module || !module->mpris_proxy) return;
    
    g_autoptr(GVariant) metadata_var = g_dbus_proxy_get_cached_property(module->mpris_proxy, "Metadata");
    if (!metadata_var) return;

    g_autoptr(GVariantDict) dict = g_variant_dict_new(metadata_var);
    const gchar *title = NULL, *art_url = NULL;
    g_variant_dict_lookup(dict, "xesam:title", "&s", &title);
    g_variant_dict_lookup(dict, "mpris:artUrl", "&s", &art_url);

    gtk_label_set_text(module->song_title_label, title ? title : "Unknown Track");

    if (g_strcmp0(art_url, module->last_art_url) != 0) {
        g_free(module->last_art_url);
        module->last_art_url = g_strdup(art_url);
        
        if (!art_url) {
            gtk_picture_set_filename(module->album_art_image, NULL);
            return;
        }

        if (g_str_has_prefix(art_url, "file://")) {
            g_autofree gchar *path = g_filename_from_uri(art_url, NULL, NULL);
            if (g_file_test(path, G_FILE_TEST_EXISTS)) {
                gtk_picture_set_filename(module->album_art_image, path);
            } else {
                gtk_picture_set_filename(module->album_art_image, NULL);
            }
        } 
        else if (g_str_has_prefix(art_url, "http://") || g_str_has_prefix(art_url, "https://")) {
            g_autofree char *checksum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, art_url, -1);
            g_autofree char *cache_path = g_build_filename(g_get_user_cache_dir(), "aurora-shell", "art", checksum, NULL);
            
            if (g_file_test(cache_path, G_FILE_TEST_EXISTS)) {
                gtk_picture_set_filename(module->album_art_image, cache_path);
            } else {
                gtk_picture_set_filename(module->album_art_image, NULL);
                ensure_cache_dir_exists(cache_path);
                GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
                GError *err = NULL;
                GSubprocess *proc = g_subprocess_launcher_spawn(launcher, &err, "curl", "-s", "-L", "-o", cache_path, art_url, NULL);
                if (proc) {
                    ArtDownloadData *data = g_new(ArtDownloadData, 1);
                    data->module = module;
                    data->local_path = g_strdup(cache_path);
                    g_subprocess_wait_async(proc, NULL, on_curl_finished, data);
                    g_object_unref(proc);
                } else {
                    if (err) g_error_free(err);
                }
                g_object_unref(launcher);
            }
        } 
        else {
            GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(GTK_WIDGET(module->album_art_image)));
            GtkIconPaintable *paintable = gtk_icon_theme_lookup_icon(
                icon_theme, "audio-x-generic", NULL, 48, 1, GTK_TEXT_DIR_NONE, GTK_ICON_LOOKUP_FORCE_REGULAR);
            if (paintable) {
                gtk_picture_set_paintable(module->album_art_image, GDK_PAINTABLE(paintable));
                g_object_unref(paintable);
            } else {
                gtk_picture_set_filename(module->album_art_image, NULL);
            }
        }
    }
}

static gchar* create_track_signature(GVariantDict *dict) {
    const char *title = NULL, *artist = NULL;
    gchar **artists = NULL;
    if (!dict) return NULL;
    g_variant_dict_lookup(dict, "xesam:title", "&s", &title);
    g_variant_dict_lookup(dict, "xesam:artist", "^as", &artists);
    if (artists && artists[0]) artist = artists[0];
    gchar *signature = (title) ? g_strdup_printf("%s - %s", artist ? artist : "Unknown", title) : NULL;
    g_strfreev(artists);
    return signature;
}

static void update_mpris_state(AudioModule *state) {
    if (!state || !state->mpris_proxy) {
        state->is_media_active = FALSE;
        update_combined_state(state);
        return;
    }

    g_autoptr(GVariant) metadata_var = g_dbus_proxy_get_cached_property(state->mpris_proxy, "Metadata");
    g_autoptr(GVariant) status_var = g_dbus_proxy_get_cached_property(state->mpris_proxy, "PlaybackStatus");

    const char *status = status_var ? g_variant_get_string(status_var, NULL) : "Stopped";
    g_autoptr(GVariantDict) dict = metadata_var ? g_variant_dict_new(metadata_var) : NULL;
    
    g_autofree gchar *new_signature = create_track_signature(dict);
    gboolean has_track = (new_signature != NULL && strlen(new_signature) > 1);
    
    if (g_strcmp0(status, "Stopped") != 0 && has_track) {
        state->is_media_active = TRUE;
        
        if (g_strcmp0(new_signature, state->current_track_signature) != 0) {
            g_free(state->current_track_signature);
            state->current_track_signature = g_strdup(new_signature);
            
            g_free(state->last_art_url);
            state->last_art_url = NULL; 
            
            gtk_picture_set_filename(state->album_art_image, NULL);
            update_mpris_view(state);
            trigger_temporary_view(state, "media_view");

            if (state->art_timer_id > 0) g_source_remove(state->art_timer_id);
            state->art_timer_id = g_timeout_add(ART_LOAD_DELAY_MS, delayed_art_update_callback, state);
        }
    } else {
        state->is_media_active = FALSE;
    }

    update_combined_state(state);
}

static void on_mpris_properties_changed(GDBusProxy *proxy, GVariant *changed, const gchar *const *invalidated, gpointer user_data) {
    (void)proxy; (void)changed; (void)invalidated;
    update_mpris_state((AudioModule*)user_data);
}

// --- Bluetooth Logic ---

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
                    if (b_var) { 
                        module->battery_percentage = g_variant_get_byte(b_var); 
                    } else {
                        g_autoptr(GDBusInterface) bat_iface = g_dbus_object_get_interface(obj, "org.bluez.Battery1");
                        if (bat_iface) {
                            g_autoptr(GVariant) b1_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(bat_iface), "Percentage");
                            if (b1_var) module->battery_percentage = g_variant_get_byte(b1_var);
                        }
                    }
                    break; 
                }
            }
        }
    }

    gtk_widget_queue_draw(module->bt_drawing_area);
    
    if (!was_connected && module->is_connected) {
        trigger_temporary_view(module, "bluetooth_view");
    } else {
        update_combined_state(module);
    }
}

static void draw_audio_bt(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    AudioModule *module = (AudioModule *)user_data;
    GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(area));
    GdkRGBA bg_color, fg_color, accent_color;

    gdk_rgba_parse(&bg_color, "#3E3E41");
    gdk_rgba_parse(&fg_color, "#ffffff");
    gdk_rgba_parse(&accent_color, "#8aadf4");

    gtk_style_context_lookup_color(context, "theme_unfocused_color", &bg_color);
    gtk_style_context_lookup_color(context, "theme_fg_color", &fg_color);
    gtk_style_context_lookup_color(context, "theme_selected_bg_color", &accent_color);

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
    gchar *text = NULL;

    if (!module->is_powered) text = g_strdup("󰂲 Off");
    else if (module->is_connected) {
        if (module->battery_percentage >= 0) text = g_strdup_printf("󰋋 %d%% %s", module->battery_percentage, module->device_name);
        else text = g_strdup_printf("󰋋 %s", module->device_name);
    } else text = g_strdup("󰂯 Disconnected");

    pango_layout_set_text(layout, text, -1); g_free(text);
    int text_w, text_h; pango_layout_get_pixel_size(layout, &text_w, &text_h);
    gdk_cairo_set_source_rgba(cr, &fg_color); 
    cairo_move_to(cr, (width - text_w) / 2.0, (height - text_h) / 2.0);
    pango_cairo_show_layout(cr, layout); g_object_unref(layout);
}

// --- Interaction ---

static gboolean on_scroll(GtkEventControllerScroll* controller, double dx, double dy, gpointer user_data) {
    (void)controller; (void)dx;
    AudioModule *module = user_data;
    
    if (module->banner_timer_id > 0) {
        g_source_remove(module->banner_timer_id);
        module->banner_timer_id = 0;
    }

    if (dy > 0) { 
        if (module->is_connected) {
            g_free(module->preferred_view);
            module->preferred_view = g_strdup("bluetooth_view");
            update_combined_state(module);
        }
    } else if (dy < 0) { 
        if (module->is_media_active) {
            g_free(module->preferred_view);
            module->preferred_view = g_strdup("media_view");
            update_mpris_view(module);
            update_combined_state(module);
        }
    }
    return TRUE;
}

// FIXED: Uses 'pactl set-default-sink NAME'
static void on_sink_button_clicked(GtkButton *button, gpointer data) {
    AudioSink *sink = (AudioSink*)data;
    g_autofree gchar *command = g_strdup_printf("pactl set-default-sink '%s'", sink->name);
    system(command);
    GtkPopover* popover = GTK_POPOVER(gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER));
    if (popover) gtk_popover_popdown(popover);
}

// FIXED: Parses 'pactl list sinks' to get stable Names
static void update_sink_list_ui(AudioModule *module) {
    // 1. Clear old children
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(module->sink_list_box))) { 
        gtk_box_remove(GTK_BOX(module->sink_list_box), child); 
    }
    
    // 2. Get current default sink name
    char default_sink[256] = {0};
    FILE *fp_def = popen("pactl get-default-sink", "r");
    if (fp_def) {
        if (fgets(default_sink, sizeof(default_sink), fp_def)) {
            g_strstrip(default_sink);
        }
        pclose(fp_def);
    }
    
    // 3. Populate List (Using pactl to get stable names)
    FILE *fp = popen("pactl list sinks | grep -E 'Name:|Description:' | awk 'NR%2{printf $2 \"|\"} NR%2==0{$1=\"\"; print substr($0,2)}'", "r");
    if (!fp) return;
    
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *name = strtok(line, "|");
        char *desc = strtok(NULL, "\n");
        
        if (name && desc) {
            g_strstrip(name);
            g_strstrip(desc);
            
            AudioSink *sink = g_new0(AudioSink, 1);
            sink->name = g_strdup(name);
            sink->description = g_strdup(desc);
            
            gboolean is_default = (g_strcmp0(name, default_sink) == 0);

            // Build UI
            GtkWidget *button = gtk_button_new(); gtk_widget_add_css_class(button, "sink-button"); gtk_widget_add_css_class(button, "flat");
            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); gtk_button_set_child(GTK_BUTTON(button), box);
            GtkWidget *glyph_label = gtk_label_new(get_glyph_for_sink(sink->description)); gtk_widget_add_css_class(glyph_label, "glyph-label");
            GtkWidget *desc_label = gtk_label_new(sink->description); gtk_label_set_xalign(GTK_LABEL(desc_label), 0.0); gtk_widget_set_hexpand(desc_label, TRUE);
            gtk_box_append(GTK_BOX(box), glyph_label); gtk_box_append(GTK_BOX(box), desc_label);
            if (is_default) gtk_widget_add_css_class(button, "active-sink");
            
            g_object_set_data_full(G_OBJECT(button), "sink-data", sink, audio_sink_free);
            g_signal_connect(button, "clicked", G_CALLBACK(on_sink_button_clicked), sink);

            gtk_box_append(GTK_BOX(module->sink_list_box), button);
        }
    }
    pclose(fp);
}

static void on_module_clicked(GtkGestureClick *gesture, int n, double x, double y, gpointer user_data) {
    (void)gesture; (void)n; (void)x; (void)y;
    AudioModule *module = (AudioModule *)user_data;
    update_sink_list_ui(module);
    gtk_popover_popup(GTK_POPOVER(module->popover));
}

// --- Setup & Cleanup ---

static void connect_to_mpris_player(const gchar *name, gpointer user_data) {
    AudioModule *module = user_data;
    if (module->mpris_proxy) g_object_unref(module->mpris_proxy); 
    
    g_print("Audio Module: Connecting to MPRIS Player: %s\n", name);
    g_autoptr(GError) error = NULL;
    module->mpris_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL, name, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", NULL, &error);
    
    if (error) { g_warning("Failed to create proxy: %s", error->message); return; }
    
    g_signal_connect(module->mpris_proxy, "g-properties-changed", G_CALLBACK(on_mpris_properties_changed), module);
    update_mpris_state(module);
}

static void on_name_owner_changed(GDBusConnection *c,const gchar *s,const gchar *o,const gchar *i,const gchar *sig,GVariant *p,gpointer d) {
    (void)c; (void)s; (void)o; (void)i; (void)sig;
    AudioModule *module = d; const gchar *name, *old_owner, *new_owner;
    g_variant_get(p, "(sss)", &name, &old_owner, &new_owner);
    if (g_str_has_prefix(name, "org.mpris.MediaPlayer2.")) {
        if (new_owner && *new_owner) { connect_to_mpris_player(name, module); }
        else if (module->mpris_proxy && g_strcmp0(g_dbus_proxy_get_name(module->mpris_proxy), name) == 0) {
            g_clear_object(&module->mpris_proxy);
            update_mpris_state(module); 
        }
    }
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

static void audio_module_cleanup(gpointer data) {
    AudioModule *module = (AudioModule *)data;
    if (module->banner_timer_id > 0) g_source_remove(module->banner_timer_id);
    if (module->art_timer_id > 0) g_source_remove(module->art_timer_id);
    if (module->bluez_manager) g_object_unref(module->bluez_manager);
    if (module->mpris_proxy) g_object_unref(module->mpris_proxy);
    if (module->mpris_name_watcher_id > 0) g_bus_unwatch_name(module->mpris_name_watcher_id);
    g_free(module->device_name);
    g_free(module->current_track_signature);
    g_free(module->preferred_view);
    g_free(module->last_art_url);
    g_free(module);
}

// --- Construction ---

GtkWidget* create_audio_module() {
    AudioModule *module = g_new0(AudioModule, 1);
    module->device_name = g_strdup("...");
    module->battery_percentage = -1;

    module->main_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(module->main_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_UP_DOWN);
    gtk_stack_set_transition_duration(GTK_STACK(module->main_stack), 400);
    
    gtk_widget_add_css_class(module->main_stack, "audio-module");
    gtk_widget_add_css_class(module->main_stack, "module");

    // Bluetooth View
    module->bt_drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(module->bt_drawing_area, 220, 28);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(module->bt_drawing_area), draw_audio_bt, module, NULL);
    gtk_stack_add_named(GTK_STACK(module->main_stack), module->bt_drawing_area, "bluetooth_view");
    
    // Media View
    module->media_overlay = gtk_overlay_new();
    GtkWidget *sizing_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(sizing_box, 220, 28);
    gtk_overlay_set_child(GTK_OVERLAY(module->media_overlay), sizing_box);

    module->album_art_image = GTK_PICTURE(gtk_picture_new());
    gtk_widget_add_css_class(GTK_WIDGET(module->album_art_image), "album-art-bg");
    gtk_picture_set_content_fit(module->album_art_image, GTK_CONTENT_FIT_COVER);
    gtk_overlay_add_overlay(GTK_OVERLAY(module->media_overlay), GTK_WIDGET(module->album_art_image));

    GtkWidget *scrim_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(scrim_box, "media-scrim");
    gtk_widget_set_halign(scrim_box, GTK_ALIGN_FILL); gtk_widget_set_valign(scrim_box, GTK_ALIGN_FILL);
    gtk_overlay_add_overlay(GTK_OVERLAY(module->media_overlay), scrim_box);

    module->song_title_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(module->song_title_label), "song-title-overlay");
    gtk_label_set_ellipsize(module->song_title_label, PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(GTK_WIDGET(module->song_title_label), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(module->song_title_label), GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(module->media_overlay), GTK_WIDGET(module->song_title_label));
    
    gtk_stack_add_named(GTK_STACK(module->main_stack), module->media_overlay, "media_view");
    
    // Popover (No animation)
    module->popover = gtk_popover_new();
    gtk_widget_set_parent(module->popover, module->main_stack);
    module->sink_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top(module->sink_list_box, 5);
    gtk_widget_set_margin_bottom(module->sink_list_box, 5);
    gtk_widget_add_css_class(module->sink_list_box, "sink-list-popover");
    gtk_popover_set_child(GTK_POPOVER(module->popover), module->sink_list_box);

    // Input
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_module_clicked), module);
    gtk_widget_add_controller(module->main_stack, GTK_EVENT_CONTROLLER(click));

    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), module);
    gtk_widget_add_controller(module->main_stack, scroll);

    // Init
    g_dbus_object_manager_client_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, "org.bluez", "/", NULL, NULL, NULL, NULL, (GAsyncReadyCallback)on_bluez_manager_created, module);
    
    g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    module->mpris_name_watcher_id = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_name_owner_changed, module, NULL);
    
    g_autoptr(GVariant) result = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if (result) {
        g_autoptr(GVariantIter) iter; g_variant_get(result, "(as)", &iter); gchar *name;
        while (g_variant_iter_loop(iter, "s", &name)) {
            if (g_str_has_prefix(name, "org.mpris.MediaPlayer2.")) {
                connect_to_mpris_player(name, module);
                break;
            }
        }
    }

    gtk_widget_set_visible(module->main_stack, FALSE);

    g_object_set_data_full(G_OBJECT(module->main_stack), "module-state", module, audio_module_cleanup);
    return module->main_stack;
}