#define _GNU_SOURCE
#include "calendar_widget.h"
#include <time.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>

typedef struct {
    GtkWidget *main_container;
    GtkLabel *month_label;
    GtkGrid *calendar_grid;
    GtkListBox *upcoming_list_box;
    GtkPopover *add_event_popover;
    GtkEntry *add_event_title_entry;
    GtkCheckButton *add_event_allday_check;
    GtkDropDown *add_event_hour_dropdown;
    GtkDropDown *add_event_minute_dropdown;
    GtkDropDown *add_event_ampm_dropdown;
    GtkWidget *add_event_time_box;
    GDateTime *current_date;
    GHashTable *events;
    GHashTable *permanent_events;
    guint grid_population_timer_id;
    GtkWidget *upcoming_events_section;
    GtkLabel *upcoming_events_title;
} CalendarWidget;

typedef struct { gchar *time; gchar *title; } Event;
typedef struct { GDateTime *datetime; Event *event; } UpcomingEvent;
typedef struct { int day_to_add; int days_in_month; int grid_x; int grid_y; int current_y; int current_m; int today_y; int today_m; int today_d; } GridPopulationState;
typedef struct { GHashTable *events; GHashTable *permanent_events; } LoadedData;
typedef struct { CalendarWidget *widget; gchar *date_key; Event *event_to_delete; GtkPopover *popover; } DeleteEventData;

static void start_grid_population(CalendarWidget *widget);
static void populate_upcoming_events_list(CalendarWidget *widget);
static void on_data_loaded(GObject *s, GAsyncResult *res, gpointer user_data);
static void save_events(CalendarWidget *widget);

static void free_event(gpointer data) { Event *e = (Event *)data; g_free(e->time); g_free(e->title); g_free(e); }
static void free_event_list(gpointer data) { g_list_free_full((GList *)data, free_event); }
static void free_upcoming_event(gpointer data) { UpcomingEvent *ue = (UpcomingEvent *)data; g_date_time_unref(ue->datetime); g_free(ue); }
static void free_delete_event_data(gpointer data) { DeleteEventData *d = (DeleteEventData *)data; g_free(d->date_key); g_free(d); }
static void destroy_popover_on_close(GtkPopover *popover, gpointer user_data G_GNUC_UNUSED) { gtk_widget_unparent(GTK_WIDGET(popover)); }
static void on_add_event_popover_closed(GtkPopover *p, gpointer user_data G_GNUC_UNUSED) { if (gtk_widget_get_parent(GTK_WIDGET(p))) gtk_widget_unparent(GTK_WIDGET(p)); }

static gchar* format_time_to_12h(const gchar *time_24h) {
    if (g_strcmp0(time_24h, "all-day") == 0) return g_strdup("All-day");
    int h, m;
    if (sscanf(time_24h, "%d:%d", &h, &m) == 2) {
        const char *ap = (h < 12) ? "AM" : "PM";
        if (h == 0) h = 12;
        else if (h > 12) h -= 12;
        return g_strdup_printf("%d:%02d %s", h, m, ap);
    }
    return g_strdup(time_24h);
}

static gchar* get_calendar_data_path(const char* filename) {
    const gchar *data_dir = g_get_user_data_dir();
    gchar *dir_path = g_build_filename(data_dir, "aura-notify", "calendar", NULL);
    g_mkdir_with_parents(dir_path, 0755);
    gchar *file_path = g_build_filename(dir_path, filename, NULL);
    g_free(dir_path);
    return file_path;
}

static GHashTable* load_event_file(const char *filename) {
    gchar *path = get_calendar_data_path(filename);
    GHashTable *event_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_event_list);
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, path, NULL)) {
        g_warning("Could not load calendar file: %s. It might not exist yet.", path);
        g_object_unref(parser);
        g_free(path);
        return event_table;
    }
    JsonObject *root_object = json_node_get_object(json_parser_get_root(parser));
    JsonObjectIter iter;
    const gchar *date_key;
    JsonNode *month_node;
    for (json_object_iter_init(&iter, root_object); json_object_iter_next(&iter, &date_key, &month_node);) {
        GList *event_list = NULL;
        JsonArray *event_array = json_node_get_array(month_node);
        for (guint j = 0; j < json_array_get_length(event_array); j++) {
            JsonObject *event_object = json_array_get_object_element(event_array, j);
            Event *event = g_new(Event, 1);
            event->time = g_strdup(json_object_get_string_member(event_object, "time"));
            event->title = g_strdup(json_object_get_string_member(event_object, "title"));
            event_list = g_list_prepend(event_list, event);
        }
        g_hash_table_insert(event_table, g_strdup(date_key), g_list_reverse(event_list));
    }
    g_object_unref(parser);
    g_free(path);
    return event_table;
}

