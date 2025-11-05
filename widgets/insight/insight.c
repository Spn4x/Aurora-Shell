#include <gtk/gtk.h>
#include <cairo.h>
#include <sqlite3.h>
#include <time.h>
#include <glib.h>
#include <math.h>
#include <string.h>
#include <ctype.h>

// --- Data Structures ---
typedef struct {
    GtkWidget *today_time_label, *top_apps_box, *chart_area, *selected_day_title_label;
    GtkWidget *week_title_label, *week_time_label;
    GtkWidget *next_week_button;
    double daily_hours[7];
    GDate *today_date;
    GDate *selected_date;
    GDate *reference_date; // A date within the week we want to display
    guint timer_id;
} AppData;

typedef struct { char app_class[256]; long seconds; } AppUsageInfo;
typedef struct { double fraction; } CustomBarData;


// --- Forward Declarations ---
static gboolean update_data_and_ui(gpointer user_data);
static void draw_chart(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);
static void on_db_changed(GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event_type, gpointer user_data);


// --- Helper & Drawing Functions ---
static gboolean get_css_color(GtkWidget *widget, const char *color_name, GdkRGBA *out_color) {
    GtkStyleContext *context = gtk_widget_get_style_context(widget);
    return gtk_style_context_lookup_color(context, color_name, out_color);
}
static char* prettify_app_name(const char* original_name) {
    const char* last_dot = strrchr(original_name, '.');
    char* pretty_name = last_dot ? g_strdup(last_dot + 1) : g_strdup(original_name);
    if (pretty_name[0]) { pretty_name[0] = toupper(pretty_name[0]); }
    return pretty_name;
}
static void draw_rounded_bar(cairo_t *cr, double x, double y, double width, double height, double radius) {
    if (height < 1 || width < 1) return;
    if (radius > height / 2) radius = height / 2;
    if (radius > width / 2) radius = width / 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, 1.5 * G_PI);
    cairo_arc(cr, x + width - radius, y + radius, radius, 1.5 * G_PI, 2 * G_PI);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, 0.5 * G_PI);
    cairo_arc(cr, x + radius, y + height - radius, radius, 0.5 * G_PI, G_PI);
    cairo_close_path(cr);
}
static void draw_custom_app_bar(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    CustomBarData *bar_data = (CustomBarData *)user_data;
    GdkRGBA accent_color, trough_color;
    get_css_color(GTK_WIDGET(area), "accent", &accent_color);
    if (get_css_color(GTK_WIDGET(area), "foreground", &trough_color)) {
        trough_color.alpha = 0.05;
    }
    cairo_set_source_rgba(cr, trough_color.red, trough_color.green, trough_color.blue, trough_color.alpha);
    draw_rounded_bar(cr, 0, 0, width, height, height / 2.0);
    cairo_fill(cr);
    double bar_width = width * bar_data->fraction;
    if (bar_width > 0) {
        cairo_set_source_rgba(cr, accent_color.red, accent_color.green, accent_color.blue, accent_color.alpha);
        draw_rounded_bar(cr, 0, 0, bar_width, height, height / 2.0);
        cairo_fill(cr);
    }
}

