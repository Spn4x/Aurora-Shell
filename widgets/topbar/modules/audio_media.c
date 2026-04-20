#include "audio_internal.h"
#include <math.h>
#include <stdlib.h>

typedef struct {
    GtkWidget *main_stack;
    char *local_path;
} ArtDownloadData;

// --- Hover Crossfade ---

static gboolean hover_fade_tick(gpointer data) {
    AudioModule *mod = data;
    double target = mod->is_hovered ? 0.0 : 1.0;
    
    mod->hover_alpha += (target - mod->hover_alpha) * 0.15;
    
    gtk_widget_set_opacity(mod->visualizer_da, mod->hover_alpha);
    gtk_widget_set_opacity(mod->art_container, 1.0 - mod->hover_alpha);
    
    if (fabs(mod->hover_alpha - target) < 0.01) {
        mod->hover_alpha = target;
        gtk_widget_set_opacity(mod->visualizer_da, mod->hover_alpha);
        gtk_widget_set_opacity(mod->art_container, 1.0 - mod->hover_alpha);
        mod->hover_anim_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void on_media_hover_enter(GtkEventControllerMotion *c, double x, double y, gpointer data) {
    (void)c; (void)x; (void)y;
    AudioModule *mod = data;
    mod->is_hovered = TRUE;
    if (mod->hover_anim_id == 0) mod->hover_anim_id = g_timeout_add(16, hover_fade_tick, mod);
}

static void on_media_hover_leave(GtkEventControllerMotion *c, gpointer data) {
    (void)c;
    AudioModule *mod = data;
    mod->is_hovered = FALSE;
    if (mod->hover_anim_id == 0) mod->hover_anim_id = g_timeout_add(16, hover_fade_tick, mod);
}

// --- 300ms Coherent Waveform Generator ---
static gboolean vis_update_tick(gpointer data) {
    AudioModule *mod = data;
    
    if (mod->is_playing) {
        // Adjusted for 300ms: Phase moves 0.24 per tick + small random jitter
        mod->vis_phase += 0.24 + ((rand() % 12) / 100.0); 
        
        for(int i = 0; i < NUM_VIS_BARS; i++) {
            double x = (double)i / (NUM_VIS_BARS - 1);
            double window = sin(x * G_PI); // Bell curve: Peaks in middle
            
            double wave1 = sin(mod->vis_phase + x * 8.0);
            double wave2 = cos((mod->vis_phase * 1.4) - (x * 12.0));
            double combined_wave = (wave1 + wave2 + 2.0) / 4.0;
            
            double eq_jitter = ((rand() % 100) / 100.0) * 0.25; 
            double final_height = (combined_wave * 0.75 + eq_jitter) * window;
            
            if (final_height < 0.05) final_height = 0.05;
            if (final_height > 1.0) final_height = 1.0;
            
            mod->vis_currents[i] = final_height;
        }
    } else {
        for(int i = 0; i < NUM_VIS_BARS; i++) mod->vis_currents[i] = 0.05;
    }

    if (gtk_widget_get_mapped(mod->visualizer_da) && mod->hover_alpha > 0.01) {
        gtk_widget_queue_draw(mod->visualizer_da);
    }
    
    return G_SOURCE_CONTINUE;
}

static void draw_visualizer(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data) {
    AudioModule *module = data;
    
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(area));
    GdkRGBA accent_color;
    gdk_rgba_parse(&accent_color, "#8aadf4");
    if (!gtk_style_context_lookup_color(context, "custom-accent", &accent_color)) {
        gtk_style_context_lookup_color(context, "theme_selected_bg_color", &accent_color);
    }
#pragma GCC diagnostic pop

    gdk_cairo_set_source_rgba(cr, &accent_color);

    double spacing = 3.0;
    double total_spacing = spacing * (NUM_VIS_BARS + 1);
    double bar_width = (width - total_spacing) / NUM_VIS_BARS;
    if (bar_width < 1.0) bar_width = 1.0;

    double start_x = spacing;

    for(int i = 0; i < NUM_VIS_BARS; i++) {
        double bar_h = (height * 0.6) * module->vis_currents[i];
        if (bar_h < 3.0) bar_h = 3.0; 
        
        double x = start_x + i * (bar_width + spacing);
        double y = (height - bar_h) / 2.0;
        
        cairo_rounded_rectangle(cr, x, y, bar_width, bar_h, 2.0);
        cairo_fill(cr);
    }
}

static void draw_media_bg(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data) {
    (void)data;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(area));
    GdkRGBA bg_color;
    gdk_rgba_parse(&bg_color, "#3E3E41");
    gtk_style_context_lookup_color(context, "theme_unfocused_color", &bg_color);
#pragma GCC diagnostic pop

    gdk_cairo_set_source_rgba(cr, &bg_color);
    cairo_rounded_rectangle(cr, 0, 0, width, height, 8.0);
    cairo_fill(cr);
}

