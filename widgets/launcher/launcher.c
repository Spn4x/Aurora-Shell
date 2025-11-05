// ===================================================================
//  Aurora Launcher Widget (Corrected)
// ===================================================================
// A searchable, keyboard-driven application launcher and command runner,
// integrated as a dynamic plugin for the Aurora Shell.

#include <gtk/gtk.h>
#include <string.h>
#include "launcher.h"
#include "modules/apps.h"
#include "modules/calculator.h"
#include "modules/commands.h"

// ===================================================================
//  GObject Definition: AuroraResultObject
//  (The data model for a single search result)
// ===================================================================

#define AURORA_RESULT_OBJECT_TYPE (aurora_result_object_get_type())
struct _AuroraResultObject {
    GObject parent_instance;
    AuroraResultType type;
    gchar *name;
    gchar *description;
    gchar *icon_name;
    gpointer data;
    GDestroyNotify data_free_func;
};
G_DEFINE_TYPE(AuroraResultObject, aurora_result_object, G_TYPE_OBJECT)

static void aurora_result_object_finalize(GObject *object) {
    AuroraResultObject *self = AURORA_RESULT_OBJECT(object);
    g_free(self->name);
    g_free(self->description);
    g_free(self->icon_name);
    if (self->data_free_func && self->data) {
        self->data_free_func(self->data);
    }
    G_OBJECT_CLASS(aurora_result_object_parent_class)->finalize(object);
}

static void aurora_result_object_init(AuroraResultObject *self) {
    (void)self;
}

static void aurora_result_object_class_init(AuroraResultObjectClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = aurora_result_object_finalize;
}

AuroraResultObject* aurora_result_object_new(AuroraResultType type, const gchar *name, const gchar *description, const gchar *icon_name, gpointer data, GDestroyNotify data_free_func) {
    AuroraResultObject *self = g_object_new(AURORA_RESULT_OBJECT_TYPE, NULL);
    self->type = type;
    self->name = g_strdup(name);
    self->description = g_strdup(description);
    self->icon_name = g_strdup(icon_name);
    self->data = data;
    self->data_free_func = data_free_func;
    return self;
}

// ===================================================================
//  Launcher State & Cleanup
// ===================================================================

typedef struct {
    GtkWidget *main_box;
    GtkWidget *entry;
    GtkWidget *listbox;
    GtkWidget *results_revealer;
    GListStore *results_store;
} LauncherState;

static void free_launcher_state(gpointer data) {
    LauncherState *state = (LauncherState *)data;
    g_object_unref(state->results_store);
    g_free(state);
}

// ===================================================================
//  Core Logic & Data Handling
// ===================================================================

static void update_search_results(LauncherState *state, const gchar *search_text) {
    g_list_store_remove_all(state->results_store);

    GList *command_results = get_command_results(search_text);
    GList *app_results = get_app_results(search_text);
    GList *calc_results = get_calculator_results(search_text);
    
    GList *all_results = g_list_concat(command_results, g_list_concat(calc_results, app_results));

    for (GList *l = all_results; l != NULL; l = l->next) {
        g_list_store_append(state->results_store, l->data);
        g_object_unref(l->data);
    }
    g_list_free(all_results);

    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(state->results_store));
    gtk_revealer_set_reveal_child(GTK_REVEALER(state->results_revealer), n_items > 0);
    if (n_items > 0) {
        GtkListBoxRow *first_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(state->listbox), 0);
        gtk_list_box_select_row(GTK_LIST_BOX(state->listbox), first_row);
    }
}

// ===================================================================
//  Signal Handlers / Callbacks
// ===================================================================

static void on_widget_mapped(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    LauncherState *state = (LauncherState *)user_data;
    gtk_editable_set_text(GTK_EDITABLE(state->entry), "");
    gtk_widget_grab_focus(state->entry);
}

static void on_entry_changed(GtkEditable *entry, gpointer user_data) {
    LauncherState *state = (LauncherState *)user_data;
    const gchar *search_text = gtk_editable_get_text(entry);
    update_search_results(state, search_text);
}

static void on_result_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)user_data;
    guint index = gtk_list_box_row_get_index(row);
    // <<< THE TYPO WAS HERE >>>
    LauncherState *state = (LauncherState*)g_object_get_data(G_OBJECT(gtk_widget_get_ancestor(GTK_WIDGET(box), GTK_TYPE_BOX)), "launcher-state");
    AuroraResultObject *result = g_list_model_get_item(G_LIST_MODEL(state->results_store), index);
    if (!result) return;

    switch (result->type) {
        case AURORA_RESULT_APP: {
            GAppInfo *app_info = G_APP_INFO(result->data);
            g_autoptr(GError) error = NULL;
            if (!g_app_info_launch(app_info, NULL, NULL, &error)) {
                g_warning("Failed to launch application: %s", error->message);
            }
            break;
        }
        case AURORA_RESULT_CALCULATOR: {
            gchar *calc_result = (gchar*)result->data;
            GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(box));
            gdk_clipboard_set_text(clipboard, calc_result);
            break;
        }
        case AURORA_RESULT_COMMAND: {
            gchar *command = (gchar*)result->data;
            g_spawn_async(NULL, (gchar*[]){"foot", "-e", command, NULL}, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
            break;
        }
        default:
            break;
    }

    g_object_unref(result);
    
    GtkWidget *toplevel = gtk_widget_get_ancestor(GTK_WIDGET(row), GTK_TYPE_WINDOW);
    if (toplevel) {
        gtk_widget_set_visible(toplevel, FALSE);
    }
}

