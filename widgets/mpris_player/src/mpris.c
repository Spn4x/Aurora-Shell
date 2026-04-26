#include "mpris.h"
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <math.h>

#define LYRICS_OFFSET_ADJUSTMENT_STEP 50

static void update_display_metadata(MprisPopoutState *state, GVariantDict *dict);
static void update_playback_status_ui(MprisPopoutState *state, const gchar *status);
static void on_properties_changed(GDBusProxy *proxy, GVariant *changed_properties, const gchar *const *invalidated_properties, gpointer user_data);
static void update_mpris_state(MprisPopoutState *state);
static void update_offset_label(MprisPopoutState *state);
static void show_toast(MprisPopoutState *state, const gchar *message);
static gboolean hide_toast_label(gpointer user_data);

typedef struct {
    GtkWidget *root_widget;
    char *local_path;
} ArtDownloadData;

static void ensure_cache_dir_exists(const char *path) {
    g_autofree char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
}

static void free_popout_state(gpointer data) {
    MprisPopoutState *state = data;
    if (!state) return;
    
    destroy_lyrics_view(state->lyrics_view_state);
    
    if (state->player_proxy) {
        if (state->properties_changed_id > 0) g_signal_handler_disconnect(state->player_proxy, state->properties_changed_id);
        g_clear_object(&state->player_proxy);
    }
    
    if (state->manager_proxy) {
        if (state->manager_properties_id > 0) g_signal_handler_disconnect(state->manager_proxy, state->manager_properties_id);
    }

    g_free(state->last_art_url);
    g_free(state->bus_name);
    g_free(state);
}

static gboolean hide_toast_label(gpointer user_data) {
    GtkWidget *label = GTK_WIDGET(user_data);
    if (GTK_IS_WIDGET(label)) { gtk_widget_set_visible(label, FALSE); }
    return G_SOURCE_REMOVE;
}

static void show_toast(MprisPopoutState *state, const gchar *message) {
    if (!state || !state->toast_label) return;
    gtk_label_set_text(GTK_LABEL(state->toast_label), message);
    gtk_widget_set_visible(state->toast_label, TRUE);
    g_timeout_add_seconds(2, hide_toast_label, state->toast_label);
}

static void on_curl_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GSubprocess *proc = G_SUBPROCESS(source);
    ArtDownloadData *data = (ArtDownloadData*)user_data;
    
    GError *error = NULL;
    gboolean success = g_subprocess_wait_check_finish(proc, res, &error);
    
    MprisPopoutState *state = g_object_get_data(G_OBJECT(data->root_widget), "mpris-state");
    
    if (state && success && g_file_test(data->local_path, G_FILE_TEST_EXISTS)) {
        gtk_image_set_from_file(state->album_art_image, data->local_path);
    } 
    if (error) g_error_free(error);
    
    g_object_unref(data->root_widget); 
    g_free(data->local_path);
    g_free(data);
}

// --- LISTEN TO THE RUST DAEMON ---
static void on_manager_properties_changed(GDBusProxy *proxy, GVariant *changed_properties, const gchar *const *invalidated_properties, gpointer user_data) {
    (void)proxy; (void)invalidated_properties;
    MprisPopoutState *state = user_data;
    if (!changed_properties) return;

    g_autoptr(GVariant) lrc_var = g_variant_lookup_value(changed_properties, "CurrentLyrics", G_VARIANT_TYPE_STRING);
    if (lrc_var) {
        parse_and_populate_lyrics(state->lyrics_view_state, g_variant_get_string(lrc_var, NULL));
    }

    g_autoptr(GVariant) idx_var = g_variant_lookup_value(changed_properties, "CurrentLyricIndex", G_VARIANT_TYPE_INT32);
    if (idx_var) {
        lyrics_view_set_active_index(state->lyrics_view_state, g_variant_get_int32(idx_var));
    }

    g_autoptr(GVariant) off_var = g_variant_lookup_value(changed_properties, "SyncOffset", G_VARIANT_TYPE_INT32);
    if (off_var) {
        state->current_sync_offset_ms = g_variant_get_int32(off_var);
        update_offset_label(state);
    }
}

