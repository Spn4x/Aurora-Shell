// FILE: ./src/drawer.c
#include "drawer.h"
#include "widget_template.h"
#include "../scenes/registry.h"
#include "globals.h"
#include "storage.h"
#include "editor.h" 
#include <math.h>

#define PAGE_FAMILIES "families"
#define PAGE_VARIANTS "variants"
#define PAGE_CONFIG   "config"

extern void start_animation(void);

static GtkWidget *config_container = NULL; 
static GtkWidget *variants_flowbox = NULL;
static GtkWidget *lbl_family_title = NULL;
static const SceneFamily *selected_family = NULL;
static guint save_timeout_id = 0;

static gboolean do_delayed_save(gpointer data) {
    (void)data;
    save_layout();
    save_timeout_id = 0;
    return G_SOURCE_REMOVE;
}

static void schedule_save(void) {
    if (save_timeout_id > 0) g_source_remove(save_timeout_id);
    save_timeout_id = g_timeout_add(1000, do_delayed_save, NULL); 
}

static void apply_css_with_key(GtkWidget *widget, const char *css, const char *data_key) {
    GtkCssProvider *p = g_object_get_data(G_OBJECT(widget), data_key);
    if (!p) {
        p = gtk_css_provider_new();
        g_object_set_data_full(G_OBJECT(widget), data_key, p, g_object_unref);
        
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        GtkStyleContext *ctx = gtk_widget_get_style_context(widget);
        gtk_style_context_add_provider(ctx, GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_USER);
        G_GNUC_END_IGNORE_DEPRECATIONS
    }
    gtk_css_provider_load_from_string(p, css);
}

static void apply_css_to_widget(GtkWidget *widget, const char *css) {
    apply_css_with_key(widget, css, "custom-style-provider");
}

static void recursive_apply_css(GtkWidget *parent, const char *target_name, const char *css) {
    const char *wname = gtk_widget_get_name(parent);
    if (g_strcmp0(target_name, "*") == 0 || g_strcmp0(wname, target_name) == 0) apply_css_to_widget(parent, css);
    for (GtkWidget *child = gtk_widget_get_first_child(parent); child != NULL; child = gtk_widget_get_next_sibling(child)) {
        recursive_apply_css(child, target_name, css);
    }
}

void apply_box_settings(Box *b) {
    if (!b || !b->widget) return;
    
    GtkWidget *card = gtk_widget_get_first_child(b->widget);
    if (!card) return;

    if (b->is_transparent) gtk_widget_add_css_class(card, "transparent");
    else gtk_widget_remove_css_class(card, "transparent");

    if (b->use_dominant_color) gtk_widget_add_css_class(card, "dominant-text");
    else gtk_widget_remove_css_class(card, "dominant-text");

    if (b->padding >= 0) {
        g_autofree char *css = g_strdup_printf(".card { padding: %dpx; }", b->padding);
        apply_css_with_key(card, css, "padding-style-provider");
    }

    g_object_set_data(G_OBJECT(card), "clock-is-24h", b->is_24h ? GINT_TO_POINTER(1) : GINT_TO_POINTER(2));

    if (g_strcmp0(b->type, "Label") == 0 && b->custom_text) {
        GtkWidget *lbl = g_object_get_data(G_OBJECT(card), "custom-label-widget");
        if (!lbl) lbl = gtk_widget_get_first_child(card); 
        if (GTK_IS_LABEL(lbl)) gtk_label_set_text(GTK_LABEL(lbl), b->custom_text);
    }

    if (g_strcmp0(b->type, "Clock") == 0) {
        if (b->font_size_time > 0) {
            g_autofree char *css = g_strdup_printf("* { font-size: %dpx; }", b->font_size_time);
            recursive_apply_css(card, "clock-time", css);
        }
        if (b->font_size_date > 0) {
            g_autofree char *css = g_strdup_printf("* { font-size: %dpx; }", b->font_size_date);
            recursive_apply_css(card, "clock-date", css);
        }
    } 
    else { 
        if (b->font_size_time > 0) {
            g_autofree char *css = g_strdup_printf("* { font-size: %dpx; }", b->font_size_time);
            recursive_apply_css(card, "*", css); 
        }
    }
}