static void on_entry_activated(GtkEntry *entry, gpointer user_data) {
    (void)entry;
    LauncherState *state = (LauncherState *)user_data;
    GtkListBoxRow *selected_row = gtk_list_box_get_selected_row(GTK_LIST_BOX(state->listbox));
    if (selected_row) {
        gtk_widget_activate(GTK_WIDGET(selected_row));
    }
}

static gboolean on_key_pressed_nav(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    (void)controller; (void)keycode; (void)state;
    LauncherState *launcher_state = (LauncherState *)user_data;
    GtkListBox *listbox = GTK_LIST_BOX(launcher_state->listbox);

    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down) {
        guint n_items = g_list_model_get_n_items(G_LIST_MODEL(launcher_state->results_store));
        if (n_items == 0) {
            return GDK_EVENT_PROPAGATE;
        }

        GtkListBoxRow *selected_row = gtk_list_box_get_selected_row(listbox);
        gint current_index = selected_row ? gtk_list_box_row_get_index(selected_row) : -1;
        gint new_index;

        if (keyval == GDK_KEY_Down) {
            new_index = (current_index + 1) % n_items;
        } else { // GDK_KEY_Up
            new_index = (current_index - 1 + n_items) % n_items;
        }

        GtkListBoxRow *row_to_select = gtk_list_box_get_row_at_index(listbox, new_index);
        gtk_list_box_select_row(listbox, row_to_select);

        // --- FIX IS HERE ---
        // This tells the parent GtkScrolledWindow to scroll and make this widget visible.
        if (row_to_select) {
            gtk_widget_grab_focus(GTK_WIDGET(row_to_select));
        }
        // --- END FIX ---

        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

// ===================================================================
//  UI Construction
// ===================================================================

static GtkWidget* create_result_row_ui(AuroraResultObject *result) {
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(main_box, 10);
    gtk_widget_set_margin_end(main_box, 10);
    gtk_widget_set_margin_top(main_box, 5);
    gtk_widget_set_margin_bottom(main_box, 5);

    GtkWidget *icon = NULL;
    if (result->icon_name && g_path_is_absolute(result->icon_name)) {
        icon = gtk_image_new_from_file(result->icon_name);
    } else {
        icon = gtk_image_new_from_icon_name(result->icon_name);
    }
    
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
    gtk_box_append(GTK_BOX(main_box), icon);

    GtkWidget *name_label = gtk_label_new(result->name);
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
    gtk_widget_add_css_class(name_label, "result-name");
    gtk_box_append(GTK_BOX(main_box), name_label);
    
    return main_box;
}

static GtkWidget* bind_model_create_widget_func(gpointer item, gpointer user_data) {
    (void)user_data;
    GtkListBoxRow *row = GTK_LIST_BOX_ROW(gtk_list_box_row_new());
    GtkWidget *content = create_result_row_ui(AURORA_RESULT_OBJECT(item));
    gtk_list_box_row_set_child(row, content);
    return GTK_WIDGET(row);
}

// ===================================================================
//  Plugin Entry Point
// ===================================================================

GtkWidget* create_widget(const char *config_string) {
    (void)config_string;

    LauncherState *state = g_new0(LauncherState, 1);
    state->results_store = g_list_store_new(AURORA_RESULT_OBJECT_TYPE);
    
    state->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(state->main_box, "launcher-box");
    g_object_set_data_full(G_OBJECT(state->main_box), "launcher-state", state, free_launcher_state);

    g_signal_connect(state->main_box, "map", G_CALLBACK(on_widget_mapped), state);

    state->entry = gtk_entry_new();
    gtk_widget_add_css_class(state->entry, "launcher-entry");
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->entry), "Search Apps, Calculate, or > Run Command");
    
    state->listbox = gtk_list_box_new();
    gtk_widget_add_css_class(state->listbox, "results-listbox");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(state->listbox), GTK_SELECTION_SINGLE);
    gtk_list_box_bind_model(GTK_LIST_BOX(state->listbox), G_LIST_MODEL(state->results_store), bind_model_create_widget_func, NULL, NULL);

    GtkWidget *scrolled_win = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scrolled_win, "results-scroller");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_win), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_win), state->listbox);

    state->results_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(state->results_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(state->results_revealer), 200);
    gtk_revealer_set_child(GTK_REVEALER(state->results_revealer), scrolled_win);

    gtk_box_append(GTK_BOX(state->main_box), state->entry);
    gtk_box_append(GTK_BOX(state->main_box), state->results_revealer);

    g_signal_connect(state->entry, "changed", G_CALLBACK(on_entry_changed), state);
    g_signal_connect(state->entry, "activate", G_CALLBACK(on_entry_activated), state);
    g_signal_connect(state->listbox, "row-activated", G_CALLBACK(on_result_activated), state);

    GtkEventController *nav_controller = gtk_event_controller_key_new();
    g_signal_connect(nav_controller, "key-pressed", G_CALLBACK(on_key_pressed_nav), state);
    gtk_widget_add_controller(state->main_box, nav_controller);

    gtk_widget_set_focusable(state->entry, TRUE);

    return state->main_box;
}