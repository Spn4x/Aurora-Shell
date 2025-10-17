// ===================================================================
//  Aurora Launcher Widget
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

// Finalizer: Called when the object's reference count drops to zero.
static void aurora_result_object_finalize(GObject *object) {
    AuroraResultObject *self = AURORA_RESULT_OBJECT(object);
    g_free(self->name);
    g_free(self->description);
    g_free(self->icon_name);
    if (self->data_free_func && self->data) {
        self->data_free_func(self->data);
    }
    // Chain up to the parent class's finalizer.
    G_OBJECT_CLASS(aurora_result_object_parent_class)->finalize(object);
}

// Instance Initializer: Called when a new object instance is created.
static void aurora_result_object_init(AuroraResultObject *self) {
    (void)self; // Nothing to initialize here for now.
}

// Class Initializer: Called once when the type is first registered.
static void aurora_result_object_class_init(AuroraResultObjectClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = aurora_result_object_finalize;
}

// Public Constructor
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

// GDestroyNotify function to clean up the state struct when the widget is destroyed.
static void free_launcher_state(gpointer data) {
    LauncherState *state = (LauncherState *)data;
    // The GListStore holds GObjects, so unreffing it will free the result objects.
    g_object_unref(state->results_store);
    g_free(state);
}

// ===================================================================
//  Core Logic & Data Handling
// ===================================================================

// Fetches results from all modules and updates the list store.
static void update_search_results(LauncherState *state, const gchar *search_text) {
    // Clear previous results. The GListStore handles unreffing old objects.
    g_list_store_remove_all(state->results_store);

    // Fetch new results from all modules.
    GList *command_results = get_command_results(search_text);
    GList *app_results = get_app_results(search_text);
    GList *calc_results = get_calculator_results(search_text);
    
    // Combine the lists (order matters: commands, then calc, then apps).
    GList *all_results = g_list_concat(command_results, g_list_concat(calc_results, app_results));

    // Populate the store with the new results.
    for (GList *l = all_results; l != NULL; l = l->next) {
        g_list_store_append(state->results_store, l->data);
        // The list store now owns a reference, so we can unref our copy.
        g_object_unref(l->data);
    }
    g_list_free(all_results);

    // Update UI based on whether there are results.
    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(state->results_store));
    gtk_revealer_set_reveal_child(GTK_REVEALER(state->results_revealer), n_items > 0);
    if (n_items > 0) {
        // Automatically select the first item.
        GtkListBoxRow *first_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(state->listbox), 0);
        gtk_list_box_select_row(GTK_LIST_BOX(state->listbox), first_row);
    }
}

// ===================================================================
//  Signal Handlers / Callbacks
// ===================================================================

// Called when the text in the GtkEntry changes.
static void on_entry_changed(GtkEditable *entry, gpointer user_data) {
    LauncherState *state = (LauncherState *)user_data;
    const gchar *search_text = gtk_editable_get_text(entry);
    update_search_results(state, search_text);
}

// Called when a result is activated (e.g., by clicking or pressing Enter on it).
static void on_result_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    LauncherState *state = (LauncherState *)user_data;
    guint index = gtk_list_box_row_get_index(row);
    AuroraResultObject *result = g_list_model_get_item(G_LIST_MODEL(state->results_store), index);
    if (!result) return;

    switch (result->type) {
        case AURORA_RESULT_APP: {
            const gchar *exec_cmd = app_info_get_exec_cmd(APP_INFO(result->data));
            if (exec_cmd) {
                g_spawn_async(NULL, (gchar*[]){"/bin/sh", "-c", (gchar*)exec_cmd, NULL}, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
            }
            break;
        }
        case AURORA_RESULT_CALCULATOR: {
            // Copy the result to the clipboard.
            gchar *calc_result = (gchar*)result->data;
            GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(box));
            gdk_clipboard_set_text(clipboard, calc_result);
            break;
        }
        case AURORA_RESULT_COMMAND: {
            // Execute the command in a terminal.
            gchar *command = (gchar*)result->data;
            g_spawn_async(NULL, (gchar*[]){"foot", "-e", command, NULL}, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
            break;
        }
        default:
            break;
    }

    g_object_unref(result);
    // The orchestrator hides the widget via Escape, but activating an item should close it too.
    // The most reliable way is to find the top-level window and close it.
    GtkWidget *toplevel = gtk_widget_get_ancestor(GTK_WIDGET(row), GTK_TYPE_WINDOW);
    if (toplevel) {
        gtk_window_close(GTK_WINDOW(toplevel));
    }
}

// Called when Enter is pressed in the GtkEntry.
static void on_entry_activated(GtkEntry *entry, gpointer user_data) {
    (void)entry;
    LauncherState *state = (LauncherState *)user_data;
    GtkListBoxRow *selected_row = gtk_list_box_get_selected_row(GTK_LIST_BOX(state->listbox));
    if (selected_row) {
        // This triggers the "row-activated" signal on the GtkListBox.
        gtk_widget_activate(GTK_WIDGET(selected_row));
    }
}