static void save_events(CalendarWidget *widget) {
    gchar *path = get_calendar_data_path("events.json");
    JsonObject *root_object = json_object_new();
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, widget->events);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        JsonArray *json_array = json_array_new();
        for (GList *l = (GList *)value; l; l = l->next) {
            Event *event = (Event *)l->data;
            JsonObject *json_object = json_object_new();
            json_object_set_string_member(json_object, "time", event->time);
            json_object_set_string_member(json_object, "title", event->title);
            json_array_add_object_element(json_array, json_object);
        }
        json_object_set_array_member(root_object, (gchar *)key, json_array);
    }
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, json_node_init_object(json_node_alloc(), root_object));
    json_generator_set_pretty(generator, TRUE);
    json_generator_to_file(generator, path, NULL);
    g_object_unref(generator);
    g_free(path);
}

static void load_data_in_thread(GTask *task, gpointer s G_GNUC_UNUSED, gpointer t G_GNUC_UNUSED, GCancellable *c G_GNUC_UNUSED) {
    LoadedData *loaded_data = g_new(LoadedData, 1);
    loaded_data->events = load_event_file("events.json");
    loaded_data->permanent_events = load_event_file("permanent_events.json");
    g_task_return_pointer(task, loaded_data, g_free);
}

static void on_data_loaded(GObject *s G_GNUC_UNUSED, GAsyncResult *res, gpointer user_data) {
    CalendarWidget *widget = (CalendarWidget *)user_data;
    GError *err = NULL;
    LoadedData *loaded_data = g_task_propagate_pointer(G_TASK(res), &err);
    if (err) {
        g_warning("Failed to load calendar data: %s", err->message);
        g_error_free(err);
        return;
    }
    if (widget->events) g_hash_table_unref(widget->events);
    if (widget->permanent_events) g_hash_table_unref(widget->permanent_events);
    widget->events = loaded_data->events;
    widget->permanent_events = loaded_data->permanent_events;
    g_free(loaded_data);
    start_grid_population(widget);
    populate_upcoming_events_list(widget);
}

static void on_delete_event_clicked(GtkButton *b G_GNUC_UNUSED, gpointer user_data) {
    DeleteEventData *data = (DeleteEventData*)user_data;
    CalendarWidget *widget = data->widget;
    gpointer key = NULL, value = NULL;
    if (!g_hash_table_steal_extended(widget->events, data->date_key, &key, &value)) return;
    GList *list = (GList*)value;
    GList *link = g_list_find(list, data->event_to_delete);
    if (link) {
        GList *new_list = g_list_delete_link(list, link);
        free_event(data->event_to_delete);
        if (new_list) {
            g_hash_table_insert(widget->events, key, new_list);
        } else {
            g_free(key);
            g_hash_table_remove(widget->events, data->date_key);
        }
    } else {
        g_hash_table_insert(widget->events, key, list);
    }
    save_events(widget);
    start_grid_population(widget);
    populate_upcoming_events_list(widget);
    gtk_popover_popdown(data->popover);
}

static void on_add_event_save(GtkButton *b G_GNUC_UNUSED, gpointer user_data) {
    CalendarWidget *widget = (CalendarWidget *)user_data;
    const gchar *date_key = g_object_get_data(G_OBJECT(widget->add_event_popover), "date-key");
    const char *title_text = gtk_editable_get_text(GTK_EDITABLE(widget->add_event_title_entry));
    if (!date_key || strlen(title_text) == 0) return;
    
    gchar *time_str;
    if (gtk_check_button_get_active(widget->add_event_allday_check)) {
        time_str = g_strdup("all-day");
    } else {
        guint hour = gtk_drop_down_get_selected(widget->add_event_hour_dropdown) + 1;
        guint minute = gtk_drop_down_get_selected(widget->add_event_minute_dropdown) * 5;
        guint is_pm = gtk_drop_down_get_selected(widget->add_event_ampm_dropdown);
        guint hour_24 = hour;
        if (is_pm && hour != 12) hour_24 += 12;
        else if (!is_pm && hour == 12) hour_24 = 0;
        time_str = g_strdup_printf("%02d:%02d", hour_24, minute);
    }
    
    Event *event = g_new(Event, 1);
    event->time = time_str;
    event->title = g_strdup(title_text);
    
    GList *list = g_hash_table_lookup(widget->events, date_key);
    list = g_list_append(list, event);
    g_hash_table_insert(widget->events, g_strdup(date_key), list);
    
    save_events(widget);
    start_grid_population(widget);
    populate_upcoming_events_list(widget);
    gtk_popover_popdown(widget->add_event_popover);
}

