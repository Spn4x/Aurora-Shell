#include <gtk/gtk.h>
#include <string.h>
#include <gio/gio.h>
#include <graphene.h>
#include "launcher.h"

#define ROW_HEIGHT 54
#define MAX_VISIBLE_ROWS 7

// ===================================================================
//  Type Definitions
// ===================================================================

typedef enum {
    AURORA_RESULT_APP = 0,
    AURORA_RESULT_CALCULATOR = 1,
    AURORA_RESULT_COMMAND = 2
} AuroraResultType;

typedef enum {
    MODE_APPS = 0,
    MODE_COMMAND = 1
} LauncherMode;

#define AURORA_TYPE_RESULT_OBJECT (aurora_result_object_get_type())
G_DECLARE_FINAL_TYPE(AuroraResultObject, aurora_result_object, AURORA, RESULT_OBJECT, GObject)

struct _AuroraResultObject {
    GObject parent_instance;
    AuroraResultType type;
    gchar *name;
    gchar *description;
    gchar *icon_name;
    gchar *payload;
    gint score;
};

G_DEFINE_TYPE(AuroraResultObject, aurora_result_object, G_TYPE_OBJECT)

static void aurora_result_object_finalize(GObject *object) {
    AuroraResultObject *self = AURORA_RESULT_OBJECT(object);
    g_free(self->name);
    g_free(self->description);
    g_free(self->icon_name);
    g_free(self->payload);
    G_OBJECT_CLASS(aurora_result_object_parent_class)->finalize(object);
}

static void aurora_result_object_init(AuroraResultObject *self) { (void)self; }
static void aurora_result_object_class_init(AuroraResultObjectClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = aurora_result_object_finalize;
}

AuroraResultObject* aurora_result_object_new(AuroraResultType type, const gchar *name, const gchar *description, const gchar *icon_name, const gchar *payload, gint score) {
    AuroraResultObject *self = g_object_new(AURORA_TYPE_RESULT_OBJECT, NULL);
    self->type = type;
    self->name = g_strdup(name);
    self->description = g_strdup(description);
    self->icon_name = g_strdup(icon_name);
    self->payload = g_strdup(payload);
    self->score = score;
    return self;
}

// ===================================================================
//  Launcher State
// ===================================================================

typedef struct {
    GtkWidget *main_box;
    GtkWidget *entry;
    GtkWidget *listbox;
    GtkWidget *results_revealer;
    GListStore *results_store;
    GtkScrolledWindow *scrolled_window;
    
    LauncherMode current_mode;

    // DBus components
    GDBusProxy *search_proxy;
    GCancellable *cancellable;
} LauncherState;

static void free_launcher_state(gpointer data) {
    LauncherState *state = (LauncherState *)data;
    if (state->cancellable) {
        g_cancellable_cancel(state->cancellable);
        g_object_unref(state->cancellable);
    }
    if (state->search_proxy) g_object_unref(state->search_proxy);
    if (state->results_store) g_object_unref(state->results_store);
    g_free(state);
}

// ===================================================================
//  UI Updates & Auto-Scroll
// ===================================================================

static void update_launcher_mode_ui(LauncherState *state) {
    if (state->current_mode == MODE_APPS) {
        gtk_entry_set_placeholder_text(GTK_ENTRY(state->entry), "Search Apps or Math...");
        gtk_entry_set_icon_from_icon_name(GTK_ENTRY(state->entry), GTK_ENTRY_ICON_PRIMARY, "system-search-symbolic");
    } else {
        gtk_entry_set_placeholder_text(GTK_ENTRY(state->entry), "Run Command...");
        gtk_entry_set_icon_from_icon_name(GTK_ENTRY(state->entry), GTK_ENTRY_ICON_PRIMARY, "utilities-terminal-symbolic");
    }
}

static void ensure_row_visible(LauncherState *state, GtkWidget *row) {
    if (!state->scrolled_window || !row) return;
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(state->scrolled_window);
    double val = gtk_adjustment_get_value(adj);
    double page = gtk_adjustment_get_page_size(adj);
    graphene_rect_t bounds;
    if (!gtk_widget_compute_bounds(row, state->listbox, &bounds)) return;
    double y = bounds.origin.y;
    double h = bounds.size.height;
    if (y < val) gtk_adjustment_set_value(adj, y);
    else if (y + h > val + page) gtk_adjustment_set_value(adj, y + h - page);
}

