#include "eds_event_source.h"
#include <gio/gio.h>

// --- Data Structures ---
typedef struct {
    ECalClient *client;
} ClientBundle;

struct _EdsEventSource {
    GObject parent_instance;
    ESourceRegistry *registry;
    GList *client_bundles;
    gboolean is_loading;
};

G_DEFINE_TYPE(EdsEventSource, eds_event_source, G_TYPE_OBJECT)

enum { SIG_CHANGED, LAST_SIGNAL };
static guint signals[LAST_SIGNAL] = { 0 };

// --- Forward Declarations ---
static void eds_event_source_finalize(GObject *object);
static void find_and_setup_calendars_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
static void on_calendars_ready(GObject *source_object, GAsyncResult *res, gpointer user_data);
static void free_client_bundle(gpointer data);

// --- Memory Management ---
void eds_calendar_event_free(gpointer data) {
    if (!data) return;
    EdsCalendarEvent *event = data;
    g_free(event->uid);
    g_free(event->summary);
    if (event->dt_start) g_date_time_unref(event->dt_start);
    if (event->dt_end) g_date_time_unref(event->dt_end);
    g_free(event);
}

static void free_client_bundle(gpointer data) {
    ClientBundle *bundle = data;
    if (!bundle) return;
    
    if (bundle->client && G_IS_OBJECT(bundle->client)) {
        g_object_unref(bundle->client);
    }
    g_free(bundle);
}

// --- GObject Implementation ---
static void eds_event_source_class_init(EdsEventSourceClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = eds_event_source_finalize;
    signals[SIG_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void eds_event_source_init(EdsEventSource *self) {
    self->is_loading = TRUE;
    GTask *task = g_task_new(self, NULL, on_calendars_ready, self);
    g_task_run_in_thread(task, find_and_setup_calendars_thread);
    g_object_unref(task);
}

static void eds_event_source_finalize(GObject *object) {
    EdsEventSource *self = EDS_EVENT_SOURCE(object);
    g_list_free_full(self->client_bundles, free_client_bundle);
    g_clear_object(&self->registry);
    G_OBJECT_CLASS(eds_event_source_parent_class)->finalize(object);
}

static void on_calendars_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    EdsEventSource *self = user_data;
    (void)source_object;
    g_autoptr(GError) error = NULL;
    GList *bundles = g_task_propagate_pointer(G_TASK(res), &error);
    if (error) {
        g_warning("Failed to find and set up calendars: %s", error->message);
    } else {
        self->client_bundles = bundles;
    }
    self->is_loading = FALSE;
    g_signal_emit(self, signals[SIG_CHANGED], 0);
}

static void find_and_setup_calendars_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    EdsEventSource *self = source_object;
    (void)task_data; (void)cancellable;
    g_autoptr(GError) error = NULL;
    GList *bundles = NULL;

    self->registry = e_source_registry_new_sync(NULL, &error);
    if (error) {
        g_task_return_error(task, g_error_copy(error));
        return;
    }
    
    g_autoptr(GList) sources = e_source_registry_list_sources(self->registry, E_SOURCE_EXTENSION_CALENDAR);
    for (GList *l = sources; l != NULL; l = l->next) {
        ESource *source = l->data;
        if (!e_source_get_enabled(source)) continue;

        g_autoptr(GError) client_error = NULL;
        
        ECalClient *client = E_CAL_CLIENT(e_cal_client_connect_sync(source, E_CAL_CLIENT_SOURCE_TYPE_EVENTS, 5, NULL, &client_error));
        if (client_error) {
            continue;
        }

        if (!e_client_open_sync(E_CLIENT(client), FALSE, NULL, &client_error)) {
            g_object_unref(client);
            continue;
        }

        ClientBundle *bundle = g_new0(ClientBundle, 1);
        bundle->client = client;
        
        bundles = g_list_prepend(bundles, bundle);
    }
    g_task_return_pointer(task, bundles, NULL);
}

EdsEventSource* eds_event_source_new(void) {
    return g_object_new(EDS_TYPE_EVENT_SOURCE, NULL);
}

