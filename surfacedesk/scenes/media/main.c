// FILE: ./scenes/media/main.c
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <stdbool.h>

// Rust Interface
extern char* rust_media_get_cache_path(const char* url);
extern bool rust_media_download(const char* url, const char* dest);
extern void rust_free_string(char* s);

typedef struct {
    GDBusProxy *mpris_proxy;
    guint watcher_id;
    GtkWidget *art_image;
    GtkWidget *bg_image;
    GtkWidget *title_label;
    GtkWidget *artist_label;
    GtkWidget *play_icon;
    char *current_art_url;
} MediaContext;

typedef struct {
    MediaContext *ctx;
    char *url;
    char *dest_path;
} DownloadTask;

// --- Threaded Download ---
static void run_download_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *c) {
    DownloadTask *dt = task_data;
    bool success = rust_media_download(dt->url, dt->dest_path);
    g_task_return_boolean(task, success);
}

static void on_download_done(GObject *s, GAsyncResult *r, gpointer user_data) {
    if (g_task_propagate_boolean(G_TASK(r), NULL)) {
        DownloadTask *dt = user_data;
        if (dt->ctx && dt->ctx->art_image) {
            GFile *f = g_file_new_for_path(dt->dest_path);
            if (g_file_query_exists(f, NULL)) {
                gtk_picture_set_file(GTK_PICTURE(dt->ctx->art_image), f);
                if (dt->ctx->bg_image) gtk_picture_set_file(GTK_PICTURE(dt->ctx->bg_image), f);
            }
            g_object_unref(f);
        }
    }
}

static void ensure_art(MediaContext *ctx, const char *url) {
    char *cache_path = rust_media_get_cache_path(url);
    if (!cache_path) return;

    if (g_file_test(cache_path, G_FILE_TEST_EXISTS)) {
        GFile *f = g_file_new_for_path(cache_path);
        gtk_picture_set_file(GTK_PICTURE(ctx->art_image), f);
        if (ctx->bg_image) gtk_picture_set_file(GTK_PICTURE(ctx->bg_image), f);
        g_object_unref(f);
    } else {
        DownloadTask *dt = g_new0(DownloadTask, 1);
        dt->ctx = ctx;
        dt->url = g_strdup(url);
        dt->dest_path = g_strdup(cache_path);

        GTask *t = g_task_new(NULL, NULL, on_download_done, dt);
        g_task_set_task_data(t, dt, (GDestroyNotify)g_free);
        g_task_run_in_thread(t, run_download_thread);
        g_object_unref(t);
    }
    rust_free_string(cache_path);
}

static void update_ui(MediaContext *ctx) {
    if (!ctx->mpris_proxy) {
        if (ctx->title_label) gtk_label_set_text(GTK_LABEL(ctx->title_label), "No Media");
        if (ctx->artist_label) gtk_label_set_text(GTK_LABEL(ctx->artist_label), "");
        return;
    }

    g_autoptr(GVariant) metadata = g_dbus_proxy_get_cached_property(ctx->mpris_proxy, "Metadata");
    if (metadata) {
        g_autoptr(GVariantDict) dict = g_variant_dict_new(metadata);
        const char *title = NULL;
        if (g_variant_dict_lookup(dict, "xesam:title", "&s", &title))
            if (ctx->title_label) gtk_label_set_text(GTK_LABEL(ctx->title_label), title);

        g_autoptr(GVariant) artist_var = g_variant_dict_lookup_value(dict, "xesam:artist", G_VARIANT_TYPE_STRING_ARRAY);
        if (artist_var && ctx->artist_label) {
            gsize len = 0; const gchar **artists = g_variant_get_strv(artist_var, &len);
            if (len > 0) gtk_label_set_text(GTK_LABEL(ctx->artist_label), artists[0]);
            g_free(artists);
        }

        const char *art_url = NULL;
        if (g_variant_dict_lookup(dict, "mpris:artUrl", "&s", &art_url) && ctx->art_image) {
            if (g_strcmp0(art_url, ctx->current_art_url) != 0) {
                if (ctx->current_art_url) g_free(ctx->current_art_url);
                ctx->current_art_url = g_strdup(art_url);

                if (g_str_has_prefix(art_url, "file://")) {
                    g_autoptr(GFile) f = g_file_new_for_uri(art_url);
                    gtk_picture_set_file(GTK_PICTURE(ctx->art_image), f);
                    if (ctx->bg_image) gtk_picture_set_file(GTK_PICTURE(ctx->bg_image), f);
                } else if (g_str_has_prefix(art_url, "http")) {
                    ensure_art(ctx, art_url);
                }
            }
        }
    }

    g_autoptr(GVariant) status = g_dbus_proxy_get_cached_property(ctx->mpris_proxy, "PlaybackStatus");
    if (status && ctx->play_icon) {
        const char *s = g_variant_get_string(status, NULL);
        gtk_image_set_from_icon_name(GTK_IMAGE(ctx->play_icon),
            (g_strcmp0(s, "Playing") == 0) ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
    }
}

static void on_mpris_properties_changed(GDBusProxy *proxy, GVariant *changed_properties, const gchar *const *invalidated_properties, gpointer user_data) {
    (void)proxy; (void)changed_properties; (void)invalidated_properties;
    update_ui((MediaContext*)user_data);
}

static void connect_player(MediaContext *ctx, const char *name) {
    if (ctx->mpris_proxy) g_object_unref(ctx->mpris_proxy);
    ctx->mpris_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
        name, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", NULL, NULL);
    if (ctx->mpris_proxy) {
        g_signal_connect(ctx->mpris_proxy, "g-properties-changed", G_CALLBACK(on_mpris_properties_changed), ctx);
        update_ui(ctx);
    }
}

static void on_name_owner_changed(GDBusConnection *c, const char *s, const char *o, const char *i, const char *sig, GVariant *p, gpointer d) {
    const char *name; g_variant_get(p, "(sss)", &name, NULL, NULL);
    if (g_str_has_prefix(name, "org.mpris.MediaPlayer2.")) connect_player((MediaContext*)d, name);
}

static void init_mpris(MediaContext *ctx) {
    g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    ctx->watcher_id = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_name_owner_changed, ctx, NULL);
    g_autoptr(GVariant) names = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if (names) {
        g_autoptr(GVariantIter) iter; g_variant_get(names, "(as)", &iter); char *name;
        while (g_variant_iter_loop(iter, "s", &name)) {
            if (g_str_has_prefix(name, "org.mpris.MediaPlayer2.")) {
                connect_player(ctx, name);
            }
        }
    }
}

