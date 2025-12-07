#ifndef EDS_EVENT_SOURCE_H
#define EDS_EVENT_SOURCE_H

#include <glib-object.h>

// THE FINAL FIX: Include the main headers in the correct order.
// libedataserver provides the base objects that libecal depends on.
#include <libedataserver/libedataserver.h>
#include <libecal/libecal.h>

G_BEGIN_DECLS

typedef struct {
    gchar *uid;
    gchar *summary;
    GDateTime *dt_start;
    GDateTime *dt_end;
} EdsCalendarEvent;

void eds_calendar_event_free(gpointer data);

#define EDS_TYPE_EVENT_SOURCE (eds_event_source_get_type())
G_DECLARE_FINAL_TYPE(EdsEventSource, eds_event_source, EDS, EVENT_SOURCE, GObject)

EdsEventSource* eds_event_source_new(void);
GList* eds_event_source_get_events(EdsEventSource *self, GDateTime *start, GDateTime *end);
gboolean eds_event_source_has_events(EdsEventSource *self, GDateTime *day);

G_END_DECLS

#endif // EDS_EVENT_SOURCE_H