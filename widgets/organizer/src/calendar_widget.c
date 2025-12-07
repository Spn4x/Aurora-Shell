#define _GNU_SOURCE
#include "calendar_widget.h"
#include "eds_event_source.h"
#include <glib/gstdio.h>

typedef struct {
    GtkWidget *main_container;
    GtkLabel *month_label;

    GtkStack *calendar_stack;
    GtkGrid *grid_a;
    GtkGrid *grid_b;
    GtkGrid *current_grid;

    GtkListBox *upcoming_list_box;
    GDateTime *current_date;
    GtkWidget *upcoming_events_section;
    GtkLabel *upcoming_events_title;

    EdsEventSource *event_source;

} CalendarWidget;

static void start_grid_population(CalendarWidget *widget, GtkStackTransitionType transition);
static void populate_upcoming_events_list(CalendarWidget *widget);
static void on_events_changed(EdsEventSource *source, gpointer user_data);

static void destroy_popover_on_close(GtkPopover *popover, gpointer user_data G_GNUC_UNUSED) {
    gtk_widget_unparent(GTK_WIDGET(popover));
}

static gchar* format_datetime_to_12h(GDateTime *dt, gboolean is_all_day) {
    if (is_all_day) return g_strdup("All-day");
    if (!dt) return g_strdup("");
    return g_date_time_format(dt, "%-l:%M %p");
}

static void on_day_left_clicked(GtkButton *button, gpointer user_data) {
    CalendarWidget *widget = (CalendarWidget*)user_data;
    const gchar *date_key_full = g_object_get_data(G_OBJECT(button), "date-key");
    
    int year, month, day;
    sscanf(date_key_full, "%d-%d-%d", &year, &month, &day);

    g_autoptr(GDateTime) day_start = g_date_time_new_local(year, month, day, 0, 0, 0);
    g_autoptr(GDateTime) day_end = g_date_time_add_days(day_start, 1);

    GList *events = eds_event_source_get_events(widget->event_source, day_start, day_end);
    
    if (!events) return;
    
    GtkWidget *popover = gtk_popover_new();
    gtk_widget_add_css_class(popover, "event-popover");
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_popover_set_child(GTK_POPOVER(popover), vbox);
    
    for (GList *l = events; l; l = l->next) {
        EdsCalendarEvent *event = (EdsCalendarEvent*)l->data;
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(hbox, "event-entry");

        gchar *time12 = format_datetime_to_12h(event->dt_start, FALSE); 
        GtkWidget *time_label = gtk_label_new(time12);
        g_free(time12);

        gtk_widget_add_css_class(time_label, "event-time");
        GtkWidget *title = gtk_label_new(event->summary);
        gtk_label_set_xalign(GTK_LABEL(title), 0.0);
        gtk_widget_add_css_class(title, "event-title");
        gtk_widget_set_hexpand(title, TRUE);

        gtk_box_append(GTK_BOX(hbox), time_label);
        gtk_box_append(GTK_BOX(hbox), title);
        gtk_box_append(GTK_BOX(vbox), hbox);
    }
    g_list_free_full(events, eds_calendar_event_free);
    gtk_widget_set_parent(popover, GTK_WIDGET(button));
    g_signal_connect(popover, "closed", G_CALLBACK(destroy_popover_on_close), NULL);
    gtk_popover_popup(GTK_POPOVER(popover));
}

static void on_prev_month_clicked(GtkButton *b G_GNUC_UNUSED, gpointer user_data) {
    CalendarWidget *widget = (CalendarWidget *)user_data;
    GDateTime *new_date = g_date_time_add_months(widget->current_date, -1);
    g_date_time_unref(widget->current_date);
    widget->current_date = new_date;
    start_grid_population(widget, GTK_STACK_TRANSITION_TYPE_SLIDE_RIGHT);
}

static void on_next_month_clicked(GtkButton *b G_GNUC_UNUSED, gpointer user_data) {
    CalendarWidget *widget = (CalendarWidget *)user_data;
    GDateTime *new_date = g_date_time_add_months(widget->current_date, 1);
    g_date_time_unref(widget->current_date);
    widget->current_date = new_date;
    start_grid_population(widget, GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT);
}

