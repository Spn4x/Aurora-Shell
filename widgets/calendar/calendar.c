#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <time.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <glib/gstdio.h>

#define WIDGET_WIDTH 550

// --- DATA STRUCTURES (Unchanged) ---
typedef struct { GtkWidget *main_container; GtkLabel *month_label; GtkGrid *calendar_grid; GtkListBox *upcoming_list_box; GtkPopover *add_event_popover; GtkEntry *add_event_title_entry; GtkCheckButton *add_event_allday_check; GtkDropDown *add_event_hour_dropdown; GtkDropDown *add_event_minute_dropdown; GtkDropDown *add_event_ampm_dropdown; GtkWidget *add_event_time_box; GDateTime *current_date; GHashTable *events; GHashTable *permanent_events; guint grid_population_timer_id; } CalendarWidget;
typedef struct { gchar *time; gchar *title; } Event;
typedef struct { GDateTime *datetime; Event *event; } UpcomingEvent;
typedef struct { int day_to_add; int days_in_month; int grid_x; int grid_y; int current_y; int current_m; int today_y; int today_m; int today_d; } GridPopulationState;
typedef struct { GHashTable *events; GHashTable *permanent_events; } LoadedData;
typedef struct { CalendarWidget *widget; gchar *date_key; Event *event_to_delete; GtkPopover *popover; } DeleteEventData;

// Forward declarations of your original functions
static void start_grid_population(CalendarWidget *widget);
static void populate_upcoming_events_list(CalendarWidget *widget);

// --- MEMORY & HELPERS (Unchanged) ---
static void free_event(gpointer data) { Event *e = (Event *)data; g_free(e->time); g_free(e->title); g_free(e); }
static void free_event_list(gpointer data) { g_list_free_full((GList *)data, free_event); }
static void free_upcoming_event(gpointer data) { UpcomingEvent *ue = (UpcomingEvent *)data; g_date_time_unref(ue->datetime); g_free(ue); }
static void free_delete_event_data(gpointer data) { DeleteEventData *d = (DeleteEventData *)data; g_free(d->date_key); g_free(d); }
static gchar* format_time_to_12h(const gchar *time_24h) { if (g_strcmp0(time_24h, "all-day") == 0) return g_strdup("All-day"); int h, m; if (sscanf(time_24h, "%d:%d", &h, &m) == 2) { const char *ap = (h < 12) ? "AM" : "PM"; if (h == 0) h = 12; else if (h > 12) h -= 12; return g_strdup_printf("%d:%02d %s", h, m, ap); } return g_strdup(time_24h); }
static void destroy_popover_on_close(GtkPopover *popover, gpointer user_data) { (void)user_data; gtk_widget_unparent(GTK_WIDGET(popover)); }