static void on_drawer_close_clicked(GtkButton *btn, gpointer user_data) {
    gtk_revealer_set_reveal_child(GTK_REVEALER(app_state.drawer_revealer), FALSE);
    if (app_state.show_drawer_btn) gtk_widget_set_visible(app_state.show_drawer_btn, TRUE);
    if (save_timeout_id > 0) { g_source_remove(save_timeout_id); save_layout(); }
}

static void on_grid_scale_changed(GtkSpinButton *spin, gpointer user_data) {
    int new_size = (int)gtk_spin_button_get_value(spin);
    if (new_size == app_state.physics.cell_size) return;
    app_state.physics.cell_size = new_size;
    PhysicsContext *ctx = &app_state.physics;
    int win_w = gtk_widget_get_width(ctx->overlay_draw);
    int win_h = gtk_widget_get_height(ctx->overlay_draw);
    if (win_w > 0) {
        int cols = floor((double)win_w / new_size);
        int rows = floor((double)win_h / new_size);
        ctx->target_offset_x = (double)(win_w - (cols * new_size)) / 2.0;
        ctx->target_offset_y = (double)(win_h - (rows * new_size)) / 2.0;
    }
    for (GList *l = ctx->boxes; l != NULL; l = l->next) {
        Box *b = (Box*)l->data;
        b->target_x = (b->grid_x * new_size) + b->nudge_x;
        b->target_y = (b->grid_y * new_size) + b->nudge_y;
        b->target_w = b->grid_w * new_size;
        b->target_h = b->grid_h * new_size;
    }
    start_animation();
    schedule_save();
}

static void on_width_changed(GtkSpinButton *spin, gpointer user_data) {
    Box *b = (Box*)user_data; b->grid_w = (int)gtk_spin_button_get_value(spin);
    b->target_w = b->grid_w * app_state.physics.cell_size; b->vis_w = b->target_w; 
    update_widget_geometry_safe(b); start_animation(); schedule_save();
}

static void on_height_changed(GtkSpinButton *spin, gpointer user_data) {
    Box *b = (Box*)user_data; b->grid_h = (int)gtk_spin_button_get_value(spin);
    b->target_h = b->grid_h * app_state.physics.cell_size; b->vis_h = b->target_h;
    update_widget_geometry_safe(b); start_animation(); schedule_save();
}

static void on_padding_changed(GtkSpinButton *spin, gpointer user_data) {
    Box *b = (Box*)user_data; b->padding = (int)gtk_spin_button_get_value(spin);
    apply_box_settings(b); schedule_save();
}

static void on_transparent_toggled(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
    Box *b = (Box*)user_data; b->is_transparent = gtk_switch_get_active(sw);
    apply_box_settings(b); schedule_save();
}

static void on_dominant_toggled(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
    Box *b = (Box*)user_data; b->use_dominant_color = gtk_switch_get_active(sw);
    apply_box_settings(b); schedule_save();
}

static void on_24h_toggled(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
    Box *b = (Box*)user_data; b->is_24h = gtk_switch_get_active(sw);
    apply_box_settings(b); schedule_save();
}

static void on_time_size_changed(GtkSpinButton *spin, gpointer user_data) {
    Box *b = (Box*)user_data; b->font_size_time = (int)gtk_spin_button_get_value(spin);
    apply_box_settings(b); schedule_save();
}

static void on_date_size_changed(GtkSpinButton *spin, gpointer user_data) {
    Box *b = (Box*)user_data; b->font_size_date = (int)gtk_spin_button_get_value(spin);
    apply_box_settings(b); schedule_save();
}

