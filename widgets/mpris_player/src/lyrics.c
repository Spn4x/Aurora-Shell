#include "lyrics.h"
#include <glib/gregex.h>
#include <graphene.h>

#define CLAMP(x, low, high)  (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))

static void free_lyric_line(gpointer data) {
    LyricLine *line = data;
    if (!line) return;
    g_free(line->line_text);
    g_free(line);
}

void lyrics_view_set_active_index(LyricsView *view, int new_line_index) {
    if (!view || !view->lyric_lines || g_list_length(view->lyric_lines) == 0) return;

    if (new_line_index != view->current_line_index) {
        if (view->current_line_index >= 0) {
            GList *old_link = g_list_nth(view->lyric_lines, view->current_line_index);
            if (old_link && old_link->data) {
                GtkWidget *label = ((LyricLine*)old_link->data)->label;
                if (GTK_IS_WIDGET(label)) gtk_widget_remove_css_class(label, "active-lyric");
            }
        }

        if (new_line_index >= 0) {
             GList *new_link = g_list_nth(view->lyric_lines, new_line_index);
             if (new_link && new_link->data) {
                GtkWidget *active_label = ((LyricLine*)new_link->data)->label;
                gtk_widget_add_css_class(active_label, "active-lyric");

                GtkAdjustment *v_adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(view->scrolled_window));
                graphene_rect_t bounds;
                (void)gtk_widget_compute_bounds(active_label, gtk_widget_get_parent(active_label), &bounds);
                double target_value = bounds.origin.y - (gtk_adjustment_get_page_size(v_adj) / 2.0) + (bounds.size.height / 2.0);
                target_value = CLAMP(target_value, gtk_adjustment_get_lower(v_adj), gtk_adjustment_get_upper(v_adj) - gtk_adjustment_get_page_size(v_adj));
                gtk_adjustment_set_value(v_adj, target_value);
             }
        }
        view->current_line_index = new_line_index;
    }
}

GtkWidget* create_lyrics_view(LyricsView **view_out) {
    LyricsView *view = g_new0(LyricsView, 1);
    view->current_line_index = -1;
    view->scrolled_window = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(view->scrolled_window), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(view->scrolled_window, TRUE);
    view->lyrics_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_add_css_class(view->lyrics_box, "lyrics-box");
    gtk_widget_set_valign(view->lyrics_box, GTK_ALIGN_CENTER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(view->scrolled_window), view->lyrics_box);
    *view_out = view;
    return view->scrolled_window;
}

void destroy_lyrics_view(LyricsView *view) {
    if (!view) return;
    g_list_free_full(view->lyric_lines, free_lyric_line);
    g_free(view);
}

void clear_lyrics_display(LyricsView *view) {
    if (!view) return;
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(view->lyrics_box))) {
        gtk_box_remove(GTK_BOX(view->lyrics_box), child);
    }
}

void parse_and_populate_lyrics(LyricsView *view, const gchar *lrc_data) {
    if (!view) return;
    clear_lyrics_display(view);
    if (view->lyric_lines) {
        g_list_free_full(view->lyric_lines, free_lyric_line);
        view->lyric_lines = NULL;
    }
    view->current_line_index = -1;

    if (!lrc_data || strlen(lrc_data) == 0) {
        GtkWidget *label = gtk_label_new("Searching for lyrics...");
        gtk_widget_add_css_class(label, "lyrics-label");
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_widget_set_vexpand(label, TRUE);
        gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(view->lyrics_box), label);
        return;
    }

    g_autofree gchar **lines = g_strsplit(lrc_data, "\n", -1);
    g_autoptr(GRegex) regex = g_regex_new("\\[([0-9]{2}):([0-9]{2})[.:]([0-9]{2,3})\\](.*)", 0, 0, NULL);
    for (int i = 0; lines && lines[i] != NULL; i++) {
        g_autoptr(GMatchInfo) match_info = NULL;
        if (g_regex_match(regex, lines[i], 0, &match_info)) {
            g_autofree gchar *text = g_match_info_fetch(match_info, 4);
            if (text) {
                g_autofree gchar *trimmed_text = g_strstrip(g_strdup(text));
                if (strlen(trimmed_text) == 0) continue;
                LyricLine *line = g_new0(LyricLine, 1);
                line->line_text = g_strdup(trimmed_text);
                line->label = gtk_label_new(line->line_text);
                gtk_widget_add_css_class(line->label, "lyrics-label");
                gtk_label_set_wrap(GTK_LABEL(line->label), TRUE);
                gtk_label_set_wrap_mode(GTK_LABEL(line->label), PANGO_WRAP_WORD_CHAR);
                gtk_label_set_xalign(GTK_LABEL(line->label), 0.0);
                gtk_label_set_justify(GTK_LABEL(line->label), GTK_JUSTIFY_LEFT);
                gtk_box_append(GTK_BOX(view->lyrics_box), line->label);
                view->lyric_lines = g_list_append(view->lyric_lines, line);
            }
        }
    }

    if (g_list_length(view->lyric_lines) == 0) {
        GtkWidget *label = gtk_label_new("No synchronized lyrics found for this track.");
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_LEFT);
        gtk_widget_set_vexpand(label, TRUE);
        gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(view->lyrics_box), label);
    }
}