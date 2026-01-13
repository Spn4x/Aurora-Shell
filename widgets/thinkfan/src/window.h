#pragma once
#include <gtk/gtk.h>
#include <adwaita.h>

G_BEGIN_DECLS

#define THINKFAN_TYPE_WIDGET (thinkfan_widget_get_type())
G_DECLARE_FINAL_TYPE(ThinkfanWidget, thinkfan_widget, THINKFAN, WIDGET, GtkBox)

GtkWidget* thinkfan_widget_new(void);

G_END_DECLS