// ===================================================================
//  D-Bus Callbacks & Interaction
// ===================================================================

static void on_query_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    LauncherState *state = (LauncherState *)user_data;
    GError *error = NULL;
    
    GVariant *result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source_object), res, &error);
    
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        return;
    }
    
    if (error) {
        g_warning("Search D-Bus call failed: %s", error->message);
        g_error_free(error);
        return;
    }

    g_list_store_remove_all(state->results_store);

    if (result) {
        GVariantIter *iter;
        g_variant_get(result, "(a(ussssi))", &iter);
        
        guint32 type;
        gchar *title, *desc, *icon, *payload;
        gint32 score;
        
        while (g_variant_iter_loop(iter, "(ussssi)", &type, &title, &desc, &icon, &payload, &score)) {
            AuroraResultObject *obj = aurora_result_object_new(
                (AuroraResultType)type, title, desc, icon, payload, score
            );
            g_list_store_append(state->results_store, obj);
            g_object_unref(obj);
        }
        g_variant_iter_free(iter);
        g_variant_unref(result);
    }

    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(state->results_store));
    
    if (n_items > 0) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(state->results_revealer), TRUE);
        int visible_rows = (n_items > MAX_VISIBLE_ROWS) ? MAX_VISIBLE_ROWS : n_items;
        int target_height = visible_rows * ROW_HEIGHT;
        gtk_scrolled_window_set_min_content_height(state->scrolled_window, target_height);
        
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(state->scrolled_window);
        gtk_adjustment_set_value(adj, 0);

        GtkListBoxRow *first_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(state->listbox), 0);
        if (first_row) gtk_list_box_select_row(GTK_LIST_BOX(state->listbox), first_row);
    } else {
        gtk_revealer_set_reveal_child(GTK_REVEALER(state->results_revealer), FALSE);
    }
}

static void on_entry_changed(GtkEditable *entry, gpointer user_data) {
    LauncherState *state = (LauncherState *)user_data;
    const gchar *raw_text = gtk_editable_get_text(entry);

    if (!raw_text || strlen(raw_text) == 0) {
        if (state->cancellable) {
            g_cancellable_cancel(state->cancellable);
        }
        g_list_store_remove_all(state->results_store);
        gtk_revealer_set_reveal_child(GTK_REVEALER(state->results_revealer), FALSE);
        return;
    }

    if (!state->search_proxy) return;

    if (state->cancellable) {
        g_cancellable_cancel(state->cancellable);
        g_object_unref(state->cancellable);
    }
    state->cancellable = g_cancellable_new();

    // Silently prepend "> " if we are in command mode
    g_autofree gchar *search_text = NULL;
    if (state->current_mode == MODE_COMMAND) {
        search_text = g_strdup_printf("> %s", raw_text);
    } else {
        search_text = g_strdup(raw_text);
    }

    g_dbus_proxy_call(
        state->search_proxy,
        "Query",
        g_variant_new("(s)", search_text),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        state->cancellable,
        on_query_ready,
        state
    );
}


static void on_result_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)user_data;
    if (!row) return;
    guint index = gtk_list_box_row_get_index(row);
    GtkWidget *parent_box = gtk_widget_get_ancestor(GTK_WIDGET(box), GTK_TYPE_BOX);
    LauncherState *state = (LauncherState*)g_object_get_data(G_OBJECT(parent_box), "launcher-state");
    AuroraResultObject *result = g_list_model_get_item(G_LIST_MODEL(state->results_store), index);
    if (!result) return;

    switch (result->type) {
        case AURORA_RESULT_APP: {
            GList *all_apps = g_app_info_get_all();
            GAppInfo *target_app = NULL;
            for (GList *l = all_apps; l != NULL; l = l->next) {
                GAppInfo *info = G_APP_INFO(l->data);
                const char *id = g_app_info_get_id(info);
                if (id && g_strcmp0(id, result->payload) == 0) {
                    target_app = info;
                    break;
                }
            }
            if (target_app) {
                GError *error = NULL;
                if (!g_app_info_launch(target_app, NULL, NULL, &error)) {
                    g_warning("Failed to launch app '%s': %s", result->payload, error->message);
                    g_error_free(error);
                }
            }
            g_list_free_full(all_apps, g_object_unref);
            break;
        }
        case AURORA_RESULT_CALCULATOR:
            gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(box)), result->payload);
            break;
        case AURORA_RESULT_COMMAND: {
            // THE FIX: Execute directly via the system shell instead of forcing 'foot'
            GError *error = NULL;
            if (!g_spawn_command_line_async(result->payload, &error)) {
                g_warning("Failed to execute command '%s': %s", result->payload, error->message);
                g_error_free(error);
            }
            break;
        }
        default: break;
    }
    g_object_unref(result);
    
    // Hide the launcher window after an action is taken
    GtkWidget *toplevel = gtk_widget_get_ancestor(GTK_WIDGET(row), GTK_TYPE_WINDOW);
    if (toplevel) gtk_widget_set_visible(toplevel, FALSE);
}

