#include "window.h"
#include "backend.h"
#include <gtk/gtk.h>
#include <adwaita.h> 

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- Data Structures ---
typedef struct {
    double rpm;
    char *level_str;
    double best_temp;
    gboolean temp_found;
} SystemStats;

struct _ThinkfanWidget {
    GtkBox parent; // Changed from GtkWindow

    // View Switcher
    AdwViewStack *stack;
    
    // Page 1: Dashboard
    GtkBox *page_dash;
    GtkLabel *lbl_rpm_val;
    GtkLabel *lbl_temp_val;
    GtkLabel *lbl_status_text; 
    GtkWidget *fan_drawing_area;

    // Page 2: Graphs
    GtkBox *page_graphs;
    GtkWidget *graph_rpm_area;
    GtkWidget *graph_temp_area;
    GtkLabel *lbl_rpm_graph_val;
    GtkLabel *lbl_temp_graph_val;

    // Controls
    GtkToggleButton *btn_auto;
    GtkToggleButton *btn_full;
    GtkToggleButton *btn_manual;
    GtkBox *slider_container;
    GtkScale *slider;
    GtkLabel *slider_val_label;

    // State
    double current_angle;
    double target_visual_speed; 
    double current_visual_speed; 
    gint64 last_frame_time;
    GdkRGBA fan_color; 

    #define HISTORY_LEN 60
    double hist_rpm[HISTORY_LEN];
    double hist_temp[HISTORY_LEN];

    guint update_timer_id;
    gboolean is_loading;
};

// Inherit from GTK_TYPE_BOX
G_DEFINE_TYPE(ThinkfanWidget, thinkfan_widget, GTK_TYPE_BOX)

// Helper: Shift array values
static void push_history(double *arr, double val) {
    for (int i = 0; i < HISTORY_LEN - 1; i++) {
        arr[i] = arr[i+1];
    }
    arr[HISTORY_LEN - 1] = val;
}

static void update_fan_color(ThinkfanWidget *self, double temp, const char *level_str, double rpm) {
    gboolean is_spinning = FALSE;
    if (g_strcmp0(level_str, "auto") == 0) {
        if (rpm > 0) is_spinning = TRUE;
    } else if (g_strcmp0(level_str, "full-speed") == 0 || g_strcmp0(level_str, "disengaged") == 0) {
        is_spinning = TRUE;
    } else {
        int lvl = atoi(level_str);
        if (lvl > 0) is_spinning = TRUE;
    }

    if (!is_spinning) {
        gdk_rgba_parse(&self->fan_color, "#ffffff");
    } else {
        if (temp >= 75) gdk_rgba_parse(&self->fan_color, "#ff7b63"); 
        else if (temp >= 55) gdk_rgba_parse(&self->fan_color, "#f8e45c"); 
        else gdk_rgba_parse(&self->fan_color, "#8ff0a4"); 
    }
}

// --- Drawing Functions (No changes in logic, just casting) ---

static void on_fan_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    ThinkfanWidget *self = THINKFAN_WIDGET(user_data);
    (void)area; 

    double cx = width / 2.0;
    double cy = height / 2.0;
    double radius = MIN(width, height) / 2.0 - 2.0;

    cairo_set_source_rgba(cr, self->fan_color.red, self->fan_color.green, self->fan_color.blue, 0.35); 
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, self->current_angle * (M_PI / 180.0));

    int blades = 7;
    double step = (2 * M_PI) / blades;
    
    for (int i = 0; i < blades; i++) {
        cairo_save(cr);
        cairo_rotate(cr, i * step);
        cairo_move_to(cr, 0, 0);
        cairo_curve_to(cr, radius * 0.2, -radius * 0.05, radius * 0.6, -radius * 0.1, radius, -radius * 0.5); 
        cairo_curve_to(cr, radius * 0.8, -radius * 0.1, radius * 0.4, 0, 0, 0); 
        cairo_close_path(cr);
        cairo_fill(cr);
        cairo_restore(cr);
    }
    
    // Hub
    cairo_set_source_rgba(cr, self->fan_color.red, self->fan_color.green, self->fan_color.blue, 0.5);
    cairo_set_line_width(cr, 2.0);
    cairo_arc(cr, 0, 0, radius * 0.35, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_set_source_rgba(cr, self->fan_color.red, self->fan_color.green, self->fan_color.blue, 0.2);
    cairo_arc(cr, 0, 0, radius * 0.2, 0, 2 * M_PI);
    cairo_fill(cr);
}