// Handles Up/Down arrow key navigation in the results list.
static gboolean on_key_pressed_nav(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    (void)controller; (void)keycode; (void)state;
    LauncherState *launcher_state = (LauncherState *)user_data;
    GtkListBox *listbox = GTK_LIST_BOX(launcher_state->listbox);

    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down) {
        guint n_items = g_list_model_get_n_items(G_LIST_MODEL(launcher_state->results_store));
        if (n_items == 0) {
            return GDK_EVENT_PROPAGATE; // Do nothing if list is empty
        }

        GtkListBoxRow *selected_row = gtk_list_box_get_selected_row(listbox);
        gint current_index = selected_row ? gtk_list_box_row_get_index(selected_row) : -1;
        gint new_index;

        if (keyval == GDK_KEY_Down) {
            new_index = (current_index + 1) % n_items; // Wrap around to the start
        } else { // GDK_KEY_Up
            new_index = (current_index - 1 + n_items) % n_items; // Wrap around to the end
        }

        GtkListBoxRow *row_to_select = gtk_list_box_get_row_at_index(listbox, new_index);
        gtk_list_box_select_row(listbox, row_to_select);

        // Crucial: Stop the event from propagating to the GtkEntry's default handler,
        // which would move the text cursor.
        return GDK_EVENT_STOP;
    }
    // Let other keys (like text input) pass through to the entry.
    return GDK_EVENT_PROPAGATE;
}

// ===================================================================
//  UI Construction
// ===================================================================

// Creates the visual representation (the content) for a single result row.
static GtkWidget* create_result_row_ui(AuroraResultObject *result) {
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(main_box, 10);
    gtk_widget_set_margin_end(main_box, 10);
    gtk_widget_set_margin_top(main_box, 5);
    gtk_widget_set_margin_bottom(main_box, 5);

    GtkWidget *icon = NULL;
    // --- START OF FIX ---
    // Check if the icon name provided is an absolute path or just a theme name.
    if (result->icon_name && g_path_is_absolute(result->icon_name)) {
        // It's a full path, so load it as a file.
        icon = gtk_image_new_from_file(result->icon_name);
    } else {
        // It's likely a theme icon name (or NULL, in which case a default is used).
        icon = gtk_image_new_from_icon_name(result->icon_name);
    }
    // --- END OF FIX ---

    gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
    gtk_box_append(GTK_BOX(main_box), icon);

    GtkWidget *name_label = gtk_label_new(result->name);
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
    gtk_widget_add_css_class(name_label, "result-name");
    gtk_box_append(GTK_BOX(main_box), name_label);
    
    return main_box;
}


// GtkListBoxBindModelCreateWidgetFunc: Factory for creating a GtkListBoxRow from a model item.
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

// This is the public function that the Aurora Shell orchestrator calls via dlopen/dlsym.
GtkWidget* create_widget(const char *config_string) {
    // The config_string is not used by the launcher yet, but the signature must match.
    (void)config_string;

    // 1. Initialize State
    LauncherState *state = g_new0(LauncherState, 1);
    state->results_store = g_list_store_new(AURORA_RESULT_OBJECT_TYPE);
    
    // 2. Create Main Container and associate state with it for cleanup.
    state->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(state->main_box, "launcher-box");
    g_object_set_data_full(G_OBJECT(state->main_box), "launcher-state", state, free_launcher_state);

    // 3. Create Search Entry
    state->entry = gtk_entry_new();
    gtk_widget_add_css_class(state->entry, "launcher-entry");
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->entry), "Search Apps, Calculate, or > Run Command");
    
    // 4. Create Results List
    state->listbox = gtk_list_box_new();
    gtk_widget_add_css_class(state->listbox, "results-listbox");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(state->listbox), GTK_SELECTION_SINGLE);
    gtk_list_box_bind_model(GTK_LIST_BOX(state->listbox), G_LIST_MODEL(state->results_store), bind_model_create_widget_func, NULL, NULL);

    // 5. Create Scroller and Revealer for smooth presentation
    GtkWidget *scrolled_win = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scrolled_win, "results-scroller");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_win), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_win), state->listbox);

    state->results_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(state->results_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(state->results_revealer), 200);
    gtk_revealer_set_child(GTK_REVEALER(state->results_revealer), scrolled_win);

    // 6. Assemble the UI
    gtk_box_append(GTK_BOX(state->main_box), state->entry);
    gtk_box_append(GTK_BOX(state->main_box), state->results_revealer);

    // 7. Connect Signals
    g_signal_connect(state->entry, "changed", G_CALLBACK(on_entry_changed), state);
    g_signal_connect(state->entry, "activate", G_CALLBACK(on_entry_activated), state);
    g_signal_connect(state->listbox, "row-activated", G_CALLBACK(on_result_activated), state);

    // Add key controller for Up/Down navigation to the main widget.
    GtkEventController *nav_controller = gtk_event_controller_key_new();
    g_signal_connect(nav_controller, "key-pressed", G_CALLBACK(on_key_pressed_nav), state);
    gtk_widget_add_controller(state->main_box, nav_controller);

    // 8. Set Initial State
    // The orchestrator makes the window visible; we just need to focus the entry.
    gtk_widget_set_focusable(state->entry, TRUE);
    gtk_widget_grab_focus(state->entry);

    return state->main_box;
}