// --- DATA HANDLING (Unchanged) ---
static GHashTable* load_event_file(const char *rel_path) { gchar *path = g_build_filename(g_get_home_dir(), "VS Code Projects/C-projects/aurora-shell/widgets/calendar/", rel_path, NULL); GHashTable *et = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_event_list); JsonParser *p = json_parser_new(); if (!json_parser_load_from_file(p, path, NULL)) { g_object_unref(p); g_free(path); return et; } JsonObject *ro = json_node_get_object(json_parser_get_root(p)); JsonObjectIter i; const gchar *dk; JsonNode *mn; for (json_object_iter_init(&i, ro); json_object_iter_next(&i, &dk, &mn);) { GList *el = NULL; JsonArray *ea = json_node_get_array(mn); for (guint j = 0; j < json_array_get_length(ea); j++) { JsonObject *eo = json_array_get_object_element(ea, j); Event *e = g_new(Event, 1); e->time = g_strdup(json_object_get_string_member(eo, "time")); e->title = g_strdup(json_object_get_string_member(eo, "title")); el = g_list_prepend(el, e); } g_hash_table_insert(et, g_strdup(dk), g_list_reverse(el)); } g_object_unref(p); g_free(path); return et; }
static void load_data_in_thread(GTask *task, gpointer s, gpointer t, GCancellable *c) { (void)s;(void)t;(void)c; LoadedData *ld = g_new(LoadedData, 1); ld->events = load_event_file("data/events.json"); ld->permanent_events = load_event_file("data/permanent_events.json"); g_task_return_pointer(task, ld, g_free); }
static void save_events(CalendarWidget *widget) { gchar *path = g_build_filename(g_get_home_dir(), "VS Code Projects/C-projects/aurora-shell/widgets/calendar/data/events.json", NULL); JsonObject *ro = json_object_new(); GHashTableIter i; gpointer k, v; g_hash_table_iter_init(&i, widget->events); while (g_hash_table_iter_next(&i, &k, &v)) { JsonArray *ja = json_array_new(); for (GList *l = (GList *)v; l; l = l->next) { Event *e = (Event *)l->data; JsonObject *jo = json_object_new(); json_object_set_string_member(jo, "time", e->time); json_object_set_string_member(jo, "title", e->title); json_array_add_object_element(ja, jo); } json_object_set_array_member(ro, (gchar *)k, ja); } JsonGenerator *g = json_generator_new(); json_generator_set_root(g, json_node_init_object(json_node_alloc(), ro)); json_generator_set_pretty(g, TRUE); json_generator_to_file(g, path, NULL); g_object_unref(g); g_free(path); }
static void on_data_loaded(GObject *s, GAsyncResult *res, gpointer user_data) { (void)s; CalendarWidget *w = (CalendarWidget *)user_data; GError *err = NULL; LoadedData *ld = g_task_propagate_pointer(G_TASK(res), &err); if (err) { g_warning("Failed: %s", err->message); g_error_free(err); return; } if (w->events) g_hash_table_unref(w->events); if (w->permanent_events) g_hash_table_unref(w->permanent_events); w->events = ld->events; w->permanent_events = ld->permanent_events; g_free(ld); start_grid_population(w); populate_upcoming_events_list(w); }