static void on_allday_toggled(GtkCheckButton *cb G_GNUC_UNUSED, GParamSpec *p G_GNUC_UNUSED, gpointer user_data) {
    CalendarWidget* widget = (CalendarWidget*)user_data;
    gtk_widget_set_sensitive(widget->add_event_time_box, !gtk_check_button_get_active(GTK_CHECK_BUTTON(cb)));
}

static void on_day_left_clicked(GtkButton *button, gpointer user_data) {
    CalendarWidget *widget = (CalendarWidget*)user_data;
    const gchar *date_key_full = g_object_get_data(G_OBJECT(button), "date-key");
    gchar *date_key_permanent = g_strdup_printf("%d-%d", g_date_time_get_month(widget->current_date), atoi(gtk_button_get_label(GTK_BUTTON(button))));
    
    GList *regular_events = g_hash_table_lookup(widget->events, date_key_full);
    GList *permanent_events = g_hash_table_lookup(widget->permanent_events, date_key_permanent);
    g_free(date_key_permanent);
    
    if (!regular_events && !permanent_events) return;
    
    GtkWidget *popover = gtk_popover_new();
    gtk_widget_add_css_class(popover, "event-popover");
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_popover_set_child(GTK_POPOVER(popover), vbox);
    
    for (GList *l = permanent_events; l; l = l->next) {
        Event *event = (Event*)l->data;
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(hbox, "event-entry");
        GtkWidget *icon = gtk_image_new_from_icon_name("starred-symbolic");
        gtk_widget_add_css_class(icon, "permanent-event-icon");
        gchar *time12 = format_time_to_12h(event->time);
        GtkWidget *time_label = gtk_label_new(time12);
        g_free(time12);
        gtk_widget_add_css_class(time_label, "event-time");
        GtkWidget *title = gtk_label_new(event->title);
        gtk_label_set_xalign(GTK_LABEL(title), 0.0);
        gtk_widget_add_css_class(title, "event-title");
        gtk_box_append(GTK_BOX(hbox), icon);
        gtk_box_append(GTK_BOX(hbox), time_label);
        gtk_box_append(GTK_BOX(hbox), title);
        gtk_box_append(GTK_BOX(vbox), hbox);
    }
    
    for (GList *l = regular_events; l; l = l->next) {
        Event *event = (Event*)l->data;
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(hbox, "event-entry");
        gchar *time12 = format_time_to_12h(event->time);
        GtkWidget *time_label = gtk_label_new(time12);
        g_free(time12);
        gtk_widget_add_css_class(time_label, "event-time");
        GtkWidget *title = gtk_label_new(event->title);
        gtk_label_set_xalign(GTK_LABEL(title), 0.0);
        gtk_widget_add_css_class(title, "event-title");
        gtk_widget_set_hexpand(title, TRUE);
        GtkWidget *delete_button = gtk_button_new_from_icon_name("edit-delete-symbolic");
        gtk_widget_add_css_class(delete_button, "delete-button");
        DeleteEventData *delete_data = g_new(DeleteEventData, 1);
        delete_data->widget = widget;
        delete_data->date_key = g_strdup(date_key_full);
        delete_data->event_to_delete = event;
        delete_data->popover = GTK_POPOVER(popover);
        g_signal_connect(delete_button, "clicked", G_CALLBACK(on_delete_event_clicked), delete_data);
        g_object_set_data_full(G_OBJECT(delete_button), "delete-data", delete_data, free_delete_event_data);
        gtk_box_append(GTK_BOX(hbox), time_label);
        gtk_box_append(GTK_BOX(hbox), title);
        gtk_box_append(GTK_BOX(hbox), delete_button);
        gtk_box_append(GTK_BOX(vbox), hbox);
    }
    
    gtk_widget_set_parent(popover, GTK_WIDGET(button));
    g_signal_connect(popover, "closed", G_CALLBACK(destroy_popover_on_close), NULL);
    gtk_popover_popup(GTK_POPOVER(popover));
}

