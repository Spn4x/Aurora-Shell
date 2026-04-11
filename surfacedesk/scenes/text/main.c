// FILE: ./scenes/text/main.c
#include <gtk/gtk.h>

GtkWidget* scene_create_custom_label(void) {
    GtkWidget *wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(wrapper, "card");
    gtk_widget_add_css_class(wrapper, "transparent"); 

    GtkWidget *label = gtk_label_new("Edit Me");
    gtk_widget_add_css_class(label, "title-1");
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_vexpand(label, TRUE);
    
    // Allow wrapping for long text
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 20);

    // FIX: Set name for CSS targeting in drawer.c
    gtk_widget_set_name(label, "custom-label-widget");

    gtk_box_append(GTK_BOX(wrapper), label);
    
    // Also set data for text updating logic
    g_object_set_data(G_OBJECT(wrapper), "custom-label-widget", label);
    
    return wrapper;
}