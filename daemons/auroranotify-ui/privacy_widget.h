#ifndef PRIVACY_WIDGET_H
#define PRIVACY_WIDGET_H

#include <gtk/gtk.h>

typedef void (*PrivacyStateChangedCb)(void);

void privacy_widget_init(PrivacyStateChangedCb callback);
void privacy_widget_update_from_json(const gchar *json_payload);
gboolean privacy_widget_has_active_apps(void);
GtkWidget* privacy_widget_create_pill(void);
GtkWidget* privacy_widget_create_dashboard(void);

// --- NEW: Dynamic update functions ---
void privacy_widget_refresh_ui(void);
gboolean privacy_widget_has_dashboard(void);

void privacy_widget_cleanup(void);

#endif // PRIVACY_WIDGET_H