// --- MPRIS and UI Setup ---

static void ensure_cache_dir_exists(const char *path) {
    g_autofree char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
}

static void on_curl_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GSubprocess *proc = G_SUBPROCESS(source);
    ArtDownloadData *data = (ArtDownloadData*)user_data;
    GError *error = NULL;
    gboolean success = g_subprocess_wait_check_finish(proc, res, &error);
    
    AudioModule *module = g_object_get_data(G_OBJECT(data->main_stack), "module-state");
    if (module && success && g_file_test(data->local_path, G_FILE_TEST_EXISTS)) {
        gtk_picture_set_filename(module->album_art_image, data->local_path);
    }
    
    if (error) g_error_free(error);
    g_object_unref(data->main_stack); 
    g_free(data->local_path);
    g_free(data);
}

void update_mpris_view(AudioModule *module) {
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
                GSubprocess *proc = g_subprocess_launcher_spawn(launcher, NULL, "curl", "-s", "-L", "-o", cache_path, art_url, NULL);
                if (proc) {
                    ArtDownloadData *data = g_new(ArtDownloadData, 1);
                    data->main_stack = g_object_ref(module->main_stack);
                    data->local_path = g_strdup(cache_path);
                    g_subprocess_wait_async(proc, NULL, on_curl_finished, data);
                    g_object_unref(proc);
                }
                g_object_unref(launcher);
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

static gboolean delayed_art_update_callback(gpointer user_data) {
    AudioModule *module = user_data;
    module->art_timer_id = 0;
    if (module && module->mpris_proxy) update_mpris_view(module);
    return G_SOURCE_REMOVE;
}

static void update_mpris_state(AudioModule *state) {
    if (!state || !state->mpris_proxy) {
        state->is_media_active = FALSE;
        state->is_playing = FALSE;
        update_combined_state(state);
        return;
    }

    g_autoptr(GVariant) metadata_var = g_dbus_proxy_get_cached_property(state->mpris_proxy, "Metadata");
    g_autoptr(GVariant) status_var = g_dbus_proxy_get_cached_property(state->mpris_proxy, "PlaybackStatus");

    const char *status = status_var ? g_variant_get_string(status_var, NULL) : "Stopped";
    state->is_playing = (g_strcmp0(status, "Playing") == 0);

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

            if (state->art_timer_id > 0) g_source_remove(state->art_timer_id);
            state->art_timer_id = g_timeout_add(ART_LOAD_DELAY_MS, delayed_art_update_callback, state);
            
            trigger_temporary_view(state, "media_view");
        }
    } else {
        state->is_media_active = FALSE;
    }

    update_combined_state(state);
}

static void on_mpris_properties_changed(GDBusProxy *proxy, GVariant *c, const gchar *const *i, gpointer d) {
    (void)proxy; (void)c; (void)i; update_mpris_state((AudioModule*)d);
}

static void connect_to_mpris_player(const gchar *name, gpointer user_data) {
    AudioModule *module = user_data;
    if (module->mpris_proxy) g_object_unref(module->mpris_proxy); 
    
    module->mpris_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL, name, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", NULL, NULL);
    if (module->mpris_proxy) {
        g_signal_connect(module->mpris_proxy, "g-properties-changed", G_CALLBACK(on_mpris_properties_changed), module);
    }
    update_mpris_state(module);
}

