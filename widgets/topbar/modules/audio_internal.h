#ifndef AUDIO_INTERNAL_H
#define AUDIO_INTERNAL_H

#include <gtk/gtk.h>
#include <gio/gio.h>

#define BANNER_DURATION_MS 5000
#define ART_LOAD_DELAY_MS 500
#define NUM_VIS_BARS 24

typedef struct {
    GtkWidget *main_stack;
    
    // Bluetooth UI
    GtkWidget *bt_drawing_area;
    
    // Media UI
    GtkWidget *media_overlay;
    GtkWidget *art_container; 
    GtkWidget *visualizer_da;
    GtkPicture *album_art_image;
    GtkLabel  *song_title_label;
    
    // Interaction UI
    GtkWidget *popover;
    GtkWidget *popover_list_box;

    // D-Bus
    GDBusObjectManager *bluez_manager;
    GDBusProxy *mpris_proxy;
    GDBusProxy *media_manager_proxy; 

    // State
    gboolean is_connected;
    gboolean is_powered;
    gboolean is_media_active;
    gboolean is_playing;
    
    gchar *device_name;
    int battery_percentage;
    gchar *current_track_signature;
    gchar *last_art_url;
    gchar *preferred_view; 
    guint banner_timer_id; 
    guint art_timer_id;

    // Visualizer State
    double vis_currents[NUM_VIS_BARS];
    double vis_phase; // NEW: Tracks the wave movement for correlation
    guint vis_timer_id;
    
    // Hover Fade State
    double hover_alpha; 
    guint hover_anim_id;
    gboolean is_hovered;

} AudioModule;

// Shared Functions
void update_combined_state(AudioModule *module);
void trigger_temporary_view(AudioModule *module, const char *view_name);
void cairo_rounded_rectangle(cairo_t *cr, double x, double y, double width, double height, double radius);

// Media Functions
void init_media_ui(AudioModule *module);
void setup_media_dbus(AudioModule *module);
void cleanup_media_module(AudioModule *module);
void update_mpris_view(AudioModule *module);

#endif // AUDIO_INTERNAL_H