static void draw_sparkline(cairo_t *cr, int w, int h, double *data, double max_val, const char *color_hex) {
    if (max_val <= 0) max_val = 100;
    GdkRGBA col;
    gdk_rgba_parse(&col, color_hex);

    cairo_set_source_rgba(cr, col.red, col.green, col.blue, 1.0);
    cairo_set_line_width(cr, 2.0);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    double step_x = (double)w / (HISTORY_LEN - 1);
    for (int i = 0; i < HISTORY_LEN; i++) {
        double x = i * step_x;
        double y = h - ((data[i] / max_val) * (h - 4)) - 2; 
        if (i == 0) cairo_move_to(cr, x, y);
        else cairo_line_to(cr, x, y);
    }
    cairo_stroke_preserve(cr);
    cairo_line_to(cr, w, h);
    cairo_line_to(cr, 0, h);
    cairo_close_path(cr);

    cairo_pattern_t *pat = cairo_pattern_create_linear(0, 0, 0, h);
    cairo_pattern_add_color_stop_rgba(pat, 0, col.red, col.green, col.blue, 0.3);
    cairo_pattern_add_color_stop_rgba(pat, 1, col.red, col.green, col.blue, 0.0);
    cairo_set_source(cr, pat);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);
}

static void on_rpm_graph_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer user_data) {
    ThinkfanWidget *self = THINKFAN_WIDGET(user_data);
    (void)area;
    draw_sparkline(cr, w, h, self->hist_rpm, 5000.0, "#62a0ea"); 
}

static void on_temp_graph_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer user_data) {
    ThinkfanWidget *self = THINKFAN_WIDGET(user_data);
    (void)area;
    draw_sparkline(cr, w, h, self->hist_temp, 100.0, "#ff7b63"); 
}

static gboolean on_fan_tick(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data) {
    ThinkfanWidget *self = THINKFAN_WIDGET(user_data);
    (void)widget;

    gint64 now = gdk_frame_clock_get_frame_time(frame_clock);
    if (self->last_frame_time == 0) {
        self->last_frame_time = now;
        return G_SOURCE_CONTINUE;
    }
    double dt = (now - self->last_frame_time) / 1000000.0;
    self->last_frame_time = now;

    double inertia = 2.0; 
    double diff = self->target_visual_speed - self->current_visual_speed;
    self->current_visual_speed += diff * (inertia * dt);

    if (self->current_visual_speed < 0.1 && self->target_visual_speed == 0) self->current_visual_speed = 0;

    self->current_angle += self->current_visual_speed * dt;
    if (self->current_angle >= 360.0) self->current_angle -= 360.0;

    if (gtk_widget_get_mapped(self->fan_drawing_area)) {
        if (self->current_visual_speed > 0.001 || self->target_visual_speed > 0) {
            gtk_widget_queue_draw(self->fan_drawing_area);
        }
    }
    return G_SOURCE_CONTINUE;
}

// --- ASYNC WORKER (Unchanged logic) ---
static void worker_thread_func(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object; (void)task_data; (void)cancellable;
    SystemStats *stats = g_new0(SystemStats, 1);
    stats->level_str = g_strdup("?");
    stats->temp_found = FALSE;

    char *raw = backend_get_fan_status_raw();
    if (raw) {
        char **lines = g_strsplit(raw, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (g_str_has_prefix(lines[i], "speed:")) {
                char *s = g_strstrip(lines[i] + 6);
                stats->rpm = atoi(s);
            }
            if (g_str_has_prefix(lines[i], "level:")) {
                g_free(stats->level_str);
                stats->level_str = g_strstrip(g_strdup(lines[i] + 6));
            }
        }
        g_strfreev(lines);
        g_free(raw);
    }

    GList *temps = backend_get_temperatures();
    if (temps) {
        SensorData *best_sensor = NULL;
        for (GList *l = temps; l != NULL; l = l->next) {
            SensorData *d = (SensorData*)l->data;
            if (!best_sensor) best_sensor = d; 
            char *lower = g_ascii_strdown(d->label, -1);
            if (strstr(lower, "cpu")) best_sensor = d;
            else if (strstr(lower, "package id 0") && !strstr(best_sensor->label, "cpu")) best_sensor = d;
            else if (strstr(lower, "temp1") && !strstr(best_sensor->label, "cpu") && !strstr(best_sensor->label, "package")) best_sensor = d;
            g_free(lower);
        }
        if (best_sensor) {
            stats->best_temp = best_sensor->value;
            stats->temp_found = TRUE;
        }
        for (GList *l = temps; l != NULL; l = l->next) {
            SensorData *d = (SensorData*)l->data; g_free(d->label); g_free(d);
        }
        g_list_free(temps);
    }
    g_task_return_pointer(task, stats, NULL);
}