static void on_day_right_clicked(GtkGestureClick *gesture, int n G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, gpointer user_data) {
    CalendarWidget *widget = (CalendarWidget *)user_data;
    GtkWidget *button = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    const gchar *date_key = g_object_get_data(G_OBJECT(button), "date-key");
    
    g_object_set_data(G_OBJECT(widget->add_event_popover), "date-key", (gpointer)date_key);
    gtk_editable_set_text(GTK_EDITABLE(widget->add_event_title_entry), "");
    gtk_check_button_set_active(widget->add_event_allday_check, FALSE);
    gtk_drop_down_set_selected(widget->add_event_hour_dropdown, 8); 
    gtk_drop_down_set_selected(widget->add_event_minute_dropdown, 0);
    gtk_drop_down_set_selected(widget->add_event_ampm_dropdown, 0); 
    
    gtk_widget_set_parent(GTK_WIDGET(widget->add_event_popover), button);
    gtk_popover_popup(GTK_POPOVER(widget->add_event_popover));
}

static void on_prev_month_clicked(GtkButton *b G_GNUC_UNUSED, gpointer user_data) {
    CalendarWidget *widget = (CalendarWidget *)user_data;
    widget->current_date = g_date_time_add_months(widget->current_date, -1);
    start_grid_population(widget);
}

static void on_next_month_clicked(GtkButton *b G_GNUC_UNUSED, gpointer user_data) {
    CalendarWidget *widget = (CalendarWidget *)user_data;
    widget->current_date = g_date_time_add_months(widget->current_date, 1);
    start_grid_population(widget);
}

static gboolean populate_one_day(gpointer user_data) {
    CalendarWidget *widget = (CalendarWidget*)user_data;
    GridPopulationState *state = g_object_get_data(G_OBJECT(widget->calendar_grid), "population-state");
    
    if (!state || state->day_to_add > state->days_in_month) {
        if (state) g_free(state);
        g_object_set_data(G_OBJECT(widget->calendar_grid), "population-state", NULL);
        widget->grid_population_timer_id = 0;
        return G_SOURCE_REMOVE;
    }
    
    gchar label[4];
    snprintf(label, 4, "%d", state->day_to_add);
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(button, "day-button");
    
    gchar *date_key = g_strdup_printf("%d-%d-%d", state->current_y, state->current_m, state->day_to_add);
    gchar *permanent_key = g_strdup_printf("%d-%d", state->current_m, state->day_to_add);
    g_object_set_data_full(G_OBJECT(button), "date-key", date_key, (GDestroyNotify)g_free);
    
    if (state->day_to_add == state->today_d && state->current_m == state->today_m && state->current_y == state->today_y) {
        gtk_widget_add_css_class(button, "today");
    }
    
    if ((widget->events && g_hash_table_contains(widget->events, date_key)) || (widget->permanent_events && g_hash_table_contains(widget->permanent_events, permanent_key))) {
        gtk_widget_add_css_class(button, "has-event");
    }
    g_free(permanent_key);
    
    g_signal_connect(button, "clicked", G_CALLBACK(on_day_left_clicked), widget);
    
    GtkGesture *right_click_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click_gesture), GDK_BUTTON_SECONDARY);
    g_signal_connect(right_click_gesture, "pressed", G_CALLBACK(on_day_right_clicked), widget);
    gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(right_click_gesture));
    
    gtk_grid_attach(GTK_GRID(widget->calendar_grid), button, state->grid_x, state->grid_y, 1, 1);
    
    state->grid_x++;
    if (state->grid_x > 6) {
        state->grid_x = 0;
        state->grid_y++;
    }
    state->day_to_add++;
    
    return G_SOURCE_CONTINUE;
}

