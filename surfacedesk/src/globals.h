// FILE: ./src/globals.h
#ifndef GLOBALS_H
#define GLOBALS_H

#include <gtk/gtk.h>

#define HANDLE_SIZE 24.0 

typedef enum {
    HANDLE_NONE,
    HANDLE_TL, HANDLE_TR, HANDLE_BL, HANDLE_BR,
    HANDLE_MOVE 
} HandleType;

typedef struct {
    int grid_x, grid_y, grid_w, grid_h;
    double vis_x, vis_y, vis_w, vis_h;
    double target_x, target_y, target_w, target_h;
    
    double nudge_x, nudge_y; 

    GtkWidget *widget;
    int id;
    char *type;         
    int variant_index;  
    
    gboolean is_transparent;
    
    int font_size_time; 
    int font_size_date; 
    int padding;        
    gboolean is_24h;
    char *custom_text;
    gboolean use_dominant_color; 

    int min_w;
    int min_h;
} Box;

typedef struct {
    GList *boxes;
    Box *active_box;
    Box *selected_box;
    int next_id;
    gboolean is_dragging;
    HandleType active_handle;
    int cell_size; 
    
    double start_mouse_x, start_mouse_y;
    double start_box_x, start_box_y, start_box_w, start_box_h;
    int anchor_col, anchor_row;
    double grab_off_x, grab_off_y;
    gboolean hover_trash;
    
    GtkWidget *fixed_container; 
    GtkWidget *overlay_draw;    
    
    guint tick_id; 
    double current_offset_x, current_offset_y;
    double target_offset_x, target_offset_y;
    
    GdkCursor *resize_cursor;
    GdkCursor *move_cursor;
    GdkCursor *default_cursor;
} PhysicsContext;

typedef struct {
    GtkWindow *window;
    GtkWidget *desktop_container; 
    GtkWidget *wallpaper_image;
    PhysicsContext physics;
    
    GtkWidget *drawer_revealer;
    GtkWidget *drawer_stack;
    GtkWidget *wallpaper_revealer;
    
    GtkWidget *show_drawer_btn; 
    
    GtkWidget *trash_revealer;
    GtkWidget *trash_box;
    
    gboolean is_editing;
    gboolean is_picking_wallpaper;
    
    char *current_wallpaper_path;
    char *original_wallpaper_path;
} SurfaceDeskApp;

extern SurfaceDeskApp app_state;

void app_set_wallpaper_mode(gboolean enable);

#endif