// --- UI CALLBACKS (Your original, correct functions) ---
static void on_delete_event_clicked(GtkButton *b, gpointer user_data) { (void)b; DeleteEventData *d = (DeleteEventData*)user_data; CalendarWidget *w = d->widget; gpointer k=NULL, v=NULL; if (!g_hash_table_steal_extended(w->events, d->date_key, &k, &v)) return; GList *l = (GList*)v, *link = g_list_find(l, d->event_to_delete); if (link) { GList *nl = g_list_delete_link(l, link); free_event(d->event_to_delete); if (nl) g_hash_table_insert(w->events, k, nl); else g_free(k); } else g_hash_table_insert(w->events, k, l); save_events(w); start_grid_population(w); populate_upcoming_events_list(w); gtk_popover_popdown(d->popover); }
static void on_add_event_save(GtkButton *b, gpointer user_data) { (void)b; CalendarWidget *w = (CalendarWidget *)user_data; const gchar *dk = g_object_get_data(G_OBJECT(w->add_event_popover), "date-key"); const char *t = gtk_editable_get_text(GTK_EDITABLE(w->add_event_title_entry)); if (!dk || strlen(t) == 0) return; gchar *ts; if (gtk_check_button_get_active(w->add_event_allday_check)) ts = g_strdup("all-day"); else { guint h = gtk_drop_down_get_selected(w->add_event_hour_dropdown)+1, m = gtk_drop_down_get_selected(w->add_event_minute_dropdown)*5, is_pm = gtk_drop_down_get_selected(w->add_event_ampm_dropdown), h24 = h; if (is_pm && h!=12) h24+=12; else if (!is_pm && h==12) h24=0; ts = g_strdup_printf("%02d:%02d", h24, m); } Event *e = g_new(Event,1); e->time = ts; e->title = g_strdup(t); GList *l = g_hash_table_lookup(w->events, dk); if (!l) { l = g_list_append(NULL, e); g_hash_table_insert(w->events, g_strdup(dk), l); } else { l = g_list_append(l, e); } save_events(w); start_grid_population(w); populate_upcoming_events_list(w); gtk_popover_popdown(w->add_event_popover); }
static void on_allday_toggled(GtkCheckButton *cb, GParamSpec *p, gpointer user_data) { (void)p; gtk_widget_set_sensitive(((CalendarWidget*)user_data)->add_event_time_box, !gtk_check_button_get_active(cb)); }
static void on_day_left_clicked(GtkButton *button, gpointer user_data) { CalendarWidget *w = (CalendarWidget*)user_data; const gchar *dkf = g_object_get_data(G_OBJECT(button), "date-key"); gchar *dkp = g_strdup_printf("%d-%d", g_date_time_get_month(w->current_date), atoi(gtk_button_get_label(GTK_BUTTON(button)))); GList *re = g_hash_table_lookup(w->events, dkf), *pe = g_hash_table_lookup(w->permanent_events, dkp); g_free(dkp); if (!re && !pe) return; GtkWidget *pop = gtk_popover_new(); gtk_widget_add_css_class(pop, "event-popover"); GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5); gtk_popover_set_child(GTK_POPOVER(pop), vbox); for (GList *l=pe;l;l=l->next) { Event *e=(Event*)l->data; GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0); gtk_widget_add_css_class(hbox,"event-entry"); GtkWidget *icon=gtk_image_new_from_icon_name("starred-symbolic"); gtk_widget_add_css_class(icon,"permanent-event-icon"); gchar *t12=format_time_to_12h(e->time); GtkWidget *tl=gtk_label_new(t12); g_free(t12); gtk_widget_add_css_class(tl,"event-time"); GtkWidget *title=gtk_label_new(e->title); gtk_label_set_xalign(GTK_LABEL(title),0.0); gtk_widget_add_css_class(title,"event-title"); gtk_box_append(GTK_BOX(hbox),icon); gtk_box_append(GTK_BOX(hbox),tl); gtk_box_append(GTK_BOX(hbox),title); gtk_box_append(GTK_BOX(vbox),hbox); } for (GList *l=re;l;l=l->next) { Event *e=(Event*)l->data; GtkWidget *hbox=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0); gtk_widget_add_css_class(hbox,"event-entry"); gchar *t12=format_time_to_12h(e->time); GtkWidget *tl=gtk_label_new(t12); g_free(t12); gtk_widget_add_css_class(tl,"event-time"); GtkWidget *title=gtk_label_new(e->title); gtk_label_set_xalign(GTK_LABEL(title),0.0); gtk_widget_add_css_class(title,"event-title"); gtk_widget_set_hexpand(title,TRUE); GtkWidget *db=gtk_button_new_from_icon_name("edit-delete-symbolic"); gtk_widget_add_css_class(db,"delete-button"); DeleteEventData *ded=g_new(DeleteEventData,1); ded->widget=w; ded->date_key=g_strdup(dkf); ded->event_to_delete=e; ded->popover=GTK_POPOVER(pop); g_signal_connect(db,"clicked",G_CALLBACK(on_delete_event_clicked),ded); g_object_set_data_full(G_OBJECT(db),"delete-data",ded,free_delete_event_data); gtk_box_append(GTK_BOX(hbox),tl); gtk_box_append(GTK_BOX(hbox),title); gtk_box_append(GTK_BOX(hbox),db); gtk_box_append(GTK_BOX(vbox),hbox); } gtk_widget_set_parent(pop, GTK_WIDGET(button)); g_signal_connect(pop, "closed", G_CALLBACK(destroy_popover_on_close), NULL); gtk_popover_popup(GTK_POPOVER(pop)); }
static void on_day_right_clicked(GtkGestureClick *g, int n, double x, double y, gpointer user_data) { (void)n;(void)x;(void)y; CalendarWidget *w = (CalendarWidget *)user_data; GtkWidget *b = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g)); const gchar *dk = g_object_get_data(G_OBJECT(b),"date-key"); g_object_set_data(G_OBJECT(w->add_event_popover),"date-key",(gpointer)dk); gtk_editable_set_text(GTK_EDITABLE(w->add_event_title_entry),""); gtk_check_button_set_active(w->add_event_allday_check,FALSE); gtk_drop_down_set_selected(w->add_event_hour_dropdown,8); gtk_drop_down_set_selected(w->add_event_minute_dropdown,0); gtk_drop_down_set_selected(w->add_event_ampm_dropdown,0); gtk_widget_set_parent(GTK_WIDGET(w->add_event_popover), b); gtk_popover_popup(GTK_POPOVER(w->add_event_popover)); }
static void on_prev_month_clicked(GtkButton *b, gpointer user_data) { (void)b; CalendarWidget *w = (CalendarWidget *)user_data; w->current_date = g_date_time_add_months(w->current_date, -1); start_grid_population(w); }
static void on_next_month_clicked(GtkButton *b, gpointer user_data) { (void)b; CalendarWidget *w = (CalendarWidget *)user_data; w->current_date = g_date_time_add_months(w->current_date, 1); start_grid_population(w); }
static void on_add_event_popover_closed(GtkPopover *p, gpointer user_data) { (void)user_data; if (gtk_widget_get_parent(GTK_WIDGET(p))) gtk_widget_unparent(GTK_WIDGET(p)); }

