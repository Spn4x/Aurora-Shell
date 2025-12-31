#include "mpris_widget.h"
#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib/gstdio.h>

#define ART_LOAD_DELAY_MS 500
#define PROGRESS_UPDATE_MS 1000

typedef struct _AudioModule AudioModule;

struct _AudioModule {
    GtkWidget *root_overlay;
    
    // UI Components
    GtkPicture *album_art_bg;
    GtkImage   *app_icon;
    GtkLabel   *song_title_label;
    GtkLabel   *artist_label; 
    GtkWidget  *play_button;
    GtkWidget  *play_icon;
    GtkRange   *timeline;
    GtkLabel   *position_label;
    GtkLabel   *duration_label;

    // D-Bus
    GDBusProxy *mpris_proxy;
    guint mpris_name_watcher_id;

    // State
    gboolean is_playing;
    gchar *current_track_signature;
    gchar *last_art_url;
    gint64 track_length; 
    
    // Timers
    guint art_timer_id;
    guint progress_timer_id;
};

static void load_mpris_css() {
    const char *css = 
        /* CARD CONTAINER */
        ".mpris-card { "
        "   border-radius: 12px; " 
        "   margin: 0 6px 12px 6px; " 
        "   background-color: #3E3E41; "
        "} "
        /* SCRIM */
        ".media-scrim { "
        "   background: linear-gradient(90deg, rgba(0,0,0,0.85) 0%, rgba(0,0,0,0.5) 60%, rgba(0,0,0,0.1) 100%); "
        "} "
        /* TEXT */
        ".mpris-title { "
        "   font-weight: 800; font-size: 1.05em; color: white; "
        "   text-shadow: 0 1px 2px rgba(0,0,0,0.8); "
        "} "
        ".mpris-artist { "
        "   font-weight: 600; font-size: 0.9em; color: #e0e0e0; "
        "   text-shadow: 0 1px 2px rgba(0,0,0,0.8); "
        "} "
        ".mpris-time { "
        "   font-size: 0.75em; color: #3E3E41; font-weight: bold; "
        "   text-shadow: 0 1px 2px rgba(0,0,0,0.8); "
        "} "
        
        /* SECONDARY CONTROLS (Scoped) */
        ".mpris-card .mpris-controls button { "
        "   background: transparent; box-shadow: none; border: none; "
        "   color: #eeeeee; padding: 0; "
        "   min-height: 24px; min-width: 24px; "
        "} "
        ".mpris-card .mpris-controls button:hover { background: #3E3E41; border-radius: 99px; } "
        
        /* PLAY BUTTON (Scoped) */
        ".mpris-card .mpris-play-btn { "
        "   background-color: #3E3E41; " /* Clean White */
        "   color: white; " /* Match Card BG for Icon */
        "   border-radius: 14px; "
        "   min-width: 48px; min-height: 48px; "
        "   padding: 0; margin: 12px; "
        "   border: none; box-shadow: none; "
        "} "
        ".mpris-card .mpris-play-btn:hover { filter: brightness(0.9); } "
        
        /* TIMELINE (Strictly Scoped to .mpris-card) */
        ".mpris-card scale trough { "
        "   background-color: rgba(255,255,255,0.2); "
        "   min-height: 4px; border-radius: 2px; "
        "} "
        ".mpris-card scale highlight { "
        "   background-color: #3E3E41; " 
        "   min-height: 4px; border-radius: 2px; "
        "} "
        ".mpris-card scale slider { "
        "   min-height: 0; min-width: 0; margin: -6px; "
        "   background: transparent; box-shadow: none; "
        "} ";
    
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

// --- Helpers ---
static void ensure_cache_dir_exists(const char *path) {
    g_autofree char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
}

static char* format_time(gint64 microseconds) {
    if (microseconds <= 0) return g_strdup("00:00");
    gint64 seconds = microseconds / 1000000;
    gint64 minutes = seconds / 60;
    seconds = seconds % 60;
    return g_strdup_printf("%02ld:%02ld", minutes, seconds);
}

// --- Callbacks ---
static void on_prev_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; AudioModule *m = user_data;
    if(m->mpris_proxy) g_dbus_proxy_call(m->mpris_proxy, "Previous", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}
static void on_next_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; AudioModule *m = user_data;
    if(m->mpris_proxy) g_dbus_proxy_call(m->mpris_proxy, "Next", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}
static void on_play_pause_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; AudioModule *m = user_data;
    if(m->mpris_proxy) g_dbus_proxy_call(m->mpris_proxy, "PlayPause", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

// --- Logic: Progress Bar Update ---
static void on_get_position_reply(GObject *source, GAsyncResult *res, gpointer user_data) {
    AudioModule *m = user_data;
    GDBusProxy *proxy = G_DBUS_PROXY(source);
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) result = g_dbus_proxy_call_finish(proxy, res, &error);
    
    if (result) {
        GVariant *val_v = NULL;
        g_variant_get(result, "(v)", &val_v);
        gint64 position = g_variant_get_int64(val_v);
        g_variant_unref(val_v);
        
        if (m->track_length > 0) {
            gtk_range_set_range(m->timeline, 0, (double)m->track_length);
            gtk_range_set_value(m->timeline, (double)position);
            
            g_autofree char *pos_txt = format_time(position);
            gtk_label_set_text(m->position_label, pos_txt);
        } else {
            gtk_range_set_value(m->timeline, 0);
            gtk_label_set_text(m->position_label, "00:00");
        }
    }
}

static gboolean update_progress(gpointer user_data) {
    AudioModule *m = user_data;
    if (!m->mpris_proxy || !m->is_playing) return G_SOURCE_CONTINUE;

    g_dbus_proxy_call(m->mpris_proxy,
        "org.freedesktop.DBus.Properties.Get",
        g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", "Position"),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_get_position_reply, m);

    return G_SOURCE_CONTINUE;
}

// --- Art Download ---
typedef struct { AudioModule *module; char *local_path; } ArtDownloadData;
static void on_curl_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GSubprocess *proc = G_SUBPROCESS(source);
    ArtDownloadData *data = (ArtDownloadData*)user_data;
    GError *error = NULL;
    if (g_subprocess_wait_check_finish(proc, res, &error)) {
        if (g_file_test(data->local_path, G_FILE_TEST_EXISTS)) {
            gtk_picture_set_filename(data->module->album_art_bg, data->local_path);
        }
    }
    if(error) g_error_free(error);
    g_free(data->local_path); g_free(data);
}

static gboolean delayed_art_update_callback(gpointer user_data) {
    AudioModule *module = user_data;
    module->art_timer_id = 0;
    if (!module->mpris_proxy) return G_SOURCE_REMOVE;
    g_autoptr(GVariant) metadata = g_dbus_proxy_get_cached_property(module->mpris_proxy, "Metadata");
    if (!metadata) return G_SOURCE_REMOVE;
    g_autoptr(GVariantDict) dict = g_variant_dict_new(metadata);
    const gchar *art_url = NULL;
    g_variant_dict_lookup(dict, "mpris:artUrl", "&s", &art_url);

    if (g_strcmp0(art_url, module->last_art_url) != 0) {
        g_free(module->last_art_url);
        module->last_art_url = g_strdup(art_url);
        if (!art_url) { gtk_picture_set_filename(module->album_art_bg, NULL); return G_SOURCE_REMOVE; }

        if (g_str_has_prefix(art_url, "file://")) {
            g_autofree gchar *path = g_filename_from_uri(art_url, NULL, NULL);
            gtk_picture_set_filename(module->album_art_bg, path);
        } else if (g_str_has_prefix(art_url, "http")) {
            g_autofree char *checksum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, art_url, -1);
            g_autofree char *cache_path = g_build_filename(g_get_user_cache_dir(), "aurora-shell", "art", checksum, NULL);
            if (g_file_test(cache_path, G_FILE_TEST_EXISTS)) {
                gtk_picture_set_filename(module->album_art_bg, cache_path);
            } else {
                ensure_cache_dir_exists(cache_path);
                GSubprocessLauncher *l = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
                GSubprocess *proc = g_subprocess_launcher_spawn(l, NULL, "curl", "-s", "-L", "-o", cache_path, art_url, NULL);
                if (proc) {
                    ArtDownloadData *d = g_new(ArtDownloadData, 1); d->module = module; d->local_path = g_strdup(cache_path);
                    g_subprocess_wait_async(proc, NULL, on_curl_finished, d);
                    g_object_unref(proc);
                }
                g_object_unref(l);
            }
        }
    }
    return G_SOURCE_REMOVE;
}