static void on_update_complete(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    ThinkfanWidget *self = THINKFAN_WIDGET(user_data);
    (void)source_object;
    GError *error = NULL;
    SystemStats *stats = g_task_propagate_pointer(G_TASK(res), &error);
    if (error) { g_error_free(error); return; }
    if (!stats) return;

    push_history(self->hist_rpm, stats->rpm);
    if (stats->temp_found) push_history(self->hist_temp, stats->best_temp);
    else push_history(self->hist_temp, 0);

    if (gtk_widget_get_mapped(self->graph_rpm_area)) gtk_widget_queue_draw(self->graph_rpm_area);
    if (gtk_widget_get_mapped(self->graph_temp_area)) gtk_widget_queue_draw(self->graph_temp_area);

    char *rpm_txt = g_strdup_printf("%.0f", stats->rpm);
    gtk_label_set_text(self->lbl_rpm_val, rpm_txt);
    gtk_label_set_text(self->lbl_rpm_graph_val, rpm_txt); 
    g_free(rpm_txt);

    char *mode_txt = g_strdup_printf("Mode: %s", stats->level_str);
    gtk_label_set_text(self->lbl_status_text, mode_txt);
    
    if (stats->temp_found) update_fan_color(self, stats->best_temp, stats->level_str, stats->rpm);
    g_free(mode_txt);

    if (stats->temp_found) {
        char *t_str = g_strdup_printf("%.0f°", stats->best_temp);
        gtk_label_set_text(self->lbl_temp_val, t_str);
        gtk_label_set_text(self->lbl_temp_graph_val, t_str); 
        g_free(t_str);
        // Styles are handled by shell, we just add classes
        gtk_widget_remove_css_class(GTK_WIDGET(self->lbl_temp_val), "temp-hot");
        gtk_widget_remove_css_class(GTK_WIDGET(self->lbl_temp_val), "temp-warm");
        gtk_widget_remove_css_class(GTK_WIDGET(self->lbl_temp_val), "temp-ok");
        if (stats->best_temp >= 75) gtk_widget_add_css_class(GTK_WIDGET(self->lbl_temp_val), "temp-hot");
        else if (stats->best_temp >= 55) gtk_widget_add_css_class(GTK_WIDGET(self->lbl_temp_val), "temp-warm");
        else gtk_widget_add_css_class(GTK_WIDGET(self->lbl_temp_val), "temp-ok");
    }

    double target = 0;
    if (g_strcmp0(stats->level_str, "auto") == 0) target = (stats->rpm > 0) ? 500.0 : 0;
    else if (g_strcmp0(stats->level_str, "full-speed") == 0 || g_strcmp0(stats->level_str, "disengaged") == 0) target = 1500.0;
    else {
        int lvl = atoi(stats->level_str);
        switch (lvl) {
            case 1: target = 500.0; break;
            case 2: target = 660.0; break;
            case 3: target = 820.0; break;
            case 4: target = 980.0; break;
            case 5: target = 1150.0; break;
            case 6: target = 1320.0; break;
            case 7: target = 1500.0; break;
            default: target = 0; break;
        }
    }
    self->target_visual_speed = target;
    g_free(stats->level_str);
    g_free(stats);
}

static gboolean on_timer(gpointer data) {
    ThinkfanWidget *self = THINKFAN_WIDGET(data);
    GTask *task = g_task_new(NULL, NULL, on_update_complete, self);
    g_task_run_in_thread(task, worker_thread_func);
    g_object_unref(task);
    return G_SOURCE_CONTINUE;
}