// --- Main Data & UI Update Function ---
static gboolean update_data_and_ui(gpointer user_data) {
    AppData *data = (AppData *)user_data;
    sqlite3 *db;
    sqlite3_stmt *stmt;
    for (int i=0; i<7; i++) data->daily_hours[i] = 0.0;
    long week_total_seconds = 0;
    
    // ==============================================================================
    // === FIX BLOCK 1: Establish a single, unambiguous definition of the week. ===
    // ==============================================================================
    
    // Calculate the start of the week (Sunday) based on the reference_date.
    GDate *week_start_date = g_date_copy(data->reference_date);
    // GLib weekday: Mon=1, Tue=2, ..., Sun=7. `g_date_get_weekday() % 7` maps Sun to 0.
    g_date_subtract_days(week_start_date, g_date_get_weekday(week_start_date) % 7);

    // Calculate the end of the week (Saturday).
    GDate *week_end_date = g_date_copy(week_start_date);
    g_date_add_days(week_end_date, 6);

    // Prepare date strings for the SQL query.
    char week_start_str[11];
    char week_end_str[11];
    g_date_strftime(week_start_str, sizeof(week_start_str), "%Y-%m-%d", week_start_date);
    g_date_strftime(week_end_str, sizeof(week_end_str), "%Y-%m-%d", week_end_date);
    
    // --- Now, use these exact dates for the query. ---
    g_autofree gchar *db_path = g_build_filename(g_get_home_dir(), ".local", "share", "aurora-insight.db", NULL);
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) { 
        g_date_free(week_start_date);
        g_date_free(week_end_date);
        return G_SOURCE_CONTINUE; 
    }
    
    // The new, unambiguous SQL query. It no longer does its own date math.
    const char *sql_week = "SELECT CAST(strftime('%w', date) AS INTEGER), SUM(usage_seconds) FROM app_usage WHERE date BETWEEN ? AND ? GROUP BY date;";
    
    if (sqlite3_prepare_v2(db, sql_week, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, week_start_str, -1, SQLITE_STATIC); // Bind start date
        sqlite3_bind_text(stmt, 2, week_end_str, -1, SQLITE_STATIC);   // Bind end date
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int day_of_week = sqlite3_column_int(stmt, 0); // Sunday=0, Monday=1, etc.
            long seconds = sqlite3_column_int(stmt, 1);
            if (day_of_week >= 0 && day_of_week <= 6) { 
                data->daily_hours[day_of_week] = (double)seconds / 3600.0; 
                week_total_seconds += seconds; 
            }
        }
    }
    sqlite3_finalize(stmt);
    
    // ==============================================================================
    // === END FIX BLOCK 1 ===
    // ==============================================================================

    char selected_date_str[11];
    g_date_strftime(selected_date_str, sizeof(selected_date_str), "%Y-%m-%d", data->selected_date);
    long selected_day_total_seconds = 0;
    const char *sql_day_total = "SELECT SUM(usage_seconds) FROM app_usage WHERE date = ?;";
    if (sqlite3_prepare_v2(db, sql_day_total, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, selected_date_str, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) { selected_day_total_seconds = sqlite3_column_int(stmt, 0); }
    }
    sqlite3_finalize(stmt);
    char time_str[128];
    snprintf(time_str, sizeof(time_str), "<span size='x-large' weight='bold'>%ldh %ldm</span>", selected_day_total_seconds / 3600, (selected_day_total_seconds % 3600) / 60);
    gtk_label_set_markup(GTK_LABEL(data->today_time_label), time_str);
    snprintf(time_str, sizeof(time_str), "<span size='x-large' weight='bold'>%ldh %ldm</span>", week_total_seconds / 3600, (week_total_seconds % 3600) / 60);
    gtk_label_set_markup(GTK_LABEL(data->week_time_label), time_str);
    gboolean is_current_week = (g_date_compare(data->today_date, week_start_date) >= 0 && g_date_compare(data->today_date, week_end_date) <= 0);
    
    if (is_current_week) { 
        gtk_label_set_text(GTK_LABEL(data->week_title_label), "This Week");
    } else { 
        GDate *today_week_start = g_date_copy(data->today_date);
        g_date_subtract_days(today_week_start, g_date_get_weekday(today_week_start) % 7);
        g_date_subtract_days(today_week_start, 7); // Start of last week
        gboolean is_last_week = (g_date_compare(week_start_date, today_week_start) == 0);
        g_date_free(today_week_start);
        
        if (is_last_week) { 
            gtk_label_set_text(GTK_LABEL(data->week_title_label), "Last Week");
        } else { 
            char range_str[64]; 
            char start_str[32], end_str[32]; 
            g_date_strftime(start_str, sizeof(start_str), "%b %d", week_start_date); 
            g_date_strftime(end_str, sizeof(end_str), "%b %d", week_end_date); 
            snprintf(range_str, sizeof(range_str), "%s - %s", start_str, end_str); 
            gtk_label_set_text(GTK_LABEL(data->week_title_label), range_str); 
        }
    }
    
    // Cleanup GDate objects from the fix block
    g_date_free(week_start_date);
    g_date_free(week_end_date);
    
    gtk_widget_set_sensitive(data->next_week_button, !is_current_week);
    if (g_date_compare(data->selected_date, data->today_date) == 0) { gtk_label_set_text(GTK_LABEL(data->selected_day_title_label), "Today");
    } else { GDate *yesterday = g_date_copy(data->today_date); g_date_subtract_days(yesterday, 1);
        if (g_date_compare(data->selected_date, yesterday) == 0) { gtk_label_set_text(GTK_LABEL(data->selected_day_title_label), "Yesterday");
        } else { char day_name[32]; g_date_strftime(day_name, sizeof(day_name), "%A", data->selected_date); gtk_label_set_text(GTK_LABEL(data->selected_day_title_label), day_name); }
        g_date_free(yesterday);
    }
    GtkWidget *child; while ((child = gtk_widget_get_first_child(data->top_apps_box))) { gtk_box_remove(GTK_BOX(data->top_apps_box), child); }
    AppUsageInfo top_apps[5]; int app_count = 0;
    const char *sql_top_apps = "SELECT app_class, SUM(usage_seconds) FROM app_usage WHERE date = ? GROUP BY app_class ORDER BY SUM(usage_seconds) DESC LIMIT 5;";
    if (sqlite3_prepare_v2(db, sql_top_apps, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, selected_date_str, -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW && app_count < 5) { strncpy(top_apps[app_count].app_class, (const char*)sqlite3_column_text(stmt, 0), 255); top_apps[app_count].seconds = sqlite3_column_int(stmt, 1); app_count++; }
    }
    sqlite3_finalize(stmt);
    long max_seconds = (app_count > 0) ? top_apps[0].seconds : 1; if (max_seconds == 0) max_seconds = 1;
    if (app_count == 0) { GtkWidget *placeholder = gtk_label_new("No activity recorded"); gtk_widget_add_css_class(placeholder, "dim-label"); gtk_widget_set_halign(placeholder, GTK_ALIGN_CENTER); gtk_widget_set_valign(placeholder, GTK_ALIGN_CENTER); gtk_widget_set_vexpand(placeholder, TRUE); gtk_box_append(GTK_BOX(data->top_apps_box), placeholder); }
    for (int i = 0; i < app_count; i++) {
        GtkWidget *row_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3); GtkWidget *labels_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5); char* pretty_name = prettify_app_name(top_apps[i].app_class); GtkWidget *name_label = gtk_label_new(pretty_name); g_free(pretty_name); gtk_widget_set_hexpand(name_label, TRUE); gtk_widget_set_halign(name_label, GTK_ALIGN_START); char app_time_str[64]; long seconds = top_apps[i].seconds;
        if (seconds >= 3600) { snprintf(app_time_str, sizeof(app_time_str), "%ldh %ldm", seconds / 3600, (seconds % 3600) / 60); }
        else { snprintf(app_time_str, sizeof(app_time_str), "%ldm %lds", seconds / 60, seconds % 60); }
        GtkWidget *time_label = gtk_label_new(app_time_str); gtk_widget_add_css_class(time_label, "dim-label"); gtk_box_append(GTK_BOX(labels_hbox), name_label); gtk_box_append(GTK_BOX(labels_hbox), time_label); GtkWidget *bar = gtk_drawing_area_new(); gtk_widget_set_size_request(bar, -1, 8); CustomBarData *bar_data = g_new(CustomBarData, 1); bar_data->fraction = (double)top_apps[i].seconds / max_seconds; gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(bar), draw_custom_app_bar, bar_data, g_free); gtk_box_append(GTK_BOX(row_vbox), labels_hbox); gtk_box_append(GTK_BOX(row_vbox), bar); gtk_box_append(GTK_BOX(data->top_apps_box), row_vbox);
    }
    sqlite3_close(db);
    gtk_widget_queue_draw(data->chart_area);
    return G_SOURCE_CONTINUE;
}