static void update_state(AudioModule *module) {
    if (!module->mpris_proxy) { gtk_widget_set_visible(module->root_overlay, FALSE); return; }
    g_autoptr(GVariant) status_var = g_dbus_proxy_get_cached_property(module->mpris_proxy, "PlaybackStatus");
    const char *status = status_var ? g_variant_get_string(status_var, NULL) : "Stopped";
    if (g_strcmp0(status, "Stopped") == 0) { gtk_widget_set_visible(module->root_overlay, FALSE); module->is_playing = FALSE; return; }

    gtk_widget_set_visible(module->root_overlay, TRUE);
    module->is_playing = (g_strcmp0(status, "Playing") == 0);
    gtk_image_set_from_icon_name(GTK_IMAGE(module->play_icon), module->is_playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");

    g_autoptr(GVariant) metadata = g_dbus_proxy_get_cached_property(module->mpris_proxy, "Metadata");
    if (metadata) {
        g_autoptr(GVariantDict) dict = g_variant_dict_new(metadata);
        const char *title = NULL; gchar **artists = NULL;
        g_variant_dict_lookup(dict, "xesam:title", "&s", &title);
        g_variant_dict_lookup(dict, "xesam:artist", "^as", &artists);
        
        if (!g_variant_dict_lookup(dict, "mpris:length", "x", &module->track_length)) {
            guint64 len_u = 0;
            if (g_variant_dict_lookup(dict, "mpris:length", "t", &len_u)) module->track_length = (gint64)len_u;
            else module->track_length = 0;
        }

        gtk_label_set_text(module->song_title_label, title ? title : "Unknown");
        gtk_label_set_text(module->artist_label, (artists && artists[0]) ? artists[0] : "");
        
        g_autofree char *len_txt = format_time(module->track_length);
        gtk_label_set_text(module->duration_label, len_txt);
        
        g_strfreev(artists);

        const char *bus_name = g_dbus_proxy_get_name(module->mpris_proxy);
        if (strstr(bus_name, "spotify")) gtk_image_set_from_icon_name(module->app_icon, "spotify");
        else if (strstr(bus_name, "firefox")) gtk_image_set_from_icon_name(module->app_icon, "firefox");
        else gtk_image_set_from_icon_name(module->app_icon, "audio-x-generic");

        if (g_strcmp0(title, module->current_track_signature) != 0) {
            g_free(module->current_track_signature);
            module->current_track_signature = g_strdup(title);
            if (module->art_timer_id > 0) g_source_remove(module->art_timer_id);
            module->art_timer_id = g_timeout_add(ART_LOAD_DELAY_MS, delayed_art_update_callback, module);
        }
    }
}

static void on_mpris_changed(GDBusProxy *p, GVariant *c, const gchar *const *i, gpointer d) { (void)p; (void)c; (void)i; update_state((AudioModule*)d); }
static void connect_player(const gchar *name, gpointer data) {
    AudioModule *m = data;
    if(m->mpris_proxy) g_object_unref(m->mpris_proxy);
    m->mpris_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL, name, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", NULL, NULL);
    if(m->mpris_proxy) { g_signal_connect(m->mpris_proxy, "g-properties-changed", G_CALLBACK(on_mpris_changed), m); update_state(m); }
}
static void on_name_owner_changed(GDBusConnection *c,const gchar *s,const gchar *o,const gchar *i,const gchar *sig,GVariant *p,gpointer d) {
    (void)c; (void)s; (void)o; (void)i; (void)sig; const gchar *name, *new_owner;
    g_variant_get(p, "(sss)", &name, NULL, &new_owner);
    if(g_str_has_prefix(name, "org.mpris.MediaPlayer2.") && new_owner && *new_owner) connect_player(name, d);
}
static void cleanup(gpointer data) {
    AudioModule *m = data;
    if(m->art_timer_id) g_source_remove(m->art_timer_id);
    if(m->progress_timer_id) g_source_remove(m->progress_timer_id);
    if(m->mpris_proxy) g_object_unref(m->mpris_proxy);
    g_free(m->current_track_signature); g_free(m->last_art_url); g_free(m);
}

GtkWidget* create_mpris_widget(void) {
    load_mpris_css();
    AudioModule *m = g_new0(AudioModule, 1);

    m->root_overlay = gtk_overlay_new();
    gtk_widget_set_size_request(m->root_overlay, -1, 115); 
    gtk_widget_set_vexpand(m->root_overlay, FALSE); 
    gtk_widget_add_css_class(m->root_overlay, "mpris-card");
    gtk_widget_set_overflow(m->root_overlay, GTK_OVERFLOW_HIDDEN);
    
    g_object_set_data_full(G_OBJECT(m->root_overlay), "module", m, cleanup);

    // 1. Sizing Box
    GtkWidget *sizing_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(sizing_box, -1, 115);
    gtk_widget_set_vexpand(sizing_box, FALSE);
    gtk_overlay_set_child(GTK_OVERLAY(m->root_overlay), sizing_box);

    // 2. Art
    m->album_art_bg = GTK_PICTURE(gtk_picture_new());
    gtk_widget_add_css_class(GTK_WIDGET(m->album_art_bg), "album-art-bg");
    gtk_picture_set_can_shrink(m->album_art_bg, TRUE);
    gtk_picture_set_content_fit(m->album_art_bg, GTK_CONTENT_FIT_COVER);
    gtk_overlay_add_overlay(GTK_OVERLAY(m->root_overlay), GTK_WIDGET(m->album_art_bg));

    // 3. Scrim
    GtkWidget *scrim = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(scrim, "media-scrim");
    gtk_overlay_add_overlay(GTK_OVERLAY(m->root_overlay), scrim);

    // 4. Content
    GtkWidget *main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_overlay_add_overlay(GTK_OVERLAY(m->root_overlay), main_hbox);

    // --- LEFT ---
    GtkWidget *left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(left_vbox, TRUE);
    gtk_widget_set_margin_start(left_vbox, 16);
    gtk_widget_set_margin_top(left_vbox, 12);
    gtk_widget_set_margin_bottom(left_vbox, 12);
    
    // Title/Icon
    GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    m->app_icon = GTK_IMAGE(gtk_image_new_from_icon_name("audio-x-generic"));
    gtk_image_set_pixel_size(m->app_icon, 16);
    gtk_widget_set_opacity(GTK_WIDGET(m->app_icon), 0.9);
    
    m->song_title_label = GTK_LABEL(gtk_label_new("No Media"));
    gtk_widget_add_css_class(GTK_WIDGET(m->song_title_label), "mpris-title");
    gtk_label_set_ellipsize(m->song_title_label, PANGO_ELLIPSIZE_END);
    
    gtk_box_append(GTK_BOX(title_box), GTK_WIDGET(m->app_icon));
    gtk_box_append(GTK_BOX(title_box), GTK_WIDGET(m->song_title_label));

    m->artist_label = GTK_LABEL(gtk_label_new("-"));
    gtk_widget_add_css_class(GTK_WIDGET(m->artist_label), "mpris-artist");
    gtk_widget_set_halign(GTK_WIDGET(m->artist_label), GTK_ALIGN_START);
    gtk_label_set_ellipsize(m->artist_label, PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(GTK_WIDGET(m->artist_label), 24);

    // Timeline Row
    GtkWidget *timeline_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(timeline_row, 8);
    
    m->position_label = GTK_LABEL(gtk_label_new("00:00"));
    gtk_widget_add_css_class(GTK_WIDGET(m->position_label), "mpris-time");
    
    m->duration_label = GTK_LABEL(gtk_label_new("00:00"));
    gtk_widget_add_css_class(GTK_WIDGET(m->duration_label), "mpris-time");

    m->timeline = GTK_RANGE(gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, NULL));
    gtk_widget_set_hexpand(GTK_WIDGET(m->timeline), TRUE);
    gtk_scale_set_draw_value(GTK_SCALE(m->timeline), FALSE);

    gtk_box_append(GTK_BOX(timeline_row), GTK_WIDGET(m->position_label)); 
    gtk_box_append(GTK_BOX(timeline_row), GTK_WIDGET(m->timeline));
    gtk_box_append(GTK_BOX(timeline_row), GTK_WIDGET(m->duration_label));

    // Controls Row
    GtkWidget *ctrl_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(ctrl_row, 2);
    gtk_widget_add_css_class(ctrl_row, "mpris-controls");

    GtkWidget *prev = gtk_button_new_from_icon_name("media-skip-backward-symbolic");
    g_signal_connect(prev, "clicked", G_CALLBACK(on_prev_clicked), m);
    GtkWidget *next = gtk_button_new_from_icon_name("media-skip-forward-symbolic");
    g_signal_connect(next, "clicked", G_CALLBACK(on_next_clicked), m);
    GtkWidget *shuf = gtk_button_new_from_icon_name("media-playlist-shuffle-symbolic");
    gtk_widget_set_opacity(shuf, 0.6);

    gtk_box_append(GTK_BOX(ctrl_row), prev);
    gtk_box_append(GTK_BOX(ctrl_row), next);
    gtk_box_append(GTK_BOX(ctrl_row), shuf);

    gtk_box_append(GTK_BOX(left_vbox), title_box);
    gtk_box_append(GTK_BOX(left_vbox), GTK_WIDGET(m->artist_label));
    gtk_box_append(GTK_BOX(left_vbox), timeline_row);
    gtk_box_append(GTK_BOX(left_vbox), ctrl_row);

    // --- RIGHT ---
    GtkWidget *right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(right_vbox, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(right_vbox, GTK_ALIGN_CENTER);

    m->play_button = gtk_button_new();
    gtk_widget_add_css_class(m->play_button, "mpris-play-btn"); 
    
    m->play_icon = gtk_image_new_from_icon_name("media-playback-start-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(m->play_icon), 24);
    gtk_button_set_child(GTK_BUTTON(m->play_button), m->play_icon);
    g_signal_connect(m->play_button, "clicked", G_CALLBACK(on_play_pause_clicked), m);

    gtk_box_append(GTK_BOX(right_vbox), m->play_button);

    gtk_box_append(GTK_BOX(main_hbox), left_vbox);
    gtk_box_append(GTK_BOX(main_hbox), right_vbox);

    // --- Init ---
    g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    m->mpris_name_watcher_id = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_name_owner_changed, m, NULL);
    g_autoptr(GVariant) res = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if(res) {
        g_autoptr(GVariantIter) iter; g_variant_get(res, "(as)", &iter); gchar *name;
        while(g_variant_iter_loop(iter, "s", &name)) {
            if(g_str_has_prefix(name, "org.mpris.MediaPlayer2.")) { connect_player(name, m); break; }
        }
    }
    m->progress_timer_id = g_timeout_add(PROGRESS_UPDATE_MS, update_progress, m);
    gtk_widget_set_visible(m->root_overlay, FALSE);
    return m->root_overlay;
}