static void sync_ui_with_hardware(ThinkfanWidget *self) {
    self->is_loading = TRUE;
    char *raw = backend_get_fan_status_raw();
    if (raw) {
        char **lines = g_strsplit(raw, "\n", -1);
        char *level_str = NULL;
        for (int i = 0; lines[i]; i++) {
            if (g_str_has_prefix(lines[i], "level:")) {
                level_str = g_strstrip(g_strdup(lines[i] + 6));
                break;
            }
        }
        if (level_str) {
            if (g_strcmp0(level_str, "auto") == 0) gtk_toggle_button_set_active(self->btn_auto, TRUE);
            else if (g_strcmp0(level_str, "full-speed") == 0) gtk_toggle_button_set_active(self->btn_full, TRUE);
            else {
                gtk_toggle_button_set_active(self->btn_manual, TRUE);
                gtk_range_set_value(GTK_RANGE(self->slider), atoi(level_str));
            }
            g_free(level_str);
        }
        g_strfreev(lines);
        g_free(raw);
    }
    self->is_loading = FALSE;
}

static void set_fan_mode(ThinkfanWidget *self, const char *mode) {
    if (self->is_loading) return;
    if (!backend_set_fan_level(mode)) backend_request_permissions();
    on_timer(self);
}

static void on_mode_toggled(GtkToggleButton *btn, ThinkfanWidget *self) {
    if (self->is_loading || !gtk_toggle_button_get_active(btn)) return;
    if (btn == self->btn_auto) set_fan_mode(self, "auto");
    else if (btn == self->btn_full) set_fan_mode(self, "full-speed");
    else if (btn == self->btn_manual) {
        double val = gtk_range_get_value(GTK_RANGE(self->slider));
        char buf[8]; snprintf(buf, 8, "%d", (int)val);
        set_fan_mode(self, buf);
    }
}

static void on_slider_changed(GtkRange *range, ThinkfanWidget *self) {
    double val = gtk_range_get_value(range);
    char buf[8]; snprintf(buf, 8, "%d", (int)val);
    gtk_label_set_text(self->slider_val_label, buf);
    if (!self->is_loading) {
        if (!gtk_toggle_button_get_active(self->btn_manual)) 
            gtk_toggle_button_set_active(self->btn_manual, TRUE);
        set_fan_mode(self, buf);
    }
}

