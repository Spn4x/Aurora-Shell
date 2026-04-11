// FILE: ./src/widget_template.c
#include "widget_template.h"
#include "../scenes/registry.h"
#include "globals.h"
#include <math.h>

#define WIDGET_GUTTER 12

GtkWidget* create_widget_content(const char* type, int variant_index) {
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *content = NULL;

    const SceneFamily *fam = scene_registry_lookup_family(type);

    if (fam && variant_index >= 0 && variant_index < fam->variant_count) {
        content = fam->variants[variant_index].factory();
    } else {
        content = gtk_label_new("Invalid Widget");
    }

    if (!content) content = gtk_label_new("Error");

    // FIX 1: Use GTK_ALIGN_FILL.
    // This ensures the widget tries to fill the allocated space (the grid slot).
    gtk_widget_set_halign(content, GTK_ALIGN_FILL);
    gtk_widget_set_valign(content, GTK_ALIGN_FILL);

    // Ensure it grabs available space
    gtk_widget_set_hexpand(content, TRUE);
    gtk_widget_set_vexpand(content, TRUE);

    gtk_box_append(GTK_BOX(frame), content);

    gtk_widget_add_css_class(frame, "tile-container");
    gtk_widget_set_can_target(frame, FALSE);

    // Allow shadows to render outside
    gtk_widget_set_overflow(frame, GTK_OVERFLOW_VISIBLE);

    // FIX 2: RE-INJECT THE CSS HACK.
    // This overrides the widget's internal minimum size calculation.
    // It tells GTK: "This widget is allowed to shrink to 0px if the container demands it."
    // Without this, the widget ignores size requests smaller than its content.
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
        ".tile-container > * { min-width: 0px; min-height: 0px; }");
    GtkStyleContext *context = gtk_widget_get_style_context(frame);
    gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    return frame;
}

void update_widget_geometry(Box *b, GtkWidget *fixed) {
    if (!b || !b->widget || !fixed) return;

    // 1. Calculate Dynamic Gutter
    double cell = (double)app_state.physics.cell_size;
    int gutter = (int)round(cell * 0.12);
    if (gutter < 4) gutter = 4;
    if (gutter > 24) gutter = 24;

    // 2. The exact size of the hole in the grid
    int grid_slot_w = (int)b->vis_w - gutter;
    int grid_slot_h = (int)b->vis_h - gutter;

    if (grid_slot_w < 1) grid_slot_w = 1;
    if (grid_slot_h < 1) grid_slot_h = 1;

    // 3. Force the widget to this size.
    // Thanks to the CSS hack above, this request will now be respected
    // even if the text/image inside wants to be larger.
    gtk_widget_set_size_request(b->widget, grid_slot_w, grid_slot_h);

    // 4. Center the widget in the slot (handling the gutter offset)
    double final_x = b->vis_x + app_state.physics.current_offset_x + (gutter / 2.0);
    double final_y = b->vis_y + app_state.physics.current_offset_y + (gutter / 2.0);

    gtk_fixed_move(GTK_FIXED(fixed), b->widget, final_x, final_y);
}