static void update_display_metadata(MprisPopoutState *state, GVariantDict *dict) {
    if (!state || !dict) return;
    
    const char *title = NULL, *art_url = NULL; 
    gchar **artists = NULL;
    
    g_variant_dict_lookup(dict, "xesam:title", "&s", &title);
    g_variant_dict_lookup(dict, "xesam:artist", "^as", &artists);
    g_variant_dict_lookup(dict, "mpris:artUrl", "&s", &art_url);
    
    gtk_label_set_text(state->title_label, title ? title : "Unknown Title");
    gtk_label_set_text(state->artist_label, (artists && artists[0]) ? artists[0] : "Unknown Artist");

    if (g_strcmp0(art_url, state->last_art_url) != 0) {
        g_free(state->last_art_url);
        state->last_art_url = g_strdup(art_url);

        if (!art_url) {
            gtk_image_set_from_icon_name(state->album_art_image, "audio-x-generic");
        } 
        else if (g_str_has_prefix(art_url, "file://")) {
            g_autofree gchar *path = g_filename_from_uri(art_url, NULL, NULL);
            if (g_file_test(path, G_FILE_TEST_EXISTS)) {
                gtk_image_set_from_file(state->album_art_image, path);
            } else {
                gtk_image_set_from_icon_name(state->album_art_image, "audio-x-generic");
            }
        } 
        else if (g_str_has_prefix(art_url, "http://") || g_str_has_prefix(art_url, "https://")) {
            g_autofree char *checksum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, art_url, -1);
            g_autofree char *cache_path = g_build_filename(g_get_user_cache_dir(), "aurora-shell", "art", checksum, NULL);
            
            if (g_file_test(cache_path, G_FILE_TEST_EXISTS)) {
                gtk_image_set_from_file(state->album_art_image, cache_path);
            } else {
                gtk_image_set_from_icon_name(state->album_art_image, "audio-x-generic");
                ensure_cache_dir_exists(cache_path);
                
                GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
                GSubprocess *proc = g_subprocess_launcher_spawn(launcher, NULL, "curl", "-s", "-L", "-o", cache_path, art_url, NULL);
                
                if (proc) {
                    ArtDownloadData *data = g_new(ArtDownloadData, 1);
                    data->root_widget = g_object_ref(state->root_widget); 
                    data->local_path = g_strdup(cache_path);
                    g_subprocess_wait_async(proc, NULL, on_curl_finished, data);
                    g_object_unref(proc);
                }
                g_object_unref(launcher);
            }
        } else {
            gtk_image_set_from_icon_name(state->album_art_image, "audio-x-generic");
        }
    }
    g_strfreev(artists);
}