// --- UI Callbacks ---
static void on_db_changed(GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event_type, gpointer user_data) {
    (void)monitor; (void)file; (void)other_file; (void)event_type;
    update_data_and_ui(user_data);
}
static void on_prev_week_clicked(GtkButton *button, gpointer user_data) { (void)button; AppData *data = (AppData *)user_data; g_date_subtract_days(data->reference_date, 7); g_date_free(data->selected_date); data->selected_date = g_date_copy(data->reference_date); update_data_and_ui(data); }
static void on_next_week_clicked(GtkButton *button, gpointer user_data) { (void)button; AppData *data = (AppData *)user_data; g_date_add_days(data->reference_date, 7); g_date_free(data->selected_date); data->selected_date = g_date_copy(data->reference_date); update_data_and_ui(data); }

static void on_chart_clicked(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) { 
    (void)gesture; (void)n_press; (void)y; 
    AppData *data = (AppData *)user_data; 
    int width = gtk_widget_get_width(data->chart_area); 
    double bar_width = (width - 6 * 10 - 40) / 7.0; 
    double bar_spacing = 10.0; 
    int clicked_index = -1; 
    for (int i = 0; i < 7; i++) { 
        double x_pos = 10 + i * (bar_width + bar_spacing); 
        if (x >= x_pos && x <= x_pos + bar_width) { 
            clicked_index = i; 
            break; 
        } 
    }
    if (clicked_index != -1) { 
        // ==============================================================================
        // === FIX BLOCK 2: Use the same consistent logic to find the clicked day. ===
        // ==============================================================================
        GDate *week_start = g_date_copy(data->reference_date); 
        g_date_subtract_days(week_start, g_date_get_weekday(week_start) % 7);
        g_date_add_days(week_start, clicked_index); 
        g_date_free(data->selected_date); 
        data->selected_date = week_start; 
        update_data_and_ui(data); 
    }
}