static void on_generic_size_changed(GtkSpinButton *spin, gpointer user_data) {
    Box *b = (Box*)user_data; b->font_size_time = (int)gtk_spin_button_get_value(spin); 
    apply_box_settings(b); schedule_save();
}

static void on_text_changed(GtkEditable *editable, gpointer user_data) {
    Box *b = (Box*)user_data;
    if (b->custom_text) g_free(b->custom_text);
    b->custom_text = g_strdup(gtk_editable_get_text(editable));
    apply_box_settings(b); schedule_save(); 
}

typedef struct { Box *box; int dx; int dy; } NudgeData;
static void on_nudge_clicked(GtkButton *btn, gpointer user_data) {
    NudgeData *nd = user_data; Box *b = nd->box;
    b->nudge_x += nd->dx; b->nudge_y += nd->dy;
    b->target_x = (b->grid_x * app_state.physics.cell_size) + b->nudge_x;
    b->target_y = (b->grid_y * app_state.physics.cell_size) + b->nudge_y;
    start_animation(); schedule_save();
}

static void add_nudge_btn(GtkWidget *row, Box *b, const char *icon, int dx, int dy) {
    GtkWidget *btn = gtk_button_new_from_icon_name(icon);
    NudgeData *nd = g_new(NudgeData, 1);
    nd->box = b; nd->dx = dx; nd->dy = dy;
    g_signal_connect_data(btn, "clicked", G_CALLBACK(on_nudge_clicked), nd, (GClosureNotify)g_free, 0);
    gtk_box_append(GTK_BOX(row), btn);
}

static void on_back_from_config(GtkButton *btn, gpointer user_data) {
    drawer_show_library(); app_state.physics.selected_box = NULL;
    gtk_widget_queue_draw(app_state.physics.overlay_draw);
}

void drawer_show_library(void) {
    gtk_stack_set_visible_child_name(GTK_STACK(app_state.drawer_stack), PAGE_FAMILIES);
    selected_family = NULL;
}

