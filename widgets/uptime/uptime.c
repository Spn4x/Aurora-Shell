// widgets/uptime/uptime.c

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

// --- Rust FFI ---
typedef struct {
    char *os_name;
    char *uptime_str;
} UptimeData;

extern UptimeData* aurora_uptime_get_data();
extern void aurora_uptime_free_data(UptimeData *data);

// --- Widget State ---
typedef struct {
    GtkWidget *info_label;
    GtkWidget *main_container;
    gchar *format_string;
} UptimeWidget;

// --- Logic ---

static gboolean update_info(gpointer user_data) {
    UptimeWidget *widget = (UptimeWidget *)user_data;
    if (!widget->format_string) return G_SOURCE_CONTINUE;

    // Call Rust
    UptimeData *data = aurora_uptime_get_data();
    if (!data) return G_SOURCE_CONTINUE;

    // Format the final string for the label
    GString *final_text = g_string_new(widget->format_string);
    g_string_replace(final_text, "{distro}", data->os_name, 0);
    g_string_replace(final_text, "{uptime}", data->uptime_str, 0);
    
    gtk_label_set_text(GTK_LABEL(widget->info_label), final_text->str);

    // Cleanup
    g_string_free(final_text, TRUE);
    aurora_uptime_free_data(data);
    
    return G_SOURCE_CONTINUE;
}

// --- Entry Point ---

G_MODULE_EXPORT GtkWidget* create_widget(const char *config_string) {
    UptimeWidget *widget_data = g_new0(UptimeWidget, 1);
    widget_data->format_string = g_strdup("Uptime: {uptime}"); 

    if (config_string && *config_string) {
        g_autoptr(JsonParser) parser = json_parser_new();
        if (json_parser_load_from_data(parser, config_string, -1, NULL)) {
            JsonObject *root_obj = json_node_get_object(json_parser_get_root(parser));
            if (json_object_has_member(root_obj, "text")) {
                g_free(widget_data->format_string);
                widget_data->format_string = g_strdup(json_object_get_string_member(root_obj, "text"));
            }
        }
    }

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_widget_set_name(frame, "uptime-widget");
    
    widget_data->main_container = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_frame_set_child(GTK_FRAME(frame), widget_data->main_container);
    
    widget_data->info_label = gtk_label_new("Loading...");
    gtk_widget_add_css_class(widget_data->info_label, "info-label");
    gtk_widget_set_margin_top(widget_data->info_label, 2);
    gtk_widget_set_margin_bottom(widget_data->info_label, 2);
    
    gtk_box_append(GTK_BOX(widget_data->main_container), widget_data->info_label);
    
    // Initial update
    update_info(widget_data);
    
    // Update every minute
    g_timeout_add_seconds(60, (GSourceFunc)update_info, widget_data);
    
    g_signal_connect_swapped(frame, "destroy", G_CALLBACK(g_free), widget_data->format_string);
    g_signal_connect_swapped(frame, "destroy", G_CALLBACK(g_free), widget_data);
    
    return frame;
}