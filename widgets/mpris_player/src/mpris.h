#ifndef MPRIS_H
#define MPRIS_H

#include <gtk/gtk.h>
#include "lyrics.h"
#include "utils.h"

typedef struct _MprisPopoutState {
    GtkWindow *window;
    
    GDBusProxy *player_proxy;
    GDBusProxy *manager_proxy; // Added proxy to the Rust Daemon
    gchar *bus_name;
    GtkWidget *root_widget; 

    GtkImage *album_art_image;
    GtkLabel *title_label;
    GtkLabel *artist_label;
    GtkButton *play_pause_button;
    
    GtkWidget *toast_label;

    LyricsView *lyrics_view_state;

    gulong properties_changed_id;
    gulong manager_properties_id; // Added signal ID

    gint64 current_sync_offset_ms;
    GtkLabel *offset_label;

    gchar *last_art_url; 
} MprisPopoutState;

GtkWidget* create_mpris_view(GDBusProxy *manager_proxy, const gchar *bus_name, MprisPopoutState **state_out, gint width, gint height);

#endif // MPRIS_H