// --- CORE UI LOGIC (Unchanged) ---
static gboolean populate_one_day(gpointer user_data) { CalendarWidget *w = (CalendarWidget*)user_data; GridPopulationState *s = g_object_get_data(G_OBJECT(w->calendar_grid),"population-state"); if (!s || s->day_to_add > s->days_in_month) { if(s) g_free(s); g_object_set_data(G_OBJECT(w->calendar_grid),"population-state",NULL); w->grid_population_timer_id = 0; return G_SOURCE_REMOVE; } gchar l[4]; snprintf(l,4,"%d",s->day_to_add); GtkWidget *b = gtk_button_new_with_label(l); gtk_widget_add_css_class(b,"day-button"); gchar *dk=g_strdup_printf("%d-%d-%d",s->current_y,s->current_m,s->day_to_add), *pk=g_strdup_printf("%d-%d",s->current_m,s->day_to_add); g_object_set_data_full(G_OBJECT(b),"date-key",dk,(GDestroyNotify)g_free); if (s->day_to_add==s->today_d && s->current_m==s->today_m && s->current_y==s->today_y) gtk_widget_add_css_class(b,"today"); if ((w->events && g_hash_table_contains(w->events,dk)) || (w->permanent_events && g_hash_table_contains(w->permanent_events,pk))) gtk_widget_add_css_class(b,"has-event"); g_free(pk); g_signal_connect(b,"clicked",G_CALLBACK(on_day_left_clicked),w); GtkGesture *rg=gtk_gesture_click_new(); gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rg),GDK_BUTTON_SECONDARY); g_signal_connect(rg,"pressed",G_CALLBACK(on_day_right_clicked),w); gtk_widget_add_controller(b,GTK_EVENT_CONTROLLER(rg)); gtk_grid_attach(GTK_GRID(w->calendar_grid),b,s->grid_x,s->grid_y,1,1); s->grid_x++; if(s->grid_x > 6) { s->grid_x=0; s->grid_y++; } s->day_to_add++; return G_SOURCE_CONTINUE; }
static void start_grid_population(CalendarWidget *widget) { if (gtk_widget_get_parent(GTK_WIDGET(widget->add_event_popover))) gtk_popover_popdown(widget->add_event_popover); if (widget->grid_population_timer_id>0) { g_source_remove(widget->grid_population_timer_id); GridPopulationState *os = g_object_get_data(G_OBJECT(widget->calendar_grid),"population-state"); if(os) g_free(os); g_object_set_data(G_OBJECT(widget->calendar_grid),"population-state",NULL); } gchar *ms = g_date_time_format(widget->current_date,"%B %Y"); gtk_label_set_text(widget->month_label,ms); g_free(ms); GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(widget->calendar_grid)); while(child) { GtkWidget *next_child = gtk_widget_get_next_sibling(child); if (gtk_widget_has_css_class(child, "day-button")) gtk_grid_remove(GTK_GRID(widget->calendar_grid), child); child = next_child; } GridPopulationState *s=g_new0(GridPopulationState,1); s->day_to_add=1; s->current_y=g_date_time_get_year(widget->current_date); s->current_m=g_date_time_get_month(widget->current_date); s->days_in_month=g_date_get_days_in_month((GDateMonth)s->current_m,(GDateYear)s->current_y); GDateTime *fdom=g_date_time_new(g_date_time_get_timezone(widget->current_date),s->current_y,s->current_m,1,0,0,0); s->grid_x=g_date_time_get_day_of_week(fdom)%7; s->grid_y=1; g_date_time_unref(fdom); GDateTime *t=g_date_time_new_now_local(); s->today_y=g_date_time_get_year(t); s->today_m=g_date_time_get_month(t); s->today_d=g_date_time_get_day_of_month(t); g_date_time_unref(t); g_object_set_data(G_OBJECT(widget->calendar_grid),"population-state",s); widget->grid_population_timer_id=g_timeout_add(1,populate_one_day,widget); }
static GtkWidget* create_upcoming_event_row(UpcomingEvent *ue) { GtkWidget *r=gtk_list_box_row_new(); gtk_widget_add_css_class(r,"upcoming-row"); GtkWidget *h=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0); gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(r),h); GtkWidget *db=gtk_box_new(GTK_ORIENTATION_VERTICAL,0); gtk_widget_set_valign(db,GTK_ALIGN_CENTER); gtk_widget_add_css_class(db,"upcoming-date-box"); gchar *ds=g_date_time_format(ue->datetime,"%d"), *ms=g_date_time_format(ue->datetime,"%b"); GtkWidget *dl=gtk_label_new(ds), *ml=gtk_label_new(ms); gtk_widget_add_css_class(dl,"upcoming-date-day"); gtk_widget_add_css_class(ml,"upcoming-date-month"); gtk_box_append(GTK_BOX(db),dl); gtk_box_append(GTK_BOX(db),ml); g_free(ds); g_free(ms); GtkWidget *detb=gtk_box_new(GTK_ORIENTATION_VERTICAL,0); GtkWidget *title=gtk_label_new(ue->event->title); gtk_label_set_xalign(GTK_LABEL(title),0.0); gtk_widget_add_css_class(title,"upcoming-event-title"); gchar *t12=format_time_to_12h(ue->event->time); GtkWidget *tl=gtk_label_new(t12); g_free(t12); gtk_label_set_xalign(GTK_LABEL(tl),0.0); gtk_widget_add_css_class(tl,"upcoming-event-time"); gtk_box_append(GTK_BOX(detb),title); gtk_box_append(GTK_BOX(detb),tl); gtk_box_append(GTK_BOX(h),db); gtk_box_append(GTK_BOX(h),detb); return r; }
static gint sort_upcoming_events(gconstpointer a, gconstpointer b) { return g_date_time_compare(((UpcomingEvent*)a)->datetime, ((UpcomingEvent*)b)->datetime); }
static void populate_upcoming_events_list(CalendarWidget *widget) { if(!widget->events&&!widget->permanent_events)return; GtkWidget *child=gtk_widget_get_first_child(GTK_WIDGET(widget->upcoming_list_box)); while(child){GtkWidget *next=gtk_widget_get_next_sibling(child);gtk_list_box_remove(GTK_LIST_BOX(widget->upcoming_list_box),child);child=next;} GList *ul=NULL; GDateTime *n=g_date_time_new_now_local(),*t=g_date_time_new(g_date_time_get_timezone(n),g_date_time_get_year(n),g_date_time_get_month(n),g_date_time_get_day_of_month(n),0,0,0); if (widget->events) {GHashTableIter i; gpointer k,v; g_hash_table_iter_init(&i,widget->events); while(g_hash_table_iter_next(&i,&k,&v)){ int y,m,d; sscanf((gchar*)k,"%d-%d-%d",&y,&m,&d); GDateTime *ed=g_date_time_new(g_date_time_get_timezone(t),y,m,d,0,0,0); if(g_date_time_compare(ed,t)>=0) for(GList *l=(GList*)v;l;l=l->next){ UpcomingEvent *ue=g_new(UpcomingEvent,1); ue->datetime=g_date_time_ref(ed); ue->event=(Event*)l->data; ul=g_list_prepend(ul,ue); } g_date_time_unref(ed);}} if(widget->permanent_events){GHashTableIter i; gpointer k,v; g_hash_table_iter_init(&i,widget->permanent_events); while(g_hash_table_iter_next(&i,&k,&v)){ int m,d; sscanf((gchar*)k,"%d-%d",&m,&d); GDateTime *edty=g_date_time_new(g_date_time_get_timezone(t),g_date_time_get_year(t),m,d,0,0,0), *no=NULL; if(g_date_time_compare(edty,t)>=0) no=g_date_time_ref(edty); else no=g_date_time_add_years(edty,1); g_date_time_unref(edty); for(GList *l=(GList*)v;l;l=l->next){ UpcomingEvent *ue=g_new(UpcomingEvent,1); ue->datetime=g_date_time_ref(no); ue->event=(Event*)l->data; ul=g_list_prepend(ul,ue); } g_date_time_unref(no);}} g_date_time_unref(n); g_date_time_unref(t); ul=g_list_sort(ul,(GCompareFunc)sort_upcoming_events); for(GList *l=ul;l;l=l->next) gtk_list_box_append(widget->upcoming_list_box,create_upcoming_event_row((UpcomingEvent*)l->data)); g_list_free_full(ul,free_upcoming_event); }