static GtkWidget* create_stat_box(const char* title, GtkWidget **title_label_ptr, GtkWidget **time_label_ptr) { GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5); *title_label_ptr = gtk_label_new(title); gtk_widget_set_halign(*title_label_ptr, GTK_ALIGN_START); gtk_box_append(GTK_BOX(vbox), *title_label_ptr); *time_label_ptr = gtk_label_new(NULL); gtk_label_set_markup(GTK_LABEL(*time_label_ptr), "<span size='x-large' weight='bold'>0h 0m</span>"); gtk_widget_set_halign(*time_label_ptr, GTK_ALIGN_START); gtk_box_append(GTK_BOX(vbox), *time_label_ptr); return vbox; }
static void on_widget_destroy(GtkWidget *widget, gpointer user_data) { (void)widget; AppData *data = (AppData *)user_data; if (data->timer_id > 0) { g_source_remove(data->timer_id); } g_date_free(data->today_date); g_date_free(data->selected_date); g_date_free(data->reference_date); }

// --- Plugin Entry Point ---
G_MODULE_EXPORT GtkWidget* create_widget(const char* config_string) {
    (void)config_string;
    AppData *app_data = g_new0(AppData, 1);
    app_data->today_date = g_date_new();
    g_date_set_time_t(app_data->today_date, time(NULL));
    app_data->selected_date = g_date_copy(app_data->today_date);
    app_data->reference_date = g_date_copy(app_data->today_date);
    GtkWidget *root_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_widget_set_name(root_vbox, "aurora-insight");
    gtk_widget_set_margin_start(root_vbox, 20);
    gtk_widget_set_margin_end(root_vbox, 20);
    gtk_widget_set_margin_top(root_vbox, 20);
    gtk_widget_set_margin_bottom(root_vbox, 15);
    g_signal_connect(root_vbox, "destroy", G_CALLBACK(on_widget_destroy), app_data);
    g_signal_connect_swapped(root_vbox, "destroy", G_CALLBACK(g_free), app_data);

    g_autofree gchar *trigger_path = g_build_filename(g_get_home_dir(), ".local", "share", "aurora-insight.trigger", NULL);
    GFile *trigger_file = g_file_new_for_path(trigger_path);
    GFileMonitor *monitor = g_file_monitor_file(trigger_file, G_FILE_MONITOR_NONE, NULL, NULL);
    if (monitor) {
        g_signal_connect(monitor, "changed", G_CALLBACK(on_db_changed), app_data);
        g_object_set_data_full(G_OBJECT(root_vbox), "db-monitor", monitor, g_object_unref);
    }
    g_object_unref(trigger_file);

    GtkWidget *main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 25);
    gtk_widget_set_vexpand(main_hbox, TRUE);
    gtk_box_append(GTK_BOX(root_vbox), main_hbox);
    GtkWidget *chart_overlay = gtk_overlay_new();
    gtk_box_append(GTK_BOX(main_hbox), chart_overlay);
    app_data->chart_area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(app_data->chart_area), 400);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(app_data->chart_area), 300);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app_data->chart_area), draw_chart, app_data, NULL);
    gtk_overlay_set_child(GTK_OVERLAY(chart_overlay), app_data->chart_area);
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_chart_clicked), app_data);
    gtk_widget_add_controller(app_data->chart_area, GTK_EVENT_CONTROLLER(click));
    GtkWidget *week_time_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(week_time_box, GTK_ALIGN_START);
    gtk_widget_set_valign(week_time_box, GTK_ALIGN_START);
    gtk_widget_set_margin_start(week_time_box, 15);
    gtk_widget_set_margin_top(week_time_box, 10);
    gtk_overlay_add_overlay(GTK_OVERLAY(chart_overlay), week_time_box);
    app_data->week_time_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(app_data->week_time_label), "<span size='x-large' weight='bold'>0h 0m</span>");
    gtk_box_append(GTK_BOX(week_time_box), app_data->week_time_label);
    GtkWidget *right_frame = gtk_frame_new(NULL);
    gtk_box_append(GTK_BOX(main_hbox), right_frame);
    GtkWidget *right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_widget_set_size_request(right_vbox, 280, -1);
    gtk_widget_set_margin_start(right_vbox, 5); gtk_widget_set_margin_end(right_vbox, 5);
    gtk_widget_set_margin_top(right_vbox, 5); gtk_widget_set_margin_bottom(right_vbox, 5);
    gtk_frame_set_child(GTK_FRAME(right_frame), right_vbox);
    GtkWidget *today_box = create_stat_box("Today", &app_data->selected_day_title_label, &app_data->today_time_label);
    gtk_box_append(GTK_BOX(right_vbox), today_box);
    gtk_box_append(GTK_BOX(right_vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    GtkWidget *top_apps_title = gtk_label_new("Top Apps");
    gtk_widget_set_halign(top_apps_title, GTK_ALIGN_START); gtk_widget_add_css_class(top_apps_title, "dim-label");
    gtk_box_append(GTK_BOX(right_vbox), top_apps_title);
    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled_window, TRUE);
    app_data->top_apps_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), app_data->top_apps_box);
    gtk_box_append(GTK_BOX(right_vbox), scrolled_window);
    GtkWidget *bottom_nav_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(bottom_nav_hbox, "bottom-nav");
    gtk_widget_set_halign(bottom_nav_hbox, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root_vbox), bottom_nav_hbox);
    GtkWidget *prev_week_button = gtk_button_new_from_icon_name("go-previous-symbolic");
    g_signal_connect(prev_week_button, "clicked", G_CALLBACK(on_prev_week_clicked), app_data);
    gtk_box_append(GTK_BOX(bottom_nav_hbox), prev_week_button);
    app_data->week_title_label = gtk_label_new("This Week");
    gtk_box_append(GTK_BOX(bottom_nav_hbox), app_data->week_title_label);
    app_data->next_week_button = gtk_button_new_from_icon_name("go-next-symbolic");
    g_signal_connect(app_data->next_week_button, "clicked", G_CALLBACK(on_next_week_clicked), app_data);
    gtk_box_append(GTK_BOX(bottom_nav_hbox), app_data->next_week_button);
    
    update_data_and_ui(app_data);
    app_data->timer_id = g_timeout_add_seconds(300, (GSourceFunc)update_data_and_ui, app_data); 
    
    return root_vbox;
}
static void draw_chart(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    AppData *data = (AppData *)user_data;
    GdkRGBA accent_color, foreground_color;
    get_css_color(GTK_WIDGET(area), "accent", &accent_color);
    get_css_color(GTK_WIDGET(area), "foreground", &foreground_color);
    const char *day_labels[7] = {"S", "M", "T", "W", "T", "F", "S"};
    int selected_wday = g_date_get_weekday(data->selected_date) % 7;
    double top_padding = 10.0, bottom_padding = 20.0, chart_height = height - top_padding - bottom_padding;
    double max_hours = 0; for (int i=0; i<7; i++) if (data->daily_hours[i] > max_hours) max_hours = data->daily_hours[i];
    max_hours = (max_hours < 4.0) ? 4.0 : (ceil(max_hours / 2.0) * 2.0);
    cairo_set_source_rgba(cr, foreground_color.red, foreground_color.green, foreground_color.blue, 0.5);
    cairo_set_font_size(cr, 12.0);
    for (int h = 0; h <= max_hours; h += 2) {
        if (max_hours <= 0) break;
        double y = top_padding + chart_height - (h / max_hours * chart_height);
        cairo_set_source_rgba(cr, foreground_color.red, foreground_color.green, foreground_color.blue, 0.1);
        cairo_set_line_width(cr, 1.0);
        char label_text[16]; snprintf(label_text, sizeof(label_text), "%dh", h);
        cairo_move_to(cr, width - 25, y + 4); cairo_show_text(cr, label_text);
        cairo_move_to(cr, 0, y); cairo_line_to(cr, width - 30, y); cairo_stroke(cr);
    }
    double bar_width = (width - 6 * 10 - 40) / 7.0; double bar_spacing = 10.0;

    // ==============================================================================
    // === FIX BLOCK 3: Use the same consistent logic to find the date for each bar. ===
    // ==============================================================================
    GDate *week_start_for_drawing = g_date_copy(data->reference_date);
    g_date_subtract_days(week_start_for_drawing, g_date_get_weekday(week_start_for_drawing) % 7);

    for (int i = 0; i < 7; i++) {
        double x_pos = 10 + i * (bar_width + bar_spacing);
        double bar_height_pixels = (max_hours > 0) ? (data->daily_hours[i] / max_hours) * chart_height : 0;
        
        GDate *bar_date = g_date_copy(week_start_for_drawing);
        g_date_add_days(bar_date, i);

        if (g_date_compare(bar_date, data->today_date) == 0) {
            cairo_set_source_rgba(cr, accent_color.red, accent_color.green, accent_color.blue, accent_color.alpha);
        } else {
            cairo_set_source_rgba(cr, accent_color.red, accent_color.green, accent_color.blue, 0.6);
        }
        g_date_free(bar_date);
        
        draw_rounded_bar(cr, x_pos, top_padding + chart_height - bar_height_pixels, bar_width, bar_height_pixels, 4.0);
        cairo_fill(cr);
        if (i == selected_wday) {
            cairo_set_source_rgba(cr, foreground_color.red, foreground_color.green, foreground_color.blue, 0.8);
            cairo_set_line_width(cr, 2.0);
            draw_rounded_bar(cr, x_pos, top_padding + chart_height - bar_height_pixels, bar_width, bar_height_pixels, 4.0);
            cairo_stroke(cr);
        }
        cairo_set_source_rgba(cr, foreground_color.red, foreground_color.green, foreground_color.blue, .7);
        cairo_move_to(cr, x_pos + (bar_width / 2) - 4, height - 5);
        cairo_show_text(cr, day_labels[i]);
    }
    g_date_free(week_start_for_drawing);
}