static void start_grid_population(CalendarWidget *widget) {
    if (gtk_widget_get_parent(GTK_WIDGET(widget->add_event_popover))) {
        gtk_popover_popdown(widget->add_event_popover);
    }
    if (widget->grid_population_timer_id > 0) {
        g_source_remove(widget->grid_population_timer_id);
        GridPopulationState *old_state = g_object_get_data(G_OBJECT(widget->calendar_grid), "population-state");
        if (old_state) g_free(old_state);
        g_object_set_data(G_OBJECT(widget->calendar_grid), "population-state", NULL);
    }
    
    gchar *month_str = g_date_time_format(widget->current_date, "%B %Y");
    gtk_label_set_text(widget->month_label, month_str);
    g_free(month_str);
    
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(widget->calendar_grid));
    while (child) {
        GtkWidget *next_child = gtk_widget_get_next_sibling(child);
        if (gtk_widget_has_css_class(child, "day-button")) {
            gtk_grid_remove(GTK_GRID(widget->calendar_grid), child);
        }
        child = next_child;
    }
    
    GridPopulationState *state = g_new0(GridPopulationState, 1);
    state->day_to_add = 1;
    state->current_y = g_date_time_get_year(widget->current_date);
    state->current_m = g_date_time_get_month(widget->current_date);
    state->days_in_month = g_date_get_days_in_month((GDateMonth)state->current_m, (GDateYear)state->current_y);
    
    GDateTime *first_day_of_month = g_date_time_new(g_date_time_get_timezone(widget->current_date), state->current_y, state->current_m, 1, 0, 0, 0);
    state->grid_x = g_date_time_get_day_of_week(first_day_of_month) % 7;
    state->grid_y = 1;
    g_date_time_unref(first_day_of_month);
    
    GDateTime *today = g_date_time_new_now_local();
    state->today_y = g_date_time_get_year(today);
    state->today_m = g_date_time_get_month(today);
    state->today_d = g_date_time_get_day_of_month(today);
    g_date_time_unref(today);
    
    g_object_set_data(G_OBJECT(widget->calendar_grid), "population-state", state);
    widget->grid_population_timer_id = g_timeout_add(1, populate_one_day, widget);
}

static GtkWidget* create_upcoming_event_row(UpcomingEvent *ue) {
    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_add_css_class(row, "upcoming-row");
    
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
    
    gchar *time12 = format_time_to_12h(ue->event->time);
    GtkWidget *time_label = gtk_label_new(time12);
    g_free(time12);
    gtk_label_set_xalign(GTK_LABEL(time_label), 0.0);
    gtk_widget_add_css_class(time_label, "upcoming-event-time");
    
    GtkWidget *title = gtk_label_new(ue->event->title);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_widget_add_css_class(title, "upcoming-event-title");
    
    gtk_box_append(GTK_BOX(hbox), time_label);
    gtk_box_append(GTK_BOX(hbox), title);
    
    return row;
}

static void populate_upcoming_events_list(CalendarWidget *widget) {
    if (!widget->events && !widget->permanent_events) return;
    
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(widget->upcoming_list_box));
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(widget->upcoming_list_box), child);
        child = next;
    }
    
    GList *upcoming_list = NULL;
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *today_start = g_date_time_new(g_date_time_get_timezone(now), g_date_time_get_year(now), g_date_time_get_month(now), g_date_time_get_day_of_month(now), 0, 0, 0);
    gchar *today_key = g_date_time_format(today_start, "%Y-%-m-%-d");
    
    GList *today_events = g_hash_table_lookup(widget->events, today_key);
    if (today_events) {
        for (GList *l = today_events; l; l = l->next) {
            UpcomingEvent *ue = g_new(UpcomingEvent, 1);
            ue->datetime = g_date_time_ref(today_start);
            ue->event = (Event*)l->data;
            upcoming_list = g_list_prepend(upcoming_list, ue);
        }
    }
    g_free(today_key);
    
    if (upcoming_list) {
        gtk_label_set_text(widget->upcoming_events_title, "Today");
        gtk_widget_set_visible(widget->upcoming_events_section, TRUE);
        for (GList *l = upcoming_list; l; l = l->next) {
            gtk_list_box_append(widget->upcoming_list_box, create_upcoming_event_row((UpcomingEvent*)l->data));
        }
    } else {
        gtk_label_set_text(widget->upcoming_events_title, "No Events Today");
        gtk_widget_set_visible(widget->upcoming_events_section, TRUE);
    }
    
    g_list_free_full(upcoming_list, free_upcoming_event);
    g_date_time_unref(now);
    g_date_time_unref(today_start);
}