void drawer_show_config(Box *b) {
    if (!config_container) return;
    
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(config_container)) != NULL) 
        gtk_box_remove(GTK_BOX(config_container), child);

    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *back = gtk_button_new_from_icon_name("go-previous-symbolic");
    g_signal_connect(back, "clicked", G_CALLBACK(on_back_from_config), NULL);
    gtk_box_append(GTK_BOX(header_box), back);
    
    GtkWidget *title = gtk_label_new(b->type);
    gtk_widget_add_css_class(title, "title-2");
    gtk_box_append(GTK_BOX(header_box), title);
    gtk_box_append(GTK_BOX(config_container), header_box);
    gtk_box_append(GTK_BOX(config_container), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *lbl_dim = gtk_label_new("Grid Size");
    gtk_widget_set_halign(lbl_dim, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(config_container), lbl_dim);
    GtkWidget *row_dim = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    
    GtkAdjustment *adj_w = gtk_adjustment_new(b->grid_w, b->min_w, 20, 1, 1, 0);
    GtkWidget *spin_w = gtk_spin_button_new(adj_w, 1, 0);
    gtk_widget_set_hexpand(spin_w, TRUE);
    gtk_box_append(GTK_BOX(row_dim), gtk_label_new("W:"));
    gtk_box_append(GTK_BOX(row_dim), spin_w);
    g_signal_connect(spin_w, "value-changed", G_CALLBACK(on_width_changed), b);

    GtkAdjustment *adj_h = gtk_adjustment_new(b->grid_h, b->min_h, 20, 1, 1, 0);
    GtkWidget *spin_h = gtk_spin_button_new(adj_h, 1, 0);
    gtk_widget_set_hexpand(spin_h, TRUE);
    gtk_box_append(GTK_BOX(row_dim), gtk_label_new("H:"));
    gtk_box_append(GTK_BOX(row_dim), spin_h);
    g_signal_connect(spin_h, "value-changed", G_CALLBACK(on_height_changed), b);

    gtk_box_append(GTK_BOX(config_container), row_dim);
    gtk_box_append(GTK_BOX(config_container), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *row_pad = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(row_pad), gtk_label_new("Inner Padding"));
    GtkAdjustment *adj_pad = gtk_adjustment_new((double)b->padding, 0.0, 100.0, 1.0, 5.0, 0.0);
    GtkWidget *spin_pad = gtk_spin_button_new(adj_pad, 1.0, 0);
    gtk_widget_set_hexpand(spin_pad, TRUE);
    gtk_widget_set_halign(spin_pad, GTK_ALIGN_END);
    g_signal_connect(spin_pad, "value-changed", G_CALLBACK(on_padding_changed), b);
    gtk_box_append(GTK_BOX(row_pad), spin_pad);
    gtk_box_append(GTK_BOX(config_container), row_pad);

    GtkWidget *row_trans = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(row_trans), gtk_label_new("Transparent"));
    GtkWidget *sw = gtk_switch_new();
    gtk_widget_set_halign(sw, GTK_ALIGN_END);
    gtk_widget_set_hexpand(sw, TRUE);
    gtk_switch_set_active(GTK_SWITCH(sw), b->is_transparent);
    g_signal_connect(sw, "notify::active", G_CALLBACK(on_transparent_toggled), b);
    gtk_box_append(GTK_BOX(row_trans), sw);
    gtk_box_append(GTK_BOX(config_container), row_trans);

    GtkWidget *row_col = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(row_col), gtk_label_new("Wallpaper Color"));
    GtkWidget *sw_col = gtk_switch_new();
    gtk_widget_set_halign(sw_col, GTK_ALIGN_END);
    gtk_widget_set_hexpand(sw_col, TRUE);
    gtk_switch_set_active(GTK_SWITCH(sw_col), b->use_dominant_color);
    g_signal_connect(sw_col, "notify::active", G_CALLBACK(on_dominant_toggled), b);
    gtk_box_append(GTK_BOX(row_col), sw_col);
    gtk_box_append(GTK_BOX(config_container), row_col);

    if (g_strcmp0(b->type, "Label") == 0) {
        GtkWidget *row_txt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_box_append(GTK_BOX(row_txt), gtk_label_new("Label Text"));
        GtkWidget *entry = gtk_entry_new();
        if (b->custom_text) gtk_editable_set_text(GTK_EDITABLE(entry), b->custom_text);
        else gtk_editable_set_text(GTK_EDITABLE(entry), "Edit Me");
        g_signal_connect(entry, "changed", G_CALLBACK(on_text_changed), b);
        gtk_box_append(GTK_BOX(row_txt), entry);
        gtk_box_append(GTK_BOX(config_container), row_txt);
    }

    if (g_strcmp0(b->type, "Clock") == 0) {
        GtkWidget *row_24 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_append(GTK_BOX(row_24), gtk_label_new("24h Format"));
        GtkWidget *sw24 = gtk_switch_new();
        gtk_widget_set_halign(sw24, GTK_ALIGN_END);
        gtk_widget_set_hexpand(sw24, TRUE);
        gtk_switch_set_active(GTK_SWITCH(sw24), b->is_24h);
        g_signal_connect(sw24, "notify::active", G_CALLBACK(on_24h_toggled), b);
        gtk_box_append(GTK_BOX(row_24), sw24);
        gtk_box_append(GTK_BOX(config_container), row_24);

        GtkWidget *row_time = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_append(GTK_BOX(row_time), gtk_label_new("Time Size"));
        GtkAdjustment *adj_t = gtk_adjustment_new((b->font_size_time > 0 ? b->font_size_time : 95.0), 10.0, 300.0, 5.0, 10.0, 0.0);
        GtkWidget *spin_t = gtk_spin_button_new(adj_t, 1.0, 0);
        gtk_widget_set_halign(spin_t, GTK_ALIGN_END);
        gtk_widget_set_hexpand(spin_t, TRUE);
        g_signal_connect(spin_t, "value-changed", G_CALLBACK(on_time_size_changed), b);
        gtk_box_append(GTK_BOX(row_time), spin_t);
        gtk_box_append(GTK_BOX(config_container), row_time);

        GtkWidget *row_date = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_append(GTK_BOX(row_date), gtk_label_new("Date Size"));
        GtkAdjustment *adj_d = gtk_adjustment_new((b->font_size_date > 0 ? b->font_size_date : 24.0), 8.0, 100.0, 1.0, 5.0, 0.0);
        GtkWidget *spin_d = gtk_spin_button_new(adj_d, 1.0, 0);
        gtk_widget_set_halign(spin_d, GTK_ALIGN_END);
        gtk_widget_set_hexpand(spin_d, TRUE);
        g_signal_connect(spin_d, "value-changed", G_CALLBACK(on_date_size_changed), b);
        gtk_box_append(GTK_BOX(row_date), spin_d);
        gtk_box_append(GTK_BOX(config_container), row_date);
    } 
    else {
        GtkWidget *row_font = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_append(GTK_BOX(row_font), gtk_label_new("Font Size"));
        GtkAdjustment *adj = gtk_adjustment_new((b->font_size_time > 0 ? b->font_size_time : 16.0), 8.0, 200.0, 1.0, 10.0, 0.0);
        GtkWidget *spin = gtk_spin_button_new(adj, 1.0, 0);
        gtk_widget_set_hexpand(spin, TRUE);
        g_signal_connect(spin, "value-changed", G_CALLBACK(on_generic_size_changed), b);
        gtk_box_append(GTK_BOX(row_font), spin);
        gtk_box_append(GTK_BOX(config_container), row_font);
    }

    GtkWidget *lbl_nudge = gtk_label_new("Fine Tune Position");
    gtk_widget_set_halign(lbl_nudge, GTK_ALIGN_START);
    gtk_widget_set_margin_top(lbl_nudge, 15);
    gtk_box_append(GTK_BOX(config_container), lbl_nudge);

    GtkWidget *row_nudge = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_halign(row_nudge, GTK_ALIGN_CENTER);
    add_nudge_btn(row_nudge, b, "go-up-symbolic", 0, -1);
    add_nudge_btn(row_nudge, b, "go-down-symbolic", 0, 1);
    add_nudge_btn(row_nudge, b, "go-previous-symbolic", -1, 0);
    add_nudge_btn(row_nudge, b, "go-next-symbolic", 1, 0);
    gtk_box_append(GTK_BOX(config_container), row_nudge);

    gtk_stack_set_visible_child_name(GTK_STACK(app_state.drawer_stack), PAGE_CONFIG);
}