static void on_widget_mapped(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    LauncherState *state = (LauncherState *)user_data;
    gtk_editable_set_text(GTK_EDITABLE(state->entry), "");
    gtk_widget_grab_focus(state->entry);
}

static void on_entry_activated(GtkEntry *entry, gpointer user_data) {
    (void)entry;
    LauncherState *state = (LauncherState *)user_data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(state->listbox));
    if (row) gtk_widget_activate(GTK_WIDGET(row));
}

static gboolean on_key_pressed_nav(GtkEventControllerKey *c, guint keyval, guint kc, GdkModifierType s, gpointer user_data) {
    (void)c; (void)kc; (void)s;
    LauncherState *state = (LauncherState *)user_data;
    
    // --- MODE SWITCHING (TAB) ---
    if (keyval == GDK_KEY_Tab) {
        state->current_mode = (state->current_mode == MODE_APPS) ? MODE_COMMAND : MODE_APPS;
        update_launcher_mode_ui(state);
        // Re-trigger search to update results instantly based on new mode
        on_entry_changed(GTK_EDITABLE(state->entry), state);
        return GDK_EVENT_STOP;
    }
    
    // --- VERTICAL NAVIGATION ---
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down) {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(state->results_store));
        if (n == 0) return GDK_EVENT_PROPAGATE;
        GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(state->listbox));
        gint idx = row ? gtk_list_box_row_get_index(row) : -1;
        gint next = (keyval == GDK_KEY_Down) ? (idx + 1) % n : (idx - 1 + n) % n;
        GtkListBoxRow *next_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(state->listbox), next);
        gtk_list_box_select_row(GTK_LIST_BOX(state->listbox), next_row);
        if (next_row) ensure_row_visible(state, GTK_WIDGET(next_row));
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

// ===================================================================
//  UI Construction
// ===================================================================