static void thinkfan_widget_init(ThinkfanWidget *self) {
    self->is_loading = TRUE;
    self->current_angle = 0;
    self->target_visual_speed = 0;
    self->current_visual_speed = 0;
    self->last_frame_time = 0;
    gdk_rgba_parse(&self->fan_color, "#ffffff");

    for(int i=0; i<HISTORY_LEN; i++) { self->hist_rpm[i] = 0; self->hist_temp[i] = 0; }

    // Load icons from GResource (icons should stay in resource for portability)
    GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    gtk_icon_theme_add_resource_path(theme, "/com/zocker/thinkfan/icons");
    
    // Configure self (Box)
    gtk_widget_set_size_request(GTK_WIDGET(self), 280, -1);
    gtk_widget_add_css_class(GTK_WIDGET(self), "hud-card");
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 12);

    // --- VIEW SWITCHER ---
    self->stack = ADW_VIEW_STACK(adw_view_stack_new());
    gtk_widget_set_vexpand(GTK_WIDGET(self->stack), TRUE);
    
    self->page_dash = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 16));
    self->page_graphs = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));

    AdwViewSwitcher *switcher = ADW_VIEW_SWITCHER(adw_view_switcher_new());
    adw_view_switcher_set_stack(switcher, self->stack);
    adw_view_switcher_set_policy(switcher, ADW_VIEW_SWITCHER_POLICY_WIDE); 
    gtk_box_append(GTK_BOX(self), GTK_WIDGET(switcher));
    gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->stack));

    // --- PAGE 1: DASHBOARD ---
    gtk_widget_set_valign(GTK_WIDGET(self->page_dash), GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(GTK_WIDGET(self->page_dash), TRUE);

        self->fan_drawing_area = gtk_drawing_area_new();
        gtk_widget_set_size_request(self->fan_drawing_area, 180, 180);
        gtk_widget_set_halign(self->fan_drawing_area, GTK_ALIGN_CENTER);
        gtk_widget_set_vexpand(self->fan_drawing_area, TRUE);
        gtk_widget_set_valign(self->fan_drawing_area, GTK_ALIGN_CENTER);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->fan_drawing_area), on_fan_draw, self, NULL);
        gtk_widget_add_tick_callback(self->fan_drawing_area, on_fan_tick, self, NULL);
        gtk_box_append(GTK_BOX(self->page_dash), self->fan_drawing_area);

        GtkWidget *hero_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_set_homogeneous(GTK_BOX(hero_box), TRUE);
        
        GtkWidget *fan_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_add_css_class(fan_box, "stat-box");
        self->lbl_rpm_val = GTK_LABEL(gtk_label_new("0"));
        gtk_widget_add_css_class(GTK_WIDGET(self->lbl_rpm_val), "hero-val");
        gtk_label_set_width_chars(self->lbl_rpm_val, 5);
        GtkWidget *lbl_fan = gtk_label_new("RPM");
        gtk_widget_add_css_class(lbl_fan, "hero-lbl");
        gtk_box_append(GTK_BOX(fan_box), GTK_WIDGET(self->lbl_rpm_val));
        gtk_box_append(GTK_BOX(fan_box), lbl_fan);

        GtkWidget *temp_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_add_css_class(temp_box, "stat-box");
        self->lbl_temp_val = GTK_LABEL(gtk_label_new("--"));
        gtk_widget_add_css_class(GTK_WIDGET(self->lbl_temp_val), "hero-val");
        gtk_label_set_width_chars(self->lbl_temp_val, 4);
        GtkWidget *lbl_cpu = gtk_label_new("CPU");
        gtk_widget_add_css_class(lbl_cpu, "hero-lbl");
        gtk_box_append(GTK_BOX(temp_box), GTK_WIDGET(self->lbl_temp_val));
        gtk_box_append(GTK_BOX(temp_box), lbl_cpu);

        gtk_box_append(GTK_BOX(hero_box), fan_box);
        gtk_box_append(GTK_BOX(hero_box), temp_box);
        gtk_widget_set_margin_top(hero_box, 12);
        gtk_box_append(GTK_BOX(self->page_dash), hero_box);

        self->lbl_status_text = GTK_LABEL(gtk_label_new("Mode: --"));
        gtk_widget_add_css_class(GTK_WIDGET(self->lbl_status_text), "status-text");
        gtk_box_append(GTK_BOX(self->page_dash), GTK_WIDGET(self->lbl_status_text));

    AdwViewStackPage *p1 = adw_view_stack_add_titled(self->stack, GTK_WIDGET(self->page_dash), "dash", "Dashboard");
    adw_view_stack_page_set_icon_name(p1, "tf-dashboard-symbolic");

    // --- PAGE 2: GRAPHS ---
    gtk_widget_set_margin_top(GTK_WIDGET(self->page_graphs), 10);
    
        GtkWidget *cnt_rpm = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(cnt_rpm, "graph-container");
        gtk_widget_set_vexpand(cnt_rpm, TRUE); 

        GtkWidget *head_rpm = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *lbl_rpm_title = gtk_label_new("RPM");
        gtk_widget_add_css_class(lbl_rpm_title, "graph-label");
        gtk_widget_set_halign(lbl_rpm_title, GTK_ALIGN_START);
        gtk_widget_set_hexpand(lbl_rpm_title, TRUE);
        self->lbl_rpm_graph_val = GTK_LABEL(gtk_label_new("0"));
        gtk_widget_add_css_class(GTK_WIDGET(self->lbl_rpm_graph_val), "graph-value");
        gtk_box_append(GTK_BOX(head_rpm), lbl_rpm_title);
        gtk_box_append(GTK_BOX(head_rpm), GTK_WIDGET(self->lbl_rpm_graph_val));
        gtk_box_append(GTK_BOX(cnt_rpm), head_rpm);

        self->graph_rpm_area = gtk_drawing_area_new();
        gtk_widget_set_vexpand(self->graph_rpm_area, TRUE);
        gtk_widget_set_size_request(self->graph_rpm_area, -1, 100);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->graph_rpm_area), on_rpm_graph_draw, self, NULL);
        gtk_box_append(GTK_BOX(cnt_rpm), self->graph_rpm_area);
        gtk_box_append(GTK_BOX(self->page_graphs), cnt_rpm);

        GtkWidget *cnt_temp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(cnt_temp, "graph-container");
        gtk_widget_set_vexpand(cnt_temp, TRUE); 

        GtkWidget *head_temp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *lbl_temp_title = gtk_label_new("Temp");
        gtk_widget_add_css_class(lbl_temp_title, "graph-label");
        gtk_widget_set_halign(lbl_temp_title, GTK_ALIGN_START);
        gtk_widget_set_hexpand(lbl_temp_title, TRUE);
        self->lbl_temp_graph_val = GTK_LABEL(gtk_label_new("0"));
        gtk_widget_add_css_class(GTK_WIDGET(self->lbl_temp_graph_val), "graph-value");
        gtk_box_append(GTK_BOX(head_temp), lbl_temp_title);
        gtk_box_append(GTK_BOX(head_temp), GTK_WIDGET(self->lbl_temp_graph_val));
        gtk_box_append(GTK_BOX(cnt_temp), head_temp);

        self->graph_temp_area = gtk_drawing_area_new();
        gtk_widget_set_vexpand(self->graph_temp_area, TRUE);
        gtk_widget_set_size_request(self->graph_temp_area, -1, 100);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->graph_temp_area), on_temp_graph_draw, self, NULL);
        gtk_box_append(GTK_BOX(cnt_temp), self->graph_temp_area);
        gtk_box_append(GTK_BOX(self->page_graphs), cnt_temp);

    AdwViewStackPage *p2 = adw_view_stack_add_titled(self->stack, GTK_WIDGET(self->page_graphs), "graphs", "Graphs");
    adw_view_stack_page_set_icon_name(p2, "tf-graphs-symbolic");

    // --- CONTROLS ---
    GtkWidget *ctrl_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(ctrl_box, "linked");
    gtk_widget_set_halign(ctrl_box, GTK_ALIGN_CENTER);

    self->btn_auto = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Auto"));
    self->btn_full = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Full"));
    self->btn_manual = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Manual"));
    
    gtk_toggle_button_set_group(self->btn_full, self->btn_auto);
    gtk_toggle_button_set_group(self->btn_manual, self->btn_auto);

    gtk_box_append(GTK_BOX(ctrl_box), GTK_WIDGET(self->btn_auto));
    gtk_box_append(GTK_BOX(ctrl_box), GTK_WIDGET(self->btn_full));
    gtk_box_append(GTK_BOX(ctrl_box), GTK_WIDGET(self->btn_manual));
    gtk_box_append(GTK_BOX(self), ctrl_box);

    // --- SLIDER ---
    self->slider_container = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8));
    gtk_widget_set_margin_top(GTK_WIDGET(self->slider_container), 8);
    
    self->slider = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 7, 1));
    gtk_scale_set_draw_value(self->slider, FALSE); 
    gtk_widget_set_hexpand(GTK_WIDGET(self->slider), TRUE);
    gtk_range_set_round_digits(GTK_RANGE(self->slider), 0);
    for(int i=0; i<=7; i++) gtk_scale_add_mark(self->slider, i, GTK_POS_BOTTOM, NULL);

    self->slider_val_label = GTK_LABEL(gtk_label_new("0"));
    gtk_widget_add_css_class(GTK_WIDGET(self->slider_val_label), "title-4");

    gtk_box_append(GTK_BOX(self->slider_container), GTK_WIDGET(self->slider));
    gtk_box_append(GTK_BOX(self->slider_container), GTK_WIDGET(self->slider_val_label));
    gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->slider_container));

    g_signal_connect(self->btn_auto, "toggled", G_CALLBACK(on_mode_toggled), self);
    g_signal_connect(self->btn_full, "toggled", G_CALLBACK(on_mode_toggled), self);
    g_signal_connect(self->btn_manual, "toggled", G_CALLBACK(on_mode_toggled), self);
    g_signal_connect(self->slider, "value-changed", G_CALLBACK(on_slider_changed), self);

    sync_ui_with_hardware(self);
    self->update_timer_id = g_timeout_add_seconds(1, on_timer, self);
    on_timer(self);
}

static void thinkfan_widget_dispose(GObject *object) {
    ThinkfanWidget *self = THINKFAN_WIDGET(object);
    if (self->update_timer_id > 0) {
        g_source_remove(self->update_timer_id);
        self->update_timer_id = 0;
    }
    G_OBJECT_CLASS(thinkfan_widget_parent_class)->dispose(object);
}

static void thinkfan_widget_class_init(ThinkfanWidgetClass *class) {
    G_OBJECT_CLASS(class)->dispose = thinkfan_widget_dispose;
}

GtkWidget* thinkfan_widget_new(void) {
    return g_object_new(THINKFAN_TYPE_WIDGET, NULL);
}