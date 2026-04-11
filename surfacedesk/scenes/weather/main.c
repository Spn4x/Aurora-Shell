// FILE: ./scenes/weather/main.c
#include <gtk/gtk.h>

GtkWidget* scene_create_weather(int w, int h) {
    (void)w; (void)h; // Mark as unused for now

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    GtkWidget *icon = gtk_image_new_from_icon_name("weather-clear-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 64);
    gtk_box_append(GTK_BOX(box), icon);

    GtkWidget *temp = gtk_label_new("24°C");
    gtk_widget_add_css_class(temp, "title-large");
    gtk_box_append(GTK_BOX(box), temp);
    
    GtkWidget *desc = gtk_label_new("Sunny");
    gtk_widget_set_opacity(desc, 0.7);
    gtk_box_append(GTK_BOX(box), desc);

    return box;
}