static void on_widget_destroy(GtkWidget *gtk_widget G_GNUC_UNUSED, gpointer user_data) {
    CalendarWidget *widget = (CalendarWidget *)user_data;
    if (widget->grid_population_timer_id > 0) g_source_remove(widget->grid_population_timer_id);
    
    GridPopulationState *state = g_object_get_data(G_OBJECT(widget->calendar_grid), "population-state");
    if (state) g_free(state);
    
    if (widget->events) g_hash_table_unref(widget->events);
    if (widget->permanent_events) g_hash_table_unref(widget->permanent_events);
    if (widget->current_date) g_date_time_unref(widget->current_date);
    
    g_free(widget);
}

GtkWidget* calendar_widget_new(void) {
    CalendarWidget *widget_data = g_new0(CalendarWidget, 1);
    widget_data->current_date = g_date_time_new_now_local();

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

    widget_data->calendar_grid = GTK_GRID(gtk_grid_new());
    gtk_widget_add_css_class(GTK_WIDGET(widget_data->calendar_grid), "calendar-grid");
    const char *weekdays[] = {"S", "M", "T", "W", "T", "F", "S"};
    for (int i = 0; i < 7; i++) {
        GtkWidget *label = gtk_label_new(weekdays[i]);
        gtk_widget_add_css_class(label, "weekday-label");
        gtk_grid_attach(GTK_GRID(widget_data->calendar_grid), label, i, 0, 1, 1);
    }

    gtk_box_append(GTK_BOX(calendar_box), header_box);
    gtk_box_append(GTK_BOX(calendar_box), GTK_WIDGET(widget_data->calendar_grid));

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

    widget_data->add_event_popover = GTK_POPOVER(gtk_popover_new());
    gtk_widget_add_css_class(GTK_WIDGET(widget_data->add_event_popover), "event-popover");
    g_signal_connect(widget_data->add_event_popover, "closed", G_CALLBACK(on_add_event_popover_closed), NULL);
    GtkWidget *popover_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(popover_vbox, 10);
    gtk_widget_set_margin_end(popover_vbox, 10);
    gtk_widget_set_margin_top(popover_vbox, 10);
    gtk_widget_set_margin_bottom(popover_vbox, 10);
    gtk_popover_set_child(widget_data->add_event_popover, popover_vbox);
    widget_data->add_event_title_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(widget_data->add_event_title_entry, "Event Title (Required)");
    gtk_box_append(GTK_BOX(popover_vbox), GTK_WIDGET(widget_data->add_event_title_entry));
    widget_data->add_event_allday_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("All-day event"));
    gtk_box_append(GTK_BOX(popover_vbox), GTK_WIDGET(widget_data->add_event_allday_check));
    widget_data->add_event_time_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    const char *hours[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", NULL};
    widget_data->add_event_hour_dropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(hours));
    const char *mins[] = {"00", "05", "10", "15", "20", "25", "30", "35", "40", "45", "50", "55", NULL};
    widget_data->add_event_minute_dropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(mins));
    const char *am_pm[] = {"AM", "PM", NULL};
    widget_data->add_event_ampm_dropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(am_pm));
    gtk_box_append(GTK_BOX(widget_data->add_event_time_box), GTK_WIDGET(widget_data->add_event_hour_dropdown));
    gtk_box_append(GTK_BOX(widget_data->add_event_time_box), gtk_label_new(":"));
    gtk_box_append(GTK_BOX(widget_data->add_event_time_box), GTK_WIDGET(widget_data->add_event_minute_dropdown));
    gtk_box_append(GTK_BOX(widget_data->add_event_time_box), GTK_WIDGET(widget_data->add_event_ampm_dropdown));
    gtk_box_append(GTK_BOX(popover_vbox), widget_data->add_event_time_box);
    GtkWidget *save_button = gtk_button_new_with_label("Save Event");
    gtk_box_append(GTK_BOX(popover_vbox), save_button);

    g_signal_connect(prev_button, "clicked", G_CALLBACK(on_prev_month_clicked), widget_data);
    g_signal_connect(next_button, "clicked", G_CALLBACK(on_next_month_clicked), widget_data);
    g_signal_connect(save_button, "clicked", G_CALLBACK(on_add_event_save), widget_data);
    g_signal_connect(widget_data->add_event_allday_check, "notify::active", G_CALLBACK(on_allday_toggled), widget_data);
    g_signal_connect(widget_data->main_container, "destroy", G_CALLBACK(on_widget_destroy), widget_data);

    GTask *task = g_task_new(NULL, NULL, on_data_loaded, widget_data);
    g_task_run_in_thread(task, load_data_in_thread);
    g_object_unref(task);

    return widget_data->main_container;
}