GList* eds_event_source_get_events(EdsEventSource *self, GDateTime *start, GDateTime *end) {
    g_return_val_if_fail(EDS_IS_EVENT_SOURCE(self), NULL);

    if (self->is_loading) {
        return NULL;
    }

    GList *results = NULL;
    const gchar *query = "#t"; // Get all events, filter manually

    for (GList *l = self->client_bundles; l != NULL; l = l->next) {
        ClientBundle *bundle = l->data;
        g_autoptr(GError) error = NULL;
        GSList *objects = NULL; 

        if (!e_cal_client_get_object_list_sync(bundle->client, query, &objects, NULL, &error)) {
            if (objects) g_slist_free(objects);
            continue;
        }
        
        for (GSList *obj_link = objects; obj_link != NULL; obj_link = obj_link->next) {
            ICalComponent *icalcomp = obj_link->data;
            ECalComponent *comp = e_cal_component_new_from_icalcomponent(icalcomp);

            ECalComponentText *summary_obj = e_cal_component_get_summary(comp);
            ECalComponentDateTime *dtstart_obj = e_cal_component_get_dtstart(comp);
            ECalComponentDateTime *dtend_obj = e_cal_component_get_dtend(comp);

            if (!summary_obj || !dtstart_obj || !dtend_obj) {
                g_object_unref(comp);
                continue;
            }

            ICalTime *ical_dtstart = e_cal_component_datetime_get_value(dtstart_obj);
            ICalTime *ical_dtend = e_cal_component_datetime_get_value(dtend_obj);
            
            g_autoptr(GDateTime) event_start = g_date_time_new_local(
                i_cal_time_get_year(ical_dtstart),
                i_cal_time_get_month(ical_dtstart),
                i_cal_time_get_day(ical_dtstart),
                i_cal_time_get_hour(ical_dtstart),
                i_cal_time_get_minute(ical_dtstart),
                i_cal_time_get_second(ical_dtstart)
            );

            g_autoptr(GDateTime) event_end = g_date_time_new_local(
                i_cal_time_get_year(ical_dtend),
                i_cal_time_get_month(ical_dtend),
                i_cal_time_get_day(ical_dtend),
                i_cal_time_get_hour(ical_dtend),
                i_cal_time_get_minute(ical_dtend),
                i_cal_time_get_second(ical_dtend)
            );

            g_autoptr(GDateTime) effective_end = NULL;
            gboolean is_all_day = i_cal_time_is_date(ical_dtstart);

            if (is_all_day) {
                effective_end = g_date_time_add_seconds(event_end, -1);
            } else {
                effective_end = g_date_time_ref(event_end);
            }

            if (g_date_time_compare(event_start, end) < 0 && g_date_time_compare(effective_end, start) > 0) {
                EdsCalendarEvent *event = g_new0(EdsCalendarEvent, 1);
                event->uid = g_strdup(e_cal_component_get_uid(comp));
                event->summary = g_strdup(e_cal_component_text_get_value(summary_obj));
                event->dt_start = g_date_time_ref(event_start);
                event->dt_end = g_date_time_ref(event_end);

                results = g_list_prepend(results, event);
            }
            
            g_object_unref(comp);
        }
        g_slist_free(objects);
    }

    return g_list_sort(results, (GCompareFunc)g_date_time_compare);
}

gboolean eds_event_source_has_events(EdsEventSource *self, GDateTime *day) {
    g_return_val_if_fail(EDS_IS_EVENT_SOURCE(self), FALSE);

    if (self->is_loading) {
        return FALSE;
    }

    g_autoptr(GDateTime) start_of_day = g_date_time_new_local(
        g_date_time_get_year(day), g_date_time_get_month(day), g_date_time_get_day_of_month(day), 0, 0, 0);
    g_autoptr(GDateTime) end_of_day = g_date_time_add_days(start_of_day, 1);

    GList *events = eds_event_source_get_events(self, start_of_day, end_of_day);
    if (events) {
        g_list_free_full(events, eds_calendar_event_free);
        return TRUE;
    }
    return FALSE;
}