// --- MODIFIED: Enforce 6 rows for stable height ---
static void start_grid_population(CalendarWidget *widget, GtkStackTransitionType transition) {
    GtkGrid *target_grid = (widget->current_grid == widget->grid_a) ? widget->grid_b : widget->grid_a;

    // Apply transition
    gtk_stack_set_transition_type(widget->calendar_stack, transition);

    g_autofree gchar *month_str = g_date_time_format(widget->current_date, "%B %Y");
    gtk_label_set_text(widget->month_label, month_str);
    
    // Clear old children
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(target_grid));
    while (child) {
        GtkWidget *next_child = gtk_widget_get_next_sibling(child);
        // Remove everything (buttons and placeholders) except weekday labels
        // Weekday labels are usually at row 0, we can detect them if we didn't give them "day-button" class.
        // Assuming weekday labels are static and we added them in `new()`, we need to be careful.
        // The implementation in `new()` adds labels. If we want to keep them, we should only remove 
        // children at row > 0.
        
        // However, standard GtkGrid behavior makes it easier to just wipe the dynamic content.
        // Let's assume we remove everything that IS NOT a weekday label.
        if (gtk_widget_has_css_class(child, "day-button") || gtk_widget_has_css_class(child, "day-placeholder")) {
            gtk_grid_remove(GTK_GRID(target_grid), child);
        }
        child = next_child;
    }
    
    int current_y = g_date_time_get_year(widget->current_date);
    int current_m = g_date_time_get_month(widget->current_date);
    int days_in_month = g_date_get_days_in_month((GDateMonth)current_m, (GDateYear)current_y);
    
    g_autoptr(GDateTime) first_day_of_month = g_date_time_new(g_date_time_get_timezone(widget->current_date), current_y, current_m, 1, 0, 0, 0);
    // 1=Mon...7=Sun. We want 0=Sun...6=Sat.
    // GDateTime: Mon=1..Sun=7. 
    // To match typical array ["S", "M", "T"...] where index 0 is Sunday:
    // If we want Sunday to be column 0:
    int weekday = g_date_time_get_day_of_week(first_day_of_month); // 1(Mon) - 7(Sun)
    int start_offset = (weekday == 7) ? 0 : weekday; 

    g_autoptr(GDateTime) today = g_date_time_new_now_local();
    int today_y = g_date_time_get_year(today);
    int today_m = g_date_time_get_month(today);
    int today_d = g_date_time_get_day_of_month(today);
    
    // --- STABLE HEIGHT FIX: Always render 6 rows ---
    // We loop through row 1 to 6 (row 0 is headers)
    int day_counter = 1;
    
    for (int row = 1; row <= 6; row++) {
        for (int col = 0; col < 7; col++) {
            
            // Check if we are in the padding before the 1st of the month
            if (row == 1 && col < start_offset) {
                // Pre-month padding
                GtkWidget *placeholder = gtk_label_new("");
                gtk_widget_add_css_class(placeholder, "day-placeholder");
                gtk_grid_attach(GTK_GRID(target_grid), placeholder, col, row, 1, 1);
            } 
            else if (day_counter <= days_in_month) {
                // Actual Day
                g_autofree gchar *label = g_strdup_printf("%d", day_counter);
                GtkWidget *button = gtk_button_new_with_label(label);
                gtk_widget_add_css_class(button, "day-button");
                
                g_autofree gchar *date_key = g_strdup_printf("%d-%d-%d", current_y, current_m, day_counter);
                g_object_set_data_full(G_OBJECT(button), "date-key", g_strdup(date_key), (GDestroyNotify)g_free);
                
                if (day_counter == today_d && current_m == today_m && current_y == today_y) {
                    gtk_widget_add_css_class(button, "today");
                }
                
                g_autoptr(GDateTime) day_dt = g_date_time_new_local(current_y, current_m, day_counter, 12, 0, 0);
                if (eds_event_source_has_events(widget->event_source, day_dt)) {
                    gtk_widget_add_css_class(button, "has-event");
                }

                g_signal_connect(button, "clicked", G_CALLBACK(on_day_left_clicked), widget);
                gtk_grid_attach(GTK_GRID(target_grid), button, col, row, 1, 1);
                
                day_counter++;
            } 
            else {
                // Post-month padding (empty slots to maintain grid height)
                // We add an invisible label that has the same class as the button to maintain spacing
                GtkWidget *placeholder = gtk_label_new(" "); // Space ensures height
                gtk_widget_add_css_class(placeholder, "day-button"); // Reuse style for dimensions
                gtk_widget_set_opacity(placeholder, 0); // Make it invisible
                gtk_widget_set_can_focus(placeholder, FALSE);
                gtk_grid_attach(GTK_GRID(target_grid), placeholder, col, row, 1, 1);
            }
        }
    }

    gtk_stack_set_visible_child(widget->calendar_stack, GTK_WIDGET(target_grid));
    widget->current_grid = target_grid;
}

