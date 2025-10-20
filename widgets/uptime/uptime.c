#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <stdio.h>
#include <string.h>
#include <json-glib/json-glib.h>

typedef struct {
    GtkWidget *info_label;
    GtkWidget *main_container;
    gchar *format_string;
} UptimeWidget;

// (All helper functions are unchanged and correct)
static void unquote_string(gchar *str) {
    if (str == NULL) return;
    size_t len = strlen(str);
    if (len >= 2 && str[0] == '"' && str[len - 1] == '"') {
        memmove(str, str + 1, len - 2);
        str[len - 2] = '\0';
    }
}

static gboolean update_info(gpointer user_data) {
    UptimeWidget *widget = (UptimeWidget *)user_data;
    if (!widget->format_string) return G_SOURCE_CONTINUE;

    gchar *distro_name = NULL, *uptime_str_raw = NULL;
    
    gchar *os_release_contents;
    if (g_file_get_contents("/etc/os-release", &os_release_contents, NULL, NULL)) {
        const gchar* keys_to_try[] = { "PRETTY_NAME=", "NAME=", NULL };
        for (int k = 0; keys_to_try[k] != NULL && distro_name == NULL; k++) {
            gchar **lines = g_strsplit(os_release_contents, "\n", -1);
            for (int i = 0; lines[i] != NULL; i++) {
                if (g_str_has_prefix(lines[i], keys_to_try[k])) {
                    gchar **parts = g_strsplit(lines[i], "=", 2);
                    if (parts[1]) {
                        distro_name = g_strdup(parts[1]);
                        distro_name = g_strstrip(distro_name);
                        unquote_string(distro_name);
                    }
                    g_strfreev(parts);
                    break;
                }
            }
            g_strfreev(lines);
        }
        g_free(os_release_contents);
    }
    
    if (distro_name == NULL) distro_name = g_strdup("Unknown OS");

    g_spawn_command_line_sync("uptime -p", &uptime_str_raw, NULL, NULL, NULL);
    gchar *uptime_str = g_strstrip(uptime_str_raw);
    const char *prefix = "up ";
    gchar *uptime_clean = uptime_str;
    if (g_str_has_prefix(uptime_str, prefix)) uptime_clean += strlen(prefix);

    GString *final_text_gstring = g_string_new(widget->format_string);
    g_string_replace(final_text_gstring, "{distro}", distro_name, 0);
    g_string_replace(final_text_gstring, "{uptime}", uptime_clean, 0);
    
    gtk_label_set_text(GTK_LABEL(widget->info_label), final_text_gstring->str);

    g_string_free(final_text_gstring, TRUE);
    g_free(distro_name);
    g_free(uptime_str_raw);
    return G_SOURCE_CONTINUE;
}

// ===================================================================
//  Plugin Entry Point (Final Version)
// ===================================================================
//  Plugin Entry Point (Final, Corrected Version using GtkFrame)
// ===================================================================
G_MODULE_EXPORT GtkWidget* create_widget(const char *config_string) {
    // This part is for managing our widget's data. It stays the same.
    UptimeWidget *widget_data = g_new0(UptimeWidget, 1);
    widget_data->format_string = g_strdup("Uptime: {uptime}"); 

    // This part reads the "text" property from your config.json. It also stays the same.
    if (config_string && *config_string) {
        g_autoptr(JsonParser) parser = json_parser_new();
        if (json_parser_load_from_data(parser, config_string, -1, NULL)) {
            JsonObject *root_obj = json_node_get_object(json_parser_get_root(parser));
            if (json_object_has_member(root_obj, "text")) {
                g_free(widget_data->format_string);
                widget_data->format_string = g_strdup(
                    json_object_get_string_member(root_obj, "text")
                );
            }
        }
    }

    // --- START OF CHANGES ---

    // 1. Create a GtkFrame. This will be our new top-level widget.
    //    A GtkFrame is designed to be styled with borders, backgrounds, etc.
    GtkWidget *frame = gtk_frame_new(NULL);

    // 2. Apply your ID to the FRAME. This is the widget your CSS will style.
    gtk_widget_set_name(frame, "uptime-widget");
    
    // 3. Create the GtkBox like before. Its only job is now to arrange the label.
    //    It no longer needs any styling information.
    widget_data->main_container = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    
    // 4. Place the box inside the frame.
    gtk_frame_set_child(GTK_FRAME(frame), widget_data->main_container);
    
    // 5. Create the label and add its CSS class, just like before.
    widget_data->info_label = gtk_label_new("");
    gtk_widget_add_css_class(widget_data->info_label, "info-label");
    gtk_widget_set_margin_top(widget_data->info_label, 2);
    gtk_widget_set_margin_bottom(widget_data->info_label, 2);
    
    // 6. Add the label to the box.
    gtk_box_append(GTK_BOX(widget_data->main_container), widget_data->info_label);
    
    // This logic to update the text stays the same.
    update_info(widget_data);
    g_timeout_add_seconds(60, (GSourceFunc)update_info, widget_data);
    
    // 7. IMPORTANT: Connect the memory cleanup signals to the new top-level widget (the frame).
    //    This ensures that when the frame is destroyed, our data is freed.
    g_signal_connect_swapped(frame, "destroy", G_CALLBACK(g_free), widget_data->format_string);
    g_signal_connect_swapped(frame, "destroy", G_CALLBACK(g_free), widget_data);
    
    // 8. Return the FRAME, which is now the complete, styleable widget.
    return frame;

    // --- END OF CHANGES ---
}