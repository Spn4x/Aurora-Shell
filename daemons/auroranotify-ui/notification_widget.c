#include "notification_widget.h"
#include <gio/gio.h>

void free_notification_data(gpointer data) {
    if (!data) return;
    NotificationData *notif = (NotificationData *)data;
    g_free(notif->icon);
    g_free(notif->summary);
    g_free(notif->body);
    if (notif->actions) g_strfreev(notif->actions);
    g_free(notif);
}

gboolean notification_has_actions(NotificationData *data) {
    if (!data || !data->actions) return FALSE;
    for (int i = 0; data->actions[i] != NULL && data->actions[i+1] != NULL; i += 2) {
        if (g_strcmp0(data->actions[i], "default") != 0) return TRUE;
    }
    return FALSE;
}

static gboolean reveal_pill_cb(gpointer user_data) {
    GtkWidget **weak_ptr = (GtkWidget **)user_data;
    if (*weak_ptr) {
        gtk_widget_remove_css_class(*weak_ptr, "pill-hidden");
        g_object_remove_weak_pointer(G_OBJECT(*weak_ptr), (gpointer *)weak_ptr);
    }
    g_free(weak_ptr);
    return G_SOURCE_REMOVE;
}

static void schedule_pill_reveal(GtkWidget *btn, guint delay_ms) {
    GtkWidget **weak_ptr = g_new(GtkWidget *, 1);
    *weak_ptr = btn;
    g_object_add_weak_pointer(G_OBJECT(btn), (gpointer *)weak_ptr);
    g_timeout_add(delay_ms, reveal_pill_cb, weak_ptr);
}

static void on_action_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    const char *action_id = g_object_get_data(G_OBJECT(btn), "action-id");
    guint32 notif_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "notif-id"));
    DismissFunc dismiss_cb = (DismissFunc)g_object_get_data(G_OBJECT(btn), "dismiss-cb");
    
    g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (bus) {
        g_dbus_connection_call(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                               "org.freedesktop.Notifications", "InvokeAction",
                               g_variant_new("(us)", notif_id, action_id),
                               NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
    if (dismiss_cb) dismiss_cb(NULL);
}

GtkWidget* notification_widget_create_expanded(NotificationData *data, DismissFunc dismiss_cb) {
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_valign(main_box, GTK_ALIGN_START);

    // 1. Text Section
    GtkWidget *summary = gtk_label_new(data->summary);
    gtk_widget_set_halign(summary, GTK_ALIGN_START); 
    gtk_widget_add_css_class(summary, "summary");
    gtk_label_set_wrap(GTK_LABEL(summary), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(summary), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(summary), 40);

    GtkWidget *body = gtk_label_new(data->body);
    gtk_widget_set_halign(body, GTK_ALIGN_START); 
    gtk_label_set_justify(GTK_LABEL(body), GTK_JUSTIFY_LEFT);
    gtk_widget_add_css_class(body, "body");
    gtk_label_set_wrap(GTK_LABEL(body), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(body), PANGO_WRAP_WORD_CHAR); 
    gtk_label_set_max_width_chars(GTK_LABEL(body), 40); 
    gtk_label_set_lines(GTK_LABEL(body), 6); 
    gtk_label_set_ellipsize(GTK_LABEL(body), PANGO_ELLIPSIZE_END);

    gtk_box_append(GTK_BOX(main_box), summary);
    gtk_box_append(GTK_BOX(main_box), body);

    // 2. Actions Section
    if (notification_has_actions(data)) {
        GtkWidget *actions_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_top(actions_box, 6);

        int count = 0;
        for (int i = 0; data->actions[i] != NULL && data->actions[i+1] != NULL; i += 2) {
            if (g_strcmp0(data->actions[i], "default") == 0) continue;

            GtkWidget *btn = gtk_button_new_with_label(data->actions[i+1]);
            gtk_widget_add_css_class(btn, "action-pill");
            gtk_widget_add_css_class(btn, "pill-hidden");

            g_object_set_data_full(G_OBJECT(btn), "action-id", g_strdup(data->actions[i]), g_free);
            g_object_set_data(G_OBJECT(btn), "notif-id", GUINT_TO_POINTER(data->id));
            g_object_set_data(G_OBJECT(btn), "dismiss-cb", dismiss_cb);
            g_signal_connect(btn, "clicked", G_CALLBACK(on_action_clicked), NULL);

            gtk_box_append(GTK_BOX(actions_box), btn);
            // --- FIX: Adjusted for faster animation ---
            schedule_pill_reveal(btn, 300 + (count * 50)); 
            count++;
        }
        gtk_box_append(GTK_BOX(main_box), actions_box);
    }

    return main_box;
}