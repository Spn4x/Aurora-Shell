// FILE: ./scenes/clock/main.c
#include <gtk/gtk.h>
#include <adwaita.h>
#include <string.h>

// Standard update loop
static gboolean update_clock_lbl(gpointer label) {
    if (!GTK_IS_WIDGET(label)) return G_SOURCE_REMOVE;
    
    // 1. Get the base format
    const char *base_fmt = g_object_get_data(G_OBJECT(label), "time-fmt");
    if (!base_fmt) base_fmt = "%H:%M";

    // 2. Check settings on parent (set by drawer.c)
    gboolean is_24h = TRUE; 
    GtkWidget *parent = GTK_WIDGET(label);
    while (parent) {
        gpointer data = g_object_get_data(G_OBJECT(parent), "clock-is-24h");
        if (data) {
            is_24h = (GPOINTER_TO_INT(data) == 1);
            break;
        }
        parent = gtk_widget_get_parent(parent);
    }

    // 3. Adjust format
    char final_fmt[64];
    strncpy(final_fmt, base_fmt, 63);
    final_fmt[63] = '\0';

    if (!is_24h) {
        char *h = strchr(final_fmt, 'H');
        if (h) *h = 'I';
    } else {
        char *i = strchr(final_fmt, 'I');
        if (i) *i = 'H';
    }

    // 4. Update text
    GDateTime *now = g_date_time_new_now_local();
    char *txt = g_date_time_format(now, final_fmt);
    gtk_label_set_text(GTK_LABEL(label), txt);
    g_free(txt);
    g_date_time_unref(now);
    
    return G_SOURCE_CONTINUE;
}

// FIX: Helper to run the first update *after* settings are applied
static gboolean on_initial_tick(gpointer label) {
    if (GTK_IS_WIDGET(label)) {
        update_clock_lbl(label);
    }
    return G_SOURCE_REMOVE; // Run once
}

static void start_clock(GtkWidget *lbl, const char *fmt) {
    g_object_set_data_full(G_OBJECT(lbl), "time-fmt", g_strdup(fmt), g_free);
    
    // FIX: Use idle_add instead of direct call.
    // This allows 'apply_box_settings' to run and set the 12/24h flag
    // BEFORE we generate the first text string. No more flicker.
    g_idle_add(on_initial_tick, lbl);
    
    guint id = g_timeout_add_seconds(1, update_clock_lbl, lbl);
    g_signal_connect_swapped(lbl, "destroy", G_CALLBACK(g_source_remove), GUINT_TO_POINTER(id));
}

// --- VARIANT 1: Simple ---
GtkWidget* scene_clock_simple(void) {
    GtkWidget *lbl = gtk_label_new("00:00");
    gtk_widget_add_css_class(lbl, "title-1");
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_name(lbl, "clock-time");
    start_clock(lbl, "%H:%M");
    
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "card");
    gtk_box_append(GTK_BOX(box), lbl);
    return box;
}

// --- VARIANT 2: Date & Time ---
GtkWidget* scene_clock_date(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "card");
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER); 
    gtk_widget_set_halign(box, GTK_ALIGN_FILL); 

    GtkWidget *time = gtk_label_new("00:00");
    gtk_widget_add_css_class(time, "display-large");
    gtk_widget_set_halign(time, GTK_ALIGN_CENTER);
    gtk_widget_set_name(time, "clock-time");
    start_clock(time, "%H:%M");

    GtkWidget *date = gtk_label_new("Date");
    gtk_widget_add_css_class(date, "title-3");
    gtk_widget_set_opacity(date, 0.7);
    gtk_widget_set_halign(date, GTK_ALIGN_CENTER);
    gtk_widget_set_name(date, "clock-date");
    start_clock(date, "%A, %B %d");

    gtk_box_append(GTK_BOX(box), time);
    gtk_box_append(GTK_BOX(box), date);
    return box;
}

// --- VARIANT 3: Big ---
GtkWidget* scene_clock_big(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "card");
    
    GtkWidget *hh = gtk_label_new("00");
    gtk_widget_add_css_class(hh, "display-large");
    gtk_widget_set_name(hh, "clock-time");
    start_clock(hh, "%H");
    
    GtkWidget *mm = gtk_label_new("00");
    gtk_widget_add_css_class(mm, "display-large");
    gtk_widget_set_margin_top(mm, -15);
    gtk_widget_set_name(mm, "clock-time");
    start_clock(mm, "%M");
    
    gtk_box_append(GTK_BOX(box), hh);
    gtk_box_append(GTK_BOX(box), mm);
    return box;
}

