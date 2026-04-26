#ifndef LYRICS_H
#define LYRICS_H

#include <gtk/gtk.h>

typedef struct {
    gchar *line_text;
    GtkWidget *label;
} LyricLine;

typedef struct {
    GtkWidget *scrolled_window;
    GtkWidget *lyrics_box;
    GList *lyric_lines;
    gint current_line_index;
} LyricsView;

GtkWidget* create_lyrics_view(LyricsView **view_out);
void destroy_lyrics_view(LyricsView *view);
void clear_lyrics_display(LyricsView *view);
void parse_and_populate_lyrics(LyricsView *view, const gchar *lrc_data);

// Changed from manual timestamp parsing to a simple active index setter
void lyrics_view_set_active_index(LyricsView *view, int index);

#endif // LYRICS_H