static GdkContentProvider* on_drag_prepare(GtkDragSource *source, double x, double y, gpointer user_data) {
    int variant_idx = GPOINTER_TO_INT(user_data);
    if (!selected_family) return NULL;
    char *payload = g_strdup_printf("%s:%d", selected_family->id, variant_idx);
    GValue value = G_VALUE_INIT;
    g_value_init(&value, G_TYPE_STRING);
    g_value_set_string(&value, payload);
    g_free(payload);
    return gdk_content_provider_new_for_value(&value);
}

static void on_family_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    const char *fam_id = g_object_get_data(G_OBJECT(row), "family-id");
    selected_family = scene_registry_lookup_family(fam_id);
    if (!selected_family) return;

    gtk_flow_box_remove_all(GTK_FLOW_BOX(variants_flowbox));
    gtk_label_set_text(GTK_LABEL(lbl_family_title), selected_family->name);

    for (int i = 0; i < selected_family->variant_count; i++) {
        const SceneVariant *var = &selected_family->variants[i];
        
        GtkWidget *wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_add_css_class(wrapper, "drawer-preview");
        gtk_widget_set_size_request(wrapper, 140, 100);

        GtkWidget *preview = var->factory(); 
        gtk_widget_set_can_target(preview, FALSE);
        gtk_widget_set_vexpand(preview, TRUE);
        gtk_box_append(GTK_BOX(wrapper), preview);

        GtkWidget *lbl = gtk_label_new(var->name);
        gtk_widget_add_css_class(lbl, "drawer-label");
        gtk_box_append(GTK_BOX(wrapper), lbl);

        GtkDragSource *ds = gtk_drag_source_new();
        g_signal_connect(ds, "prepare", G_CALLBACK(on_drag_prepare), GINT_TO_POINTER(i));
        gtk_drag_source_set_actions(ds, GDK_ACTION_COPY);
        gtk_widget_add_controller(wrapper, GTK_EVENT_CONTROLLER(ds));

        gtk_flow_box_append(GTK_FLOW_BOX(variants_flowbox), wrapper);
    }
    gtk_stack_set_visible_child_name(GTK_STACK(app_state.drawer_stack), PAGE_VARIANTS);
}