static GtkWidget* create_upcoming_event_row(EdsCalendarEvent *event) {
    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_add_css_class(row, "upcoming-row");
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
    
    gchar *time12 = format_datetime_to_12h(event->dt_start, FALSE);
    GtkWidget *time_label = gtk_label_new(time12);
    g_free(time12);
    gtk_label_set_xalign(GTK_LABEL(time_label), 0.0);
    gtk_widget_add_css_class(time_label, "upcoming-event-time");
    
    GtkWidget *title = gtk_label_new(event->summary);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_widget_add_css_class(title, "upcoming-event-title");
    
    gtk_box_append(GTK_BOX(hbox), time_label);
    gtk_box_append(GTK_BOX(hbox), title);
    return row;
}

static void populate_upcoming_events_list(CalendarWidget *widget) {
    if (!widget->event_source) return;
    
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(widget->upcoming_list_box));
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(widget->upcoming_list_box), child);
        child = next;
    }
    
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autoptr(GDateTime) today_start = g_date_time_new(g_date_time_get_timezone(now), g_date_time_get_year(now), g_date_time_get_month(now), g_date_time_get_day_of_month(now), 0, 0, 0);
    g_autoptr(GDateTime) today_end = g_date_time_add_days(today_start, 1);
    
    GList* today_events = eds_event_source_get_events(widget->event_source, today_start, today_end);
    
    if (today_events) {
        gtk_label_set_text(widget->upcoming_events_title, "Today");
        gtk_widget_set_visible(widget->upcoming_events_section, TRUE);
        for (GList *l = today_events; l; l = l->next) {
            gtk_list_box_append(widget->upcoming_list_box, create_upcoming_event_row((EdsCalendarEvent*)l->data));
        }
        g_list_free_full(today_events, eds_calendar_event_free);
    } else {
        gtk_label_set_text(widget->upcoming_events_title, "No Events Today");
        gtk_widget_set_visible(widget->upcoming_events_section, TRUE);
    }
}

static void on_widget_destroy(GtkWidget *gtk_widget G_GNUC_UNUSED, gpointer user_data) {
    CalendarWidget *widget = (CalendarWidget *)user_data;
    g_clear_object(&widget->event_source);
    if (widget->current_date) g_date_time_unref(widget->current_date);
    g_free(widget);
}

static void on_events_changed(EdsEventSource *source G_GNUC_UNUSED, gpointer user_data) {
    CalendarWidget *widget = (CalendarWidget*)user_data;
    // For background updates, no animation
    start_grid_population(widget, GTK_STACK_TRANSITION_TYPE_NONE);
    populate_upcoming_events_list(widget);
}

