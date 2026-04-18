#ifndef UI_STATE_H
#define UI_STATE_H

#include "qscreen.h"
#include "features/annotation.h"
#include <gtk/gtk.h>

typedef enum {
    MODE_REGION,
    MODE_WINDOW,
    MODE_TEXT,
    MODE_COLOR
} SelectionMode;

typedef enum {
    ANN_MODE_DRAW,
    ANN_MODE_TEXT,
    ANN_MODE_RECTANGLE,
    ANN_MODE_CIRCLE,
    ANN_MODE_ARROW,
    ANN_MODE_PIXELATE // NEW
} AnnMode;

typedef struct {
    QScreenState *app_state;
    GtkWindow *window;
    GtkWidget *drawing_area;
    GdkPixbuf *screenshot_pixbuf;
    gchar *temp_screenshot_path;
    SelectionMode current_mode;
    
    GtkRevealer *bottom_panel_revealer;
    GtkRevealer *top_panel_revealer;
    
    GtkWidget *region_button, *window_button, *text_button, *color_button, *screen_button, *save_button, *annotate_toggle_btn;
    
    guint animation_timer_id;
    gboolean is_animating;
    double current_x, current_y, current_w, current_h;
    double target_x, target_y, target_w, target_h;
    double scale_x, scale_y;
    GtkGesture *drag_gesture;
    GtkEventController *motion_controller;
    GtkGesture *click_gesture;
    double drag_start_x, drag_start_y;
    GList *window_geometries, *text_boxes, *selected_text_boxes;
    GtkWidget *ocr_notification_revealer, *ocr_notification_stack;
    gboolean ocr_has_run;

    gboolean has_hovered_color;
    GdkRGBA hovered_color;
    double hover_x, hover_y;

    // Annotation Data
    gboolean is_annotating;
    AnnMode current_ann_mode;
    
    // NEW: ann_pixelate_btn added below
    GtkWidget *ann_draw_btn, *ann_text_btn, *ann_rect_btn, *ann_circle_btn, *ann_arrow_btn, *ann_pixelate_btn, *size_scale;
    
    GList *strokes;
    GList *redo_strokes; 
    AnnotationItem *current_stroke;
    GdkRGBA current_color;
    GList *color_indicators;
    double current_brush_size;
    double current_font_size;
    
    GtkWidget *annotation_fixed;
    GtkWidget *active_text_entry;
    double active_text_x, active_text_y;
} UIState;

void qscreen_set_mode(UIState *state, SelectionMode mode);

#endif // UI_STATE_H