// --- Cleanup (Unchanged) ---
static void on_widget_destroy(GtkWidget *gtk_widget, gpointer user_data) { (void)gtk_widget; CalendarWidget *w = (CalendarWidget *)user_data; if (w->grid_population_timer_id > 0) g_source_remove(w->grid_population_timer_id); GridPopulationState *s = g_object_get_data(G_OBJECT(w->calendar_grid), "population-state"); if (s) g_free(s); if (w->events) g_hash_table_unref(w->events); if (w->permanent_events) g_hash_table_unref(w->permanent_events); if (w->current_date) g_date_time_unref(w->current_date); g_free(w); }


// ==============================================================================
//                 THE FINAL, FULLY RESTORED `create_widget`
// ==============================================================================
G_MODULE_EXPORT GtkWidget* create_widget(const char *config_string) {
    // We no longer use the config string inside the plugin for styling.
    (void)config_string;

    CalendarWidget *widget_data = g_new0(CalendarWidget, 1);

    // This is the plugin's ONLY responsibility for styling: set a unique name.
    // The orchestrator (main.c) will use this hook.
    widget_data->main_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_name(widget_data->main_container, "calendar-widget");

    // --- ALL OF YOUR ORIGINAL UI-BUILDING LOGIC IS RESTORED BELOW ---

    gtk_widget_set_size_request(widget_data->main_container, WIDGET_WIDTH, -1);
    gtk_widget_set_halign(widget_data->main_container, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(widget_data->main_container, GTK_ALIGN_CENTER);
    
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(header_box, "header-box");
    gtk_box_append(GTK_BOX(widget_data->main_container), header_box);

    GtkWidget *prev_button = gtk_button_new_with_label("‹");
    gtk_widget_add_css_class(prev_button, "nav-button");
    gtk_box_append(GTK_BOX(header_box), prev_button);

    GtkWidget *month_menu_button = gtk_menu_button_new();
    gtk_widget_add_css_class(month_menu_button, "month-button");
    gtk_widget_set_hexpand(month_menu_button, TRUE);
    gtk_box_append(GTK_BOX(header_box), month_menu_button);

    widget_data->month_label = GTK_LABEL(gtk_label_new("..."));
    gtk_widget_add_css_class(GTK_WIDGET(widget_data->month_label), "month-label");
    gtk_menu_button_set_child(GTK_MENU_BUTTON(month_menu_button), GTK_WIDGET(widget_data->month_label));

    GtkWidget *next_button = gtk_button_new_with_label("›");
    gtk_widget_add_css_class(next_button, "nav-button");
    gtk_box_append(GTK_BOX(header_box), next_button);

    GtkWidget *upcoming_popover = gtk_popover_new();
    gtk_widget_add_css_class(upcoming_popover, "event-popover");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(month_menu_button), upcoming_popover);

    GtkWidget *panel_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(panel_container, "upcoming-panel");
    gtk_popover_set_child(GTK_POPOVER(upcoming_popover), panel_container);

    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_widget_set_size_request(scrolled_window, -1, 300);
    gtk_box_append(GTK_BOX(panel_container), scrolled_window);

    widget_data->upcoming_list_box = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(widget_data->upcoming_list_box), "upcoming-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), GTK_WIDGET(widget_data->upcoming_list_box));

    widget_data->calendar_grid = GTK_GRID(gtk_grid_new());
    gtk_widget_add_css_class(GTK_WIDGET(widget_data->calendar_grid), "calendar-grid");
    gtk_box_append(GTK_BOX(widget_data->main_container), GTK_WIDGET(widget_data->calendar_grid));

    const char *weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    for (int i=0; i < 7; i++) {
        GtkWidget *label = gtk_label_new(weekdays[i]);
        gtk_widget_add_css_class(label, "weekday-label");
        gtk_grid_attach(GTK_GRID(widget_data->calendar_grid), label, i, 0, 1, 1);
    }

    widget_data->add_event_popover = GTK_POPOVER(gtk_popover_new());
    gtk_widget_add_css_class(GTK_WIDGET(widget_data->add_event_popover), "event-popover");
    g_signal_connect(widget_data->add_event_popover, "closed", G_CALLBACK(on_add_event_popover_closed), NULL);

    GtkWidget *popover_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(popover_vbox, 10); gtk_widget_set_margin_end(popover_vbox, 10); gtk_widget_set_margin_top(popover_vbox, 10); gtk_widget_set_margin_bottom(popover_vbox, 10);
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
    
    // Connect only internal signals. main.c handles visibility.
    g_signal_connect(prev_button, "clicked", G_CALLBACK(on_prev_month_clicked), widget_data);
    g_signal_connect(next_button, "clicked", G_CALLBACK(on_next_month_clicked), widget_data);
    g_signal_connect(save_button, "clicked", G_CALLBACK(on_add_event_save), widget_data);
    g_signal_connect(widget_data->add_event_allday_check, "notify::active", G_CALLBACK(on_allday_toggled), widget_data);
    g_signal_connect(widget_data->main_container, "destroy", G_CALLBACK(on_widget_destroy), widget_data);
    
    // --- Load Data ---
    widget_data->current_date = g_date_time_new_now_local();
    GTask *task = g_task_new(NULL, NULL, on_data_loaded, widget_data);
    g_task_run_in_thread(task, load_data_in_thread);
    g_object_unref(task);

    return widget_data->main_container;
}