GtkWidget* calendar_widget_new(void) {
    CalendarWidget *widget_data = g_new0(CalendarWidget, 1);
    widget_data->current_date = g_date_time_new_now_local();
    widget_data->event_source = eds_event_source_new();
    g_signal_connect(widget_data->event_source, "changed", G_CALLBACK(on_events_changed), widget_data);

    widget_data->main_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_name(widget_data->main_container, "calendar-widget");
    gtk_widget_add_css_class(widget_data->main_container, "calendar-pane");

    GtkWidget *date_header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(date_header, "calendar-date-header");
    gchar *day_str = g_date_time_format(widget_data->current_date, "%A");
    gchar *date_str = g_date_time_format(widget_data->current_date, "%B %-d, %Y");
    GtkWidget *day_label = gtk_label_new(day_str);
    gtk_widget_add_css_class(day_label, "header-day-label");
    GtkWidget *date_label = gtk_label_new(date_str);
    gtk_widget_add_css_class(date_label, "header-date-label");
    gtk_box_append(GTK_BOX(date_header), day_label);
    gtk_box_append(GTK_BOX(date_header), date_label);
    g_free(day_str);
    g_free(date_str);

    GtkWidget *calendar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(calendar_box, "calendar-inner-box");

    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *prev_button = gtk_button_new_with_label("‹");
    gtk_widget_add_css_class(prev_button, "nav-button");
    GtkWidget *next_button = gtk_button_new_with_label("›");
    gtk_widget_add_css_class(next_button, "nav-button");

    widget_data->month_label = GTK_LABEL(gtk_label_new("..."));
    gtk_widget_set_hexpand(GTK_WIDGET(widget_data->month_label), TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(widget_data->month_label), "month-label");

    gtk_box_append(GTK_BOX(header_box), prev_button);
    gtk_box_append(GTK_BOX(header_box), GTK_WIDGET(widget_data->month_label));
    gtk_box_append(GTK_BOX(header_box), next_button);

    // --- Stack Setup ---
    widget_data->calendar_stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_duration(GTK_STACK(widget_data->calendar_stack), 300);
    // Important: Turn OFF interpolate size now that we are enforcing fixed height.
    // This prevents any unnecessary size queries.
    gtk_stack_set_interpolate_size(GTK_STACK(widget_data->calendar_stack), FALSE);

    widget_data->grid_a = GTK_GRID(gtk_grid_new());
    widget_data->grid_b = GTK_GRID(gtk_grid_new());
    gtk_widget_add_css_class(GTK_WIDGET(widget_data->grid_a), "calendar-grid");
    gtk_widget_add_css_class(GTK_WIDGET(widget_data->grid_b), "calendar-grid");

    const char *weekdays[] = {"S", "M", "T", "W", "T", "F", "S"};
    for (int i = 0; i < 7; i++) {
        GtkWidget *label_a = gtk_label_new(weekdays[i]);
        GtkWidget *label_b = gtk_label_new(weekdays[i]);
        gtk_widget_add_css_class(label_a, "weekday-label");
        gtk_widget_add_css_class(label_b, "weekday-label");
        gtk_grid_attach(widget_data->grid_a, label_a, i, 0, 1, 1);
        gtk_grid_attach(widget_data->grid_b, label_b, i, 0, 1, 1);
    }
    
    gtk_stack_add_child(widget_data->calendar_stack, GTK_WIDGET(widget_data->grid_a));
    gtk_stack_add_child(widget_data->calendar_stack, GTK_WIDGET(widget_data->grid_b));
    
    widget_data->current_grid = widget_data->grid_a;
    gtk_stack_set_visible_child(widget_data->calendar_stack, GTK_WIDGET(widget_data->current_grid));

    gtk_box_append(GTK_BOX(calendar_box), header_box);
    gtk_box_append(GTK_BOX(calendar_box), GTK_WIDGET(widget_data->calendar_stack));

    widget_data->upcoming_events_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(widget_data->upcoming_events_section, "upcoming-events-section");

    widget_data->upcoming_events_title = GTK_LABEL(gtk_label_new("Today"));
    gtk_widget_add_css_class(GTK_WIDGET(widget_data->upcoming_events_title), "upcoming-title");
    gtk_widget_set_halign(GTK_WIDGET(widget_data->upcoming_events_title), GTK_ALIGN_START);

    widget_data->upcoming_list_box = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(widget_data->upcoming_list_box), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(widget_data->upcoming_list_box), "upcoming-list");

    gtk_box_append(GTK_BOX(widget_data->upcoming_events_section), GTK_WIDGET(widget_data->upcoming_events_title));
    gtk_box_append(GTK_BOX(widget_data->upcoming_events_section), GTK_WIDGET(widget_data->upcoming_list_box));

    gtk_box_append(GTK_BOX(widget_data->main_container), date_header);
    gtk_box_append(GTK_BOX(widget_data->main_container), calendar_box);
    gtk_box_append(GTK_BOX(widget_data->main_container), widget_data->upcoming_events_section);

    g_signal_connect(prev_button, "clicked", G_CALLBACK(on_prev_month_clicked), widget_data);
    g_signal_connect(next_button, "clicked", G_CALLBACK(on_next_month_clicked), widget_data);
    g_signal_connect(widget_data->main_container, "destroy", G_CALLBACK(on_widget_destroy), widget_data);

    // Initial load: No animation (NONE)
    start_grid_population(widget_data, GTK_STACK_TRANSITION_TYPE_NONE);
    populate_upcoming_events_list(widget_data);

    return widget_data->main_container;
}