// --- VARIANT 4: Wide Side (Time Left, Date Right) ---
GtkWidget* scene_clock_wide(void) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20); 
    gtk_widget_add_css_class(card, "card");
    gtk_widget_set_halign(card, GTK_ALIGN_FILL);
    gtk_widget_set_valign(card, GTK_ALIGN_FILL);
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(card, GTK_ALIGN_CENTER);

    GtkWidget *time = gtk_label_new("00:00");
    gtk_widget_add_css_class(time, "display-large");
    gtk_widget_set_name(time, "clock-time");
    gtk_widget_set_valign(time, GTK_ALIGN_CENTER);
    start_clock(time, "%H:%M");
    
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
    
    GtkWidget *day = gtk_label_new("Day");
    gtk_widget_add_css_class(day, "title-2");
    gtk_widget_set_halign(day, GTK_ALIGN_START);
    gtk_widget_set_name(day, "clock-date");
    start_clock(day, "%A"); 

    GtkWidget *date = gtk_label_new("Date");
    gtk_widget_add_css_class(date, "title-3");
    gtk_widget_set_opacity(date, 0.7);
    gtk_widget_set_halign(date, GTK_ALIGN_START);
    gtk_widget_set_name(date, "clock-date");
    start_clock(date, "%B %d");

    gtk_box_append(GTK_BOX(vbox), day);
    gtk_box_append(GTK_BOX(vbox), date);

    gtk_box_append(GTK_BOX(card), time);
    gtk_box_append(GTK_BOX(card), gtk_separator_new(GTK_ORIENTATION_VERTICAL)); 
    gtk_box_append(GTK_BOX(card), vbox);

    return card;
}

// --- VARIANT 5: Left Modern (Vertical Stack, Left Aligned) ---
GtkWidget* scene_clock_left(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(box, "card");
    gtk_widget_set_halign(box, GTK_ALIGN_FILL); 
    gtk_widget_set_valign(box, GTK_ALIGN_FILL);
    
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(inner, GTK_ALIGN_CENTER); 
    gtk_widget_set_halign(inner, GTK_ALIGN_START);  
    gtk_widget_set_margin_start(inner, 24);         
    
    GtkWidget *time = gtk_label_new("00:00");
    gtk_widget_add_css_class(time, "display-large");
    gtk_widget_set_halign(time, GTK_ALIGN_START);
    gtk_widget_set_name(time, "clock-time");
    start_clock(time, "%H:%M");

    GtkWidget *day = gtk_label_new("Monday");
    gtk_widget_add_css_class(day, "title-2");
    gtk_widget_set_halign(day, GTK_ALIGN_START);
    gtk_widget_set_name(day, "clock-date");
    start_clock(day, "%A");

    GtkWidget *date = gtk_label_new("Dec 15");
    gtk_widget_add_css_class(date, "title-3");
    gtk_widget_set_opacity(date, 0.6);
    gtk_widget_set_halign(date, GTK_ALIGN_START);
    gtk_widget_set_name(date, "clock-date");
    start_clock(date, "%B %d, %Y");

    gtk_box_append(GTK_BOX(inner), time);
    gtk_box_append(GTK_BOX(inner), day);
    gtk_box_append(GTK_BOX(inner), date);
    
    gtk_box_append(GTK_BOX(box), inner);
    return box;
}

// --- VARIANT 6: Pill Badge (Time with date inside a pill) ---
GtkWidget* scene_clock_pill(void) {
    GtkWidget *wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(wrapper, "card");
    gtk_widget_set_valign(wrapper, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(wrapper, GTK_ALIGN_CENTER);

    GtkWidget *time = gtk_label_new("00:00");
    gtk_widget_add_css_class(time, "display-large");
    gtk_widget_set_halign(time, GTK_ALIGN_CENTER);
    gtk_widget_set_name(time, "clock-time");
    start_clock(time, "%H:%M");

    GtkWidget *pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(pill, GTK_ALIGN_CENTER);
    
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, 
        ".date-pill { background-color: rgba(255,255,255,0.15); border-radius: 99px; padding: 4px 12px; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_widget_add_css_class(pill, "date-pill");

    GtkWidget *date = gtk_label_new("Date");
    gtk_widget_add_css_class(date, "title-3");
    gtk_widget_set_name(date, "clock-date");
    start_clock(date, "%a, %b %d");

    gtk_box_append(GTK_BOX(pill), date);
    gtk_box_append(GTK_BOX(wrapper), time);
    gtk_box_append(GTK_BOX(wrapper), pill);
    
    return wrapper;
}