static void on_destroy(GtkWidget *w, gpointer data) {
    MediaContext *ctx = data;
    if (ctx->watcher_id > 0) {
        g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        if (bus) g_dbus_connection_signal_unsubscribe(bus, ctx->watcher_id);
    }
    if (ctx->mpris_proxy) g_object_unref(ctx->mpris_proxy);
    g_free(ctx->current_art_url); g_free(ctx);
}

static void on_call_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GDBusProxy *proxy = G_DBUS_PROXY(source_object);
    GError *error = NULL;
    GVariant *result = g_dbus_proxy_call_finish(proxy, res, &error);
    if (error) {
        g_warning("MPRIS Call Failed: %s", error->message);
        g_error_free(error);
    }
    if (result) g_variant_unref(result);
}

static void on_control_click(GtkButton *btn, gpointer user_data) {
    MediaContext *ctx = user_data;
    if (!ctx->mpris_proxy) {
        g_print("SurfaceDesk: No player connected\n");
        return;
    }
    const char *method = g_object_get_data(G_OBJECT(btn), "method");
    if (method) {
        g_dbus_proxy_call(ctx->mpris_proxy, method, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_call_ready, NULL);
    }
}

static GtkWidget* create_control_box(MediaContext *ctx, int size) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 15);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    const char *icons[] = { "media-skip-backward-symbolic", "media-playback-start-symbolic", "media-skip-forward-symbolic" };
    const char *methods[] = { "Previous", "PlayPause", "Next" };
    for(int i=0; i<3; i++) {
        GtkWidget *btn = gtk_button_new_from_icon_name(icons[i]);
        gtk_widget_add_css_class(btn, "flat");
        gtk_widget_set_size_request(btn, size, size);
        g_object_set_data(G_OBJECT(btn), "method", (gpointer)methods[i]);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_control_click), ctx);
        gtk_box_append(GTK_BOX(box), btn);
        if (i == 1) ctx->play_icon = gtk_button_get_child(GTK_BUTTON(btn));
    }
    return box;
}

GtkWidget* scene_media_art(void) {
    MediaContext *ctx = g_new0(MediaContext, 1);
    GtkWidget *wrapper = gtk_overlay_new();
    gtk_widget_add_css_class(wrapper, "card");
    gtk_widget_set_overflow(wrapper, GTK_OVERFLOW_HIDDEN);

    ctx->bg_image = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(ctx->bg_image), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_opacity(ctx->bg_image, 0.35);
    gtk_overlay_set_child(GTK_OVERLAY(wrapper), ctx->bg_image);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 15);
    gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(hbox, 20);

    ctx->art_image = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(ctx->art_image), GTK_CONTENT_FIT_COVER);

    // FIX: Removed strict size request (was 70,70).
    // Now the grid layout determines the size.
    // gtk_widget_set_size_request(ctx->art_image, 70, 70);

    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, "picture { border-radius: 8px; }");
    gtk_style_context_add_provider(gtk_widget_get_style_context(ctx->art_image), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_USER);
    gtk_box_append(GTK_BOX(hbox), ctx->art_image);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
    ctx->title_label = gtk_label_new("No Media");
    gtk_widget_add_css_class(ctx->title_label, "title-2");
    gtk_label_set_ellipsize(GTK_LABEL(ctx->title_label), PANGO_ELLIPSIZE_END);
    ctx->artist_label = gtk_label_new("");
    gtk_widget_add_css_class(ctx->artist_label, "title-3");
    gtk_widget_set_opacity(ctx->artist_label, 0.7);
    gtk_box_append(GTK_BOX(vbox), ctx->title_label);
    gtk_box_append(GTK_BOX(vbox), ctx->artist_label);
    gtk_box_append(GTK_BOX(hbox), vbox);
    gtk_overlay_add_overlay(GTK_OVERLAY(wrapper), hbox);

    init_mpris(ctx);
    g_signal_connect(wrapper, "destroy", G_CALLBACK(on_destroy), ctx);
    return wrapper;
}