static GtkWidget* create_result_row_ui(AuroraResultObject *result) {
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(main_box, 8);
    gtk_widget_set_margin_end(main_box, 8);
    gtk_widget_set_size_request(main_box, -1, 42); 

    GtkWidget *icon = NULL;
    if (result->icon_name && g_path_is_absolute(result->icon_name)) {
        icon = gtk_image_new_from_file(result->icon_name);
    } else {
        icon = gtk_image_new_from_icon_name(result->icon_name);
    }
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
    gtk_box_append(GTK_BOX(main_box), icon);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

    GtkWidget *name_label = gtk_label_new(result->name);
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
    gtk_widget_add_css_class(name_label, "result-name");
    gtk_label_set_ellipsize(GTK_LABEL(name_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(name_label), 40);
    gtk_widget_set_hexpand(name_label, TRUE);
    gtk_widget_set_halign(name_label, GTK_ALIGN_START);

    GtkWidget *desc_label = gtk_label_new(result->description);
    gtk_label_set_xalign(GTK_LABEL(desc_label), 0.0);
    gtk_widget_add_css_class(desc_label, "result-desc");
    gtk_widget_set_opacity(desc_label, 0.7);
    gtk_label_set_ellipsize(GTK_LABEL(desc_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(desc_label), 50);
    gtk_widget_set_hexpand(desc_label, TRUE);
    gtk_widget_set_halign(desc_label, GTK_ALIGN_START);

    gtk_box_append(GTK_BOX(vbox), name_label);
    gtk_box_append(GTK_BOX(vbox), desc_label);
    gtk_box_append(GTK_BOX(main_box), vbox);
    
    return main_box;
}

static GtkWidget* bind_model_create_widget_func(gpointer item, gpointer user_data) {
    (void)user_data;
    GtkListBoxRow *row = GTK_LIST_BOX_ROW(gtk_list_box_row_new());
    gtk_widget_add_css_class(GTK_WIDGET(row), "app-row");
    gtk_widget_set_margin_top(GTK_WIDGET(row), 4);
    gtk_widget_set_margin_bottom(GTK_WIDGET(row), 4);
    
    GtkWidget *content = create_result_row_ui(AURORA_RESULT_OBJECT(item));
    gtk_list_box_row_set_child(row, content);
    return GTK_WIDGET(row);
}

static void on_dbus_proxy_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object;
    LauncherState *state = (LauncherState*)user_data;
    GError *error = NULL;
    state->search_proxy = g_dbus_proxy_new_for_bus_finish(res, &error);
    if (error) {
        g_warning("Launcher failed to connect to Search Daemon: %s", error->message);
        g_error_free(error);
    }
}

G_MODULE_EXPORT GtkWidget* create_widget(const char *config_string) {
    (void)config_string;
    
    LauncherState *state = g_new0(LauncherState, 1);
    state->results_store = g_list_store_new(AURORA_TYPE_RESULT_OBJECT);
    state->current_mode = MODE_APPS; // Set initial mode
    
    // Connect to the DBus daemon asynchronously
    g_dbus_proxy_new_for_bus(
        G_BUS_TYPE_SESSION,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,
        "com.meismeric.auroralauncher",
        "/com/meismeric/auroralauncher",
        "com.meismeric.auroralauncher.Search",
        NULL,
        on_dbus_proxy_ready,
        state
    );
    
    state->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(state->main_box, "launcher-box");
    g_object_set_data_full(G_OBJECT(state->main_box), "launcher-state", state, free_launcher_state);
    gtk_widget_set_valign(state->main_box, GTK_ALIGN_CENTER);
    g_signal_connect(state->main_box, "map", G_CALLBACK(on_widget_mapped), state);

    GtkCssProvider *css_provider = gtk_css_provider_new();
    const char *css = 
        ".launcher-scroll scrollbar { opacity: 0; min-width: 0px; min-height: 0px; }"
        ".launcher-scroll scrollbar slider { min-width: 0px; min-height: 0px; }"
        ".launcher-scroll contents { border: none; }";
    gtk_css_provider_load_from_string(css_provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css_provider);

    state->entry = gtk_entry_new();
    gtk_widget_add_css_class(state->entry, "launcher-entry");
    
    // Apply the initial UI (placeholder & icon)
    update_launcher_mode_ui(state);
    
    state->listbox = gtk_list_box_new();
    gtk_widget_add_css_class(state->listbox, "results-listbox");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(state->listbox), GTK_SELECTION_SINGLE);
    gtk_list_box_bind_model(GTK_LIST_BOX(state->listbox), G_LIST_MODEL(state->results_store), bind_model_create_widget_func, NULL, NULL);

    state->scrolled_window = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->scrolled_window), "results-scroller");
    gtk_widget_add_css_class(GTK_WIDGET(state->scrolled_window), "launcher-scroll");

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(state->scrolled_window), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(state->scrolled_window, FALSE);
    
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(state->scrolled_window), GTK_WIDGET(state->listbox));

    state->results_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(state->results_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(state->results_revealer), 150);
    gtk_revealer_set_child(GTK_REVEALER(state->results_revealer), GTK_WIDGET(state->scrolled_window));

    gtk_box_append(GTK_BOX(state->main_box), state->entry);
    gtk_box_append(GTK_BOX(state->main_box), state->results_revealer);

    g_signal_connect(state->entry, "changed", G_CALLBACK(on_entry_changed), state);
    g_signal_connect(state->entry, "activate", G_CALLBACK(on_entry_activated), state);
    g_signal_connect(state->listbox, "row-activated", G_CALLBACK(on_result_activated), state);

    GtkEventController *nav = gtk_event_controller_key_new();
    
    // Set to CAPTURE phase so GTK's default focus logic doesn't eat the Tab key
    gtk_event_controller_set_propagation_phase(nav, GTK_PHASE_CAPTURE);
    
    g_signal_connect(nav, "key-pressed", G_CALLBACK(on_key_pressed_nav), state);
    gtk_widget_add_controller(state->main_box, nav);

    return state->main_box;
}