static void on_media_manager_changed(GDBusProxy *proxy, GVariant *c, const gchar *const *i, gpointer d) {
    (void)c; (void)i;
    AudioModule *module = d;
    g_autoptr(GVariant) active_var = g_dbus_proxy_get_cached_property(proxy, "ActivePlayer");
    const char *active_player = active_var ? g_variant_get_string(active_var, NULL) : "";

    if (active_player && strlen(active_player) > 0) {
        connect_to_mpris_player(active_player, module);
    } else {
        g_clear_object(&module->mpris_proxy);
        update_mpris_state(module);
    }
}

static void on_media_manager_ready(GObject *source, GAsyncResult *res, gpointer user_data) {
    (void)source; AudioModule *module = user_data;
    module->media_manager_proxy = g_dbus_proxy_new_for_bus_finish(res, NULL);
    if (module->media_manager_proxy) {
        g_signal_connect(module->media_manager_proxy, "g-properties-changed", G_CALLBACK(on_media_manager_changed), module);
        on_media_manager_changed(module->media_manager_proxy, NULL, NULL, module); 
    }
}

void init_media_ui(AudioModule *module) {
    module->media_overlay = gtk_overlay_new();
    
    GtkWidget *bg_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(bg_da, 220, 28);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(bg_da), draw_media_bg, module, NULL);
    gtk_overlay_set_child(GTK_OVERLAY(module->media_overlay), bg_da);

    module->art_container = gtk_overlay_new();
    gtk_widget_set_opacity(module->art_container, 0.0); 
    
    module->album_art_image = GTK_PICTURE(gtk_picture_new());
    gtk_widget_add_css_class(GTK_WIDGET(module->album_art_image), "album-art-bg");
    gtk_picture_set_content_fit(module->album_art_image, GTK_CONTENT_FIT_COVER);
    gtk_overlay_set_child(GTK_OVERLAY(module->art_container), GTK_WIDGET(module->album_art_image));

    GtkWidget *scrim_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(scrim_box, "media-scrim");
    gtk_overlay_add_overlay(GTK_OVERLAY(module->art_container), scrim_box);

    module->song_title_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(module->song_title_label), "song-title-overlay");
    gtk_label_set_ellipsize(module->song_title_label, PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(GTK_WIDGET(module->song_title_label), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(module->song_title_label), GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(module->art_container), GTK_WIDGET(module->song_title_label));

    gtk_overlay_add_overlay(GTK_OVERLAY(module->media_overlay), module->art_container);

    module->visualizer_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(module->visualizer_da, 220, 28);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(module->visualizer_da), draw_visualizer, module, NULL);
    
    module->hover_alpha = 1.0;
    gtk_widget_set_opacity(module->visualizer_da, 1.0);
    gtk_overlay_add_overlay(GTK_OVERLAY(module->media_overlay), module->visualizer_da);

    GtkEventController *hover = gtk_event_controller_motion_new();
    g_signal_connect(hover, "enter", G_CALLBACK(on_media_hover_enter), module);
    g_signal_connect(hover, "leave", G_CALLBACK(on_media_hover_leave), module);
    gtk_widget_add_controller(module->media_overlay, hover);

    module->vis_phase = 0.0;
    for (int i = 0; i < NUM_VIS_BARS; i++) {
        module->vis_currents[i] = 0.1;
    }
    
    // CHANGED TO 500ms
    module->vis_timer_id = g_timeout_add(500, vis_update_tick, module);
}

void setup_media_dbus(AudioModule *module) {
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "com.meismeric.aurora.MediaManager", "/com/meismeric/aurora/MediaManager", "com.meismeric.aurora.MediaManager",
        NULL, on_media_manager_ready, module);
}

void cleanup_media_module(AudioModule *module) {
    if (module->art_timer_id > 0) g_source_remove(module->art_timer_id);
    if (module->vis_timer_id > 0) g_source_remove(module->vis_timer_id);
    if (module->hover_anim_id > 0) g_source_remove(module->hover_anim_id);
    g_clear_object(&module->mpris_proxy);
    g_clear_object(&module->media_manager_proxy);
    g_free(module->current_track_signature);
    g_free(module->last_art_url);
}