static void on_back_to_families(GtkButton *btn, gpointer user_data) {
    gtk_stack_set_visible_child_name(GTK_STACK(app_state.drawer_stack), PAGE_FAMILIES);
    selected_family = NULL;
}

GtkWidget *create_drawer(void) {
    GtkWidget *revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), 300);

    GtkWidget *bg = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(bg, "drawer-bg");
    gtk_widget_set_size_request(bg, 360, 500); 
    gtk_widget_set_valign(bg, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(bg, 20);
    gtk_widget_set_margin_bottom(bg, 20);
    gtk_widget_set_margin_end(bg, 20);

    app_state.drawer_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(app_state.drawer_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    
    GtkWidget *p_families = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *header_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top(header_row, 20);
    gtk_widget_set_margin_bottom(header_row, 10);
    gtk_widget_set_margin_start(header_row, 20);
    gtk_widget_set_margin_end(header_row, 15);

    GtkWidget *header = gtk_label_new("Widgets");
    gtk_widget_add_css_class(header, "drawer-header");
    gtk_widget_set_hexpand(header, TRUE);
    gtk_widget_set_halign(header, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(header_row), header);

    GtkWidget *close_btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(close_btn, "flat");
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_drawer_close_clicked), NULL);
    gtk_box_append(GTK_BOX(header_row), close_btn);
    gtk_box_append(GTK_BOX(p_families), header_row);

    GtkWidget *scale_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(scale_box, 20);
    gtk_widget_set_margin_end(scale_box, 20);
    gtk_widget_set_margin_bottom(scale_box, 15);
    GtkWidget *scale_lbl = gtk_label_new("Grid Size");
    gtk_widget_add_css_class(scale_lbl, "heading");
    gtk_box_append(GTK_BOX(scale_box), scale_lbl);
    
    GtkAdjustment *adj = gtk_adjustment_new(app_state.physics.cell_size, 40, 150, 5, 10, 0);
    GtkWidget *spin = gtk_spin_button_new(adj, 5, 0);
    gtk_widget_set_hexpand(spin, TRUE);
    gtk_widget_set_halign(spin, GTK_ALIGN_END);
    g_signal_connect(spin, "value-changed", G_CALLBACK(on_grid_scale_changed), NULL);
    gtk_box_append(GTK_BOX(scale_box), spin);
    gtk_box_append(GTK_BOX(p_families), scale_box);

    GtkWidget *fam_scroll = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(fam_scroll, TRUE);
    gtk_widget_set_vexpand(fam_scroll, TRUE);
    GtkWidget *fam_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(fam_list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(fam_list, "content");
    g_signal_connect(fam_list, "row-activated", G_CALLBACK(on_family_row_activated), NULL);

    int count = 0;
    const SceneFamily *all_fams = scene_registry_get_all(&count);
    for (int i=0; i<count; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 15);
        gtk_widget_set_margin_top(row, 12);
        gtk_widget_set_margin_bottom(row, 12);
        gtk_widget_set_margin_start(row, 15);
        gtk_widget_set_margin_end(row, 15);

        GtkWidget *icon = gtk_image_new_from_icon_name(all_fams[i].icon);
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 24);
        gtk_box_append(GTK_BOX(row), icon);

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
        GtkWidget *lbl = gtk_label_new(all_fams[i].name);
        gtk_widget_add_css_class(lbl, "title-3");
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(vbox), lbl);
        char *sub = g_strdup_printf("%d Layouts", all_fams[i].variant_count);
        GtkWidget *sublbl = gtk_label_new(sub);
        gtk_widget_add_css_class(sublbl, "caption");
        gtk_widget_set_halign(sublbl, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(vbox), sublbl);
        g_free(sub);
        gtk_box_append(GTK_BOX(row), vbox);
        
        GtkWidget *arrow = gtk_image_new_from_icon_name("go-next-symbolic");
        gtk_widget_set_hexpand(arrow, TRUE);
        gtk_widget_set_halign(arrow, GTK_ALIGN_END);
        gtk_box_append(GTK_BOX(row), arrow);

        GtkWidget *list_row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(list_row), row);
        g_object_set_data(G_OBJECT(list_row), "family-id", (gpointer)all_fams[i].id);
        gtk_list_box_append(GTK_LIST_BOX(fam_list), list_row);
    }
    
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(fam_scroll), fam_list);
    gtk_box_append(GTK_BOX(p_families), fam_scroll);

    GtkWidget *p_variants = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *v_header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top(v_header_box, 20);
    gtk_widget_set_margin_bottom(v_header_box, 10);
    gtk_widget_set_margin_start(v_header_box, 10);

    GtkWidget *back_btn = gtk_button_new_from_icon_name("go-previous-symbolic");
    gtk_widget_add_css_class(back_btn, "flat");
    g_signal_connect(back_btn, "clicked", G_CALLBACK(on_back_to_families), NULL);
    gtk_box_append(GTK_BOX(v_header_box), back_btn);

    lbl_family_title = gtk_label_new("Variants");
    gtk_widget_add_css_class(lbl_family_title, "title-2");
    gtk_box_append(GTK_BOX(v_header_box), lbl_family_title);
    gtk_box_append(GTK_BOX(p_variants), v_header_box);

    GtkWidget *var_scroll = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(var_scroll, TRUE);
    gtk_widget_set_vexpand(var_scroll, TRUE);
    variants_flowbox = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(variants_flowbox), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(variants_flowbox), 2);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(variants_flowbox), 10);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(variants_flowbox), 10);
    gtk_widget_set_margin_start(variants_flowbox, 15);
    gtk_widget_set_margin_end(variants_flowbox, 15);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(var_scroll), variants_flowbox);
    gtk_box_append(GTK_BOX(p_variants), var_scroll);

    GtkWidget *p_config = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *config_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(config_scroll, TRUE);
    gtk_widget_set_hexpand(config_scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(config_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    config_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_widget_set_margin_top(config_container, 20);
    gtk_widget_set_margin_bottom(config_container, 20);
    gtk_widget_set_margin_start(config_container, 20);
    gtk_widget_set_margin_end(config_container, 20);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(config_scroll), config_container);
    gtk_box_append(GTK_BOX(p_config), config_scroll);

    gtk_stack_add_named(GTK_STACK(app_state.drawer_stack), p_families, PAGE_FAMILIES);
    gtk_stack_add_named(GTK_STACK(app_state.drawer_stack), p_variants, PAGE_VARIANTS);
    gtk_stack_add_named(GTK_STACK(app_state.drawer_stack), p_config, PAGE_CONFIG);

    gtk_box_append(GTK_BOX(bg), app_state.drawer_stack);
    gtk_revealer_set_child(GTK_REVEALER(revealer), bg);
    return revealer;
}