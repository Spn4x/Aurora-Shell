#ifndef NOTIFICATION_WIDGET_H
#define NOTIFICATION_WIDGET_H

#include <gtk/gtk.h>

typedef struct {
    guint32 id;
    gchar *icon;
    gchar *summary;
    gchar *body;
    gchar **actions;
} NotificationData;

typedef gboolean (*DismissFunc)(gpointer user_data);

void free_notification_data(gpointer data);

// New helper to let the main UI know if it should disable the timeout
gboolean notification_has_actions(NotificationData *data);

GtkWidget* notification_widget_create_expanded(NotificationData *data, DismissFunc dismiss_cb);

#endif // NOTIFICATION_WIDGET_H