static void update_playback_status_ui(MprisPopoutState *state, const gchar *status) {
    if (!state || !status) return;
    gtk_button_set_icon_name(state->play_pause_button, g_strcmp0(status, "Playing") == 0 ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
}

static void on_properties_changed(GDBusProxy *proxy, GVariant *changed_properties, const gchar *const *invalidated_properties, gpointer user_data) {
    (void)proxy; (void)changed_properties; (void)invalidated_properties;
    update_mpris_state(user_data);
}

static void update_mpris_state(MprisPopoutState *state) {
    if (!state || !state->player_proxy) return;
    g_autoptr(GVariant) metadata_var = g_dbus_proxy_get_cached_property(state->player_proxy, "Metadata");
    g_autoptr(GVariantDict) dict = metadata_var ? g_variant_dict_new(metadata_var) : NULL;
    g_autoptr(GVariant) status_var = g_dbus_proxy_get_cached_property(state->player_proxy, "PlaybackStatus");
    const char *status = status_var ? g_variant_get_string(status_var, NULL) : "Paused";
    
    update_display_metadata(state, dict);
    update_playback_status_ui(state, status);
}

// --- Direct Commands ---
static void on_prev_clicked(GtkButton*b,gpointer d){(void)b;g_dbus_proxy_call(((MprisPopoutState*)d)->player_proxy,"Previous",NULL,G_DBUS_CALL_FLAGS_NONE,-1,NULL,NULL,NULL);}
static void on_play_pause_clicked(GtkButton*b,gpointer d){(void)b;g_dbus_proxy_call(((MprisPopoutState*)d)->player_proxy,"PlayPause",NULL,G_DBUS_CALL_FLAGS_NONE,-1,NULL,NULL,NULL);}
static void on_next_clicked(GtkButton*b,gpointer d){(void)b;g_dbus_proxy_call(((MprisPopoutState*)d)->player_proxy,"Next",NULL,G_DBUS_CALL_FLAGS_NONE,-1,NULL,NULL,NULL);}

static void on_save_lyrics_clicked(GtkButton *b,gpointer d){
    (void)b; MprisPopoutState*s=d;
    g_dbus_proxy_call(s->manager_proxy, "SaveCurrentLyrics", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    show_toast(s, "Alternative lyrics selected & saved.");
}

static void update_offset_label(MprisPopoutState *state) {
    g_autofree gchar *label_text = g_strdup_printf("Sync Offset: %+ld ms", state->current_sync_offset_ms);
    gtk_label_set_text(state->offset_label, label_text);
}

static void push_offset_to_daemon(MprisPopoutState *state) {
    g_dbus_proxy_call(state->manager_proxy, "SetSyncOffset", g_variant_new("(i)", (gint32)state->current_sync_offset_ms), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void on_offset_decrease_clicked(GtkButton *b, gpointer d) { (void)b; MprisPopoutState *s = d; s->current_sync_offset_ms -= LYRICS_OFFSET_ADJUSTMENT_STEP; push_offset_to_daemon(s); }
static void on_offset_increase_clicked(GtkButton *b, gpointer d) { (void)b; MprisPopoutState *s = d; s->current_sync_offset_ms += LYRICS_OFFSET_ADJUSTMENT_STEP; push_offset_to_daemon(s); }


// --- Main Public Function ---
GtkWidget* create_mpris_view(GDBusProxy *manager_proxy, const gchar *bus_name, MprisPopoutState **state_out, gint width, gint height) {
    g_return_val_if_fail(bus_name != NULL, NULL);
    
    MprisPopoutState *state = g_new0(MprisPopoutState, 1);
    state->bus_name = g_strdup(bus_name);
    state->manager_proxy = manager_proxy; // Attached directly from main.c
    
    GtkWidget *root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_size_request(root_box, width, height); 
    
    state->root_widget = root_box;
    g_object_set_data_full(G_OBJECT(root_box), "mpris-state", state, free_popout_state);

    gtk_widget_set_margin_start(root_box, 15);
    gtk_widget_set_margin_end(root_box, 15);
    gtk_widget_set_margin_top(root_box, 15);
    gtk_widget_set_margin_bottom(root_box, 15);
    
    GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 15);
    state->album_art_image = GTK_IMAGE(gtk_image_new());
    gtk_image_set_pixel_size(state->album_art_image, 84);
    gtk_widget_add_css_class(GTK_WIDGET(state->album_art_image), "album-art");
    
    GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(text_box, TRUE); 
    gtk_widget_set_valign(text_box, GTK_ALIGN_CENTER);

    state->title_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(state->title_label), "title-label");
    gtk_label_set_xalign(state->title_label, 0.0);
    
    gtk_label_set_wrap(state->title_label, TRUE);
    gtk_label_set_wrap_mode(state->title_label, PANGO_WRAP_WORD_CHAR);
    gtk_label_set_ellipsize(state->title_label, PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(state->title_label, 2); 
    gtk_label_set_max_width_chars(state->title_label, 1); 

    state->artist_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(state->artist_label), "artist-label");
    gtk_label_set_xalign(state->artist_label, 0.0);
    
    gtk_label_set_wrap(state->artist_label, TRUE);
    gtk_label_set_wrap_mode(state->artist_label, PANGO_WRAP_WORD_CHAR);
    gtk_label_set_ellipsize(state->artist_label, PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(state->artist_label, 1); 
    gtk_label_set_max_width_chars(state->artist_label, 1);

    gtk_box_append(GTK_BOX(text_box), GTK_WIDGET(state->title_label));
    gtk_box_append(GTK_BOX(text_box), GTK_WIDGET(state->artist_label));
    gtk_box_append(GTK_BOX(info_box), GTK_WIDGET(state->album_art_image));
    gtk_box_append(GTK_BOX(info_box), text_box);
    gtk_box_append(GTK_BOX(root_box), info_box); 
    
    GtkWidget *ctrl_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_set_homogeneous(GTK_BOX(ctrl_box), TRUE); 
    gtk_widget_set_halign(ctrl_box, GTK_ALIGN_CENTER); 
    gtk_widget_add_css_class(ctrl_box, "linked");
    
    GtkWidget *prev = gtk_button_new_from_icon_name("media-skip-backward-symbolic");
    state->play_pause_button = GTK_BUTTON(gtk_button_new_from_icon_name("media-playback-start-symbolic"));
    GtkWidget *next = gtk_button_new_from_icon_name("media-skip-forward-symbolic");
    GtkWidget *save_btn = gtk_button_new_from_icon_name("view-refresh-symbolic");
    GtkWidget *offset_decrease_btn = gtk_button_new_from_icon_name("go-previous-symbolic");
    GtkWidget *offset_increase_btn = gtk_button_new_from_icon_name("go-next-symbolic");
    
    gtk_widget_set_tooltip_text(prev, "Previous Track");
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->play_pause_button), "Play/Pause");
    gtk_widget_set_tooltip_text(next, "Next Track");
    gtk_widget_set_tooltip_text(save_btn, "Force refresh alternative lyrics");
    gtk_widget_set_tooltip_text(offset_decrease_btn, "Sync Lyrics Earlier (-50ms)");
    gtk_widget_set_tooltip_text(offset_increase_btn, "Sync Lyrics Later (+50ms)");
    
    g_signal_connect(prev, "clicked", G_CALLBACK(on_prev_clicked), state);
    g_signal_connect(state->play_pause_button, "clicked", G_CALLBACK(on_play_pause_clicked), state);
    g_signal_connect(next, "clicked", G_CALLBACK(on_next_clicked), state);
    g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save_lyrics_clicked), state);
    g_signal_connect(offset_decrease_btn, "clicked", G_CALLBACK(on_offset_decrease_clicked), state);
    g_signal_connect(offset_increase_btn, "clicked", G_CALLBACK(on_offset_increase_clicked), state);
    
    gtk_box_append(GTK_BOX(ctrl_box), prev);
    gtk_box_append(GTK_BOX(ctrl_box), GTK_WIDGET(state->play_pause_button));
    gtk_box_append(GTK_BOX(ctrl_box), next);
    gtk_box_append(GTK_BOX(ctrl_box), offset_decrease_btn);
    gtk_box_append(GTK_BOX(ctrl_box), offset_increase_btn);
    gtk_box_append(GTK_BOX(ctrl_box), save_btn);
    gtk_box_append(GTK_BOX(root_box), ctrl_box);

    GtkWidget *offset_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(offset_box, GTK_ALIGN_CENTER);
    state->offset_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(state->offset_label), "offset-label");
    gtk_box_append(GTK_BOX(offset_box), GTK_WIDGET(state->offset_label));
    gtk_box_append(GTK_BOX(root_box), offset_box);

    GtkWidget *lyrics_widget = create_lyrics_view(&state->lyrics_view_state);
    gtk_widget_set_vexpand(lyrics_widget, TRUE);
    gtk_box_append(GTK_BOX(root_box), lyrics_widget);
    
    GtkWidget *overlay_container = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay_container), root_box);

    state->toast_label = gtk_label_new("");
    gtk_widget_add_css_class(state->toast_label, "toast-label");
    gtk_widget_set_visible(state->toast_label, FALSE);
    gtk_widget_set_halign(state->toast_label, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(state->toast_label, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(state->toast_label, 20);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay_container), state->toast_label);

    g_autoptr(GError) error = NULL;
    state->player_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL, bus_name, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", NULL, &error);
    if (error) {
        g_warning("Could not connect to player: %s", error->message);
    } else {
        state->properties_changed_id = g_signal_connect(state->player_proxy, "g-properties-changed", G_CALLBACK(on_properties_changed), state);
        update_mpris_state(state);
    }
    
    if (state->manager_proxy) {
        state->manager_properties_id = g_signal_connect(state->manager_proxy, "g-properties-changed", G_CALLBACK(on_manager_properties_changed), state);
        
        // Initial sync of properties from daemon
        g_autoptr(GVariant) lrc_var = g_dbus_proxy_get_cached_property(state->manager_proxy, "CurrentLyrics");
        if (lrc_var) parse_and_populate_lyrics(state->lyrics_view_state, g_variant_get_string(lrc_var, NULL));
        
        g_autoptr(GVariant) idx_var = g_dbus_proxy_get_cached_property(state->manager_proxy, "CurrentLyricIndex");
        if (idx_var) lyrics_view_set_active_index(state->lyrics_view_state, g_variant_get_int32(idx_var));
        
        g_autoptr(GVariant) off_var = g_dbus_proxy_get_cached_property(state->manager_proxy, "SyncOffset");
        if (off_var) state->current_sync_offset_ms = g_variant_get_int32(off_var);
        update_offset_label(state);
    }
    
    *state_out = state;
    return overlay_container;
}