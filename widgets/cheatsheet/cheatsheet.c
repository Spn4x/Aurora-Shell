#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <stdio.h>
#include <gdk/gdkkeysyms.h>
#include <json-glib/json-glib.h> // Keep includes, good practice

#define NUM_COLUMNS 3

// The state struct is now extremely simple.
typedef struct {
    GtkWidget *main_container;
} CheatsheetWidget;

// Helper functions for memory cleanup, unchanged.
static void free_category_list(gpointer data) { g_list_free_full((GList *)data, g_free); }
static void free_categories(GList *categories) { g_list_free_full(categories, free_category_list); }

// The file parsing logic is core to this plugin and remains unchanged.
static GList* parse_categories_from_file(void) {
    // NOTE: This hardcoded path could be made configurable in config.json in the future!
    const char *keys_file_path = "./widgets/cheatsheet/keys.conf";
    GList *categories = NULL, *current_category = NULL;
    FILE *file = fopen(keys_file_path, "r");
    if (!file) return NULL;

    char *line = NULL;
    size_t len = 0;
    while (getline(&line, &len, file) != -1) {
        gchar *stripped_line = g_strstrip(line);
        if (strlen(stripped_line) == 0 || g_str_has_prefix(stripped_line, "#")) continue;

        if (!strstr(stripped_line, "=")) { // This line is a header
            if (current_category) {
                categories = g_list_append(categories, current_category);
            }
            current_category = NULL;
            current_category = g_list_append(current_category, g_strdup(stripped_line));
        } else { // This line is a key-value pair
            if (current_category) {
                current_category = g_list_append(current_category, g_strdup(stripped_line));
            }
        }
    }

    if (current_category) {
        categories = g_list_append(categories, current_category);
    }
    
    fclose(file);
    if (line) free(line);
    
    return g_list_reverse(categories);
}

// UI creation for a single category. Now simpler without the CSS provider argument.
static GtkWidget* create_category_widget(GList *category_data) {
    gchar *header_text = (gchar*)category_data->data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(box, "category-box");

    GtkWidget *header_label = gtk_label_new(header_text);
    gtk_widget_set_halign(header_label, GTK_ALIGN_START);
    gtk_widget_add_css_class(header_label, "category-header");
    gtk_box_append(GTK_BOX(box), header_label);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 20);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);

    int row = 0;
    for (GList *l = g_list_next(category_data); l; l = g_list_next(l)) {
        gchar **parts = g_strsplit((gchar*)l->data, "=", 2);
        if (parts[0] && parts[1]) {
            GtkWidget *key = gtk_label_new(g_strstrip(parts[0]));
            gtk_widget_set_halign(key, GTK_ALIGN_END);
            gtk_widget_add_css_class(key, "key-label");

            GtkWidget *desc = gtk_label_new(g_strstrip(parts[1]));
            gtk_widget_set_halign(desc, GTK_ALIGN_START);
            gtk_widget_add_css_class(desc, "desc-label");

            gtk_grid_attach(GTK_GRID(grid), key, 0, row, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), desc, 1, row, 1, 1);
            row++;
        }
        g_strfreev(parts);
    }
    gtk_box_append(GTK_BOX(box), grid);
    return box;
}

// ===================================================================
//  Plugin Entry Point (Simplified and Unified Version)
// ===================================================================
G_MODULE_EXPORT GtkWidget* create_widget(const char *config_string) {
    (void)config_string; // This plugin doesn't need any config from the orchestrator yet.
    
    CheatsheetWidget *widget_data = g_new0(CheatsheetWidget, 1);
    
    // Create the main container.
    widget_data->main_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    // CRITICAL: Keep the original CSS class name so the external stylesheet works!
    gtk_widget_add_css_class(widget_data->main_container, "main-container");
    
    // --- All internal CSS loading has been REMOVED ---
    
    GtkWidget *content_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(content_grid), 30);
    gtk_widget_set_margin_top(content_grid, 20);
    gtk_widget_set_margin_bottom(content_grid, 20);
    gtk_widget_set_margin_start(content_grid, 25);
    gtk_widget_set_margin_end(content_grid, 25);
    gtk_widget_set_hexpand(content_grid, TRUE);
    gtk_box_append(GTK_BOX(widget_data->main_container), content_grid);

    // The Escape key is already handled by the orchestrator for interactive widgets,
    // so the internal key controller is redundant, but we can leave it for now.
    
    GList *categories = parse_categories_from_file();
    if (categories) {
        GtkWidget *columns[NUM_COLUMNS];
        for (int i = 0; i < NUM_COLUMNS; i++) {
            columns[i] = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
            gtk_grid_attach(GTK_GRID(content_grid), columns[i], i, 0, 1, 1);
        }
        int i = 0;
        for (GList *l = categories; l; l = g_list_next(l)) {
            // Call the simplified create_category_widget function
            gtk_box_append(GTK_BOX(columns[i++ % NUM_COLUMNS]), create_category_widget((GList*)l->data));
        }
        free_categories(categories);
    } else {
        gtk_grid_attach(GTK_GRID(content_grid), gtk_label_new("Could not load keys.conf"), 0, 0, 1, 1);
    }
    
    // Setup cleanup signals
    g_signal_connect_swapped(widget_data->main_container, "destroy", G_CALLBACK(g_free), widget_data);
    
    return widget_data->main_container;
}