GtkWidget* scene_media_controls(void) {
    MediaContext *ctx = g_new0(MediaContext, 1);
    GtkWidget *wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(wrapper, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(wrapper, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(wrapper), create_control_box(ctx, 48));
    init_mpris(ctx);
    g_signal_connect(wrapper, "destroy", G_CALLBACK(on_destroy), ctx);
    return wrapper;
}

GtkWidget* scene_media_full(void) {
    MediaContext *ctx = g_new0(MediaContext, 1);
    GtkWidget *wrapper = gtk_overlay_new();
    gtk_widget_add_css_class(wrapper, "card");
    gtk_widget_set_overflow(wrapper, GTK_OVERFLOW_HIDDEN);

    ctx->bg_image = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(ctx->bg_image), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_opacity(ctx->bg_image, 0.35);
    gtk_overlay_set_child(GTK_OVERLAY(wrapper), ctx->bg_image);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(vbox, GTK_ALIGN_CENTER);

    ctx->art_image = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(ctx->art_image), GTK_CONTENT_FIT_COVER);

    // FIX: Removed strict size request (was 140,140).
    // gtk_widget_set_size_request(ctx->art_image, 140, 140);

    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, "picture { border-radius: 12px; }");
    gtk_style_context_add_provider(gtk_widget_get_style_context(ctx->art_image), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_USER);
    gtk_box_append(GTK_BOX(vbox), ctx->art_image);

    ctx->title_label = gtk_label_new("No Media");
    gtk_widget_add_css_class(ctx->title_label, "title-2");
    ctx->artist_label = gtk_label_new("");
    gtk_widget_add_css_class(ctx->artist_label, "title-3");
    gtk_widget_set_opacity(ctx->artist_label, 0.7);
    gtk_box_append(GTK_BOX(vbox), ctx->title_label);
    gtk_box_append(GTK_BOX(vbox), ctx->artist_label);

    gtk_box_append(GTK_BOX(vbox), create_control_box(ctx, 42));
    gtk_overlay_add_overlay(GTK_OVERLAY(wrapper), vbox);

    init_mpris(ctx);
    g_signal_connect(wrapper, "destroy", G_CALLBACK(on_destroy), ctx);
    return wrapper;
}

GtkWidget* scene_media_wide(void) {
    MediaContext *ctx = g_new0(MediaContext, 1);
    GtkWidget *wrapper = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 15);
    gtk_widget_add_css_class(wrapper, "card");
    gtk_widget_set_overflow(wrapper, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_margin_start(wrapper, 0);

    ctx->art_image = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(ctx->art_image), GTK_CONTENT_FIT_COVER);

    // FIX: Removed strict size request (was 80,80).
    // gtk_widget_set_size_request(ctx->art_image, 80, 80);

    gtk_box_append(GTK_BOX(wrapper), ctx->art_image);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(vbox, TRUE);

    ctx->title_label = gtk_label_new("No Media");
    gtk_widget_add_css_class(ctx->title_label, "title-2");
    gtk_label_set_ellipsize(GTK_LABEL(ctx->title_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(ctx->title_label), 0.0);
    gtk_label_set_max_width_chars(GTK_LABEL(ctx->title_label), 18);

    ctx->artist_label = gtk_label_new("");
    gtk_widget_add_css_class(ctx->artist_label, "title-3");
    gtk_widget_set_opacity(ctx->artist_label, 0.7);
    gtk_label_set_ellipsize(GTK_LABEL(ctx->artist_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(ctx->artist_label), 0.0);
    gtk_label_set_max_width_chars(GTK_LABEL(ctx->artist_label), 18);

    gtk_box_append(GTK_BOX(vbox), ctx->title_label);
    gtk_box_append(GTK_BOX(vbox), ctx->artist_label);
    gtk_box_append(GTK_BOX(wrapper), vbox);

    gtk_box_append(GTK_BOX(wrapper), create_control_box(ctx, 36));
    init_mpris(ctx);
    g_signal_connect(wrapper, "destroy", G_CALLBACK(on_destroy), ctx);
    return wrapper;
}
