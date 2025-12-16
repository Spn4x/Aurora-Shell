// widgets/launcher/launcher.c

#include <gtk/gtk.h>
#include <string.h>
#include <gio/gio.h>
#include <graphene.h> // Required for rect calculations
#include "launcher.h"

// ===================================================================
//  Type Definitions & Rust FFI
// ===================================================================

typedef enum {
    AURORA_RESULT_APP,
    AURORA_RESULT_CALCULATOR,
    AURORA_RESULT_COMMAND,
    AURORA_RESULT_SYSTEM_ACTION,
    AURORA_RESULT_FILE
} AuroraResultType;

typedef struct {
    char *title;
    char *description;
    char *icon;
    uint32_t type;
    int score;
    char *exec_data;
} AuroraSearchResult;

extern AuroraSearchResult* aurora_backend_query(const char *query, size_t *count);
extern void aurora_backend_free_results(AuroraSearchResult *ptr, size_t count);

// ===================================================================
//  GObject Definition
// ===================================================================

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
} LauncherState;

static void free_launcher_state(gpointer data) {
    LauncherState *state = (LauncherState *)data;
    if (state->results_store) g_object_unref(state->results_store);
    g_free(state);
}

// ===================================================================
//  Helper: Auto-Scroll
// ===================================================================

static void ensure_row_visible(LauncherState *state, GtkWidget *row) {
    if (!state->scrolled_window || !row) return;

    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(state->scrolled_window);
    double val = gtk_adjustment_get_value(adj);
    double page = gtk_adjustment_get_page_size(adj);

    // Calculate row bounds relative to the listbox (the scrollable content)
    graphene_rect_t bounds;
    if (!gtk_widget_compute_bounds(row, state->listbox, &bounds)) return;

    double y = bounds.origin.y;
    double h = bounds.size.height;

    // If row is above visible area, scroll up
    if (y < val) {
        gtk_adjustment_set_value(adj, y);
    } 
    // If row is below visible area, scroll down
    else if (y + h > val + page) {
        gtk_adjustment_set_value(adj, y + h - page);
    }
}

// ===================================================================
//  Logic & Interaction
// ===================================================================

static void update_search_results(LauncherState *state, const gchar *search_text) {
    g_list_store_remove_all(state->results_store);

    if (!search_text || strlen(search_text) == 0) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(state->results_revealer), FALSE);
        return;
    }

    size_t count = 0;
    AuroraSearchResult *results = aurora_backend_query(search_text, &count);

    if (results) {
        for (size_t i = 0; i < count; i++) {
            AuroraResultType type_enum = AURORA_RESULT_APP;
            if (results[i].type == 1) type_enum = AURORA_RESULT_CALCULATOR;
            if (results[i].type == 2) type_enum = AURORA_RESULT_COMMAND;

            AuroraResultObject *obj = aurora_result_object_new(
                type_enum,
                results[i].title,
                results[i].description,
                results[i].icon,
                results[i].exec_data,
                results[i].score
            );
            g_list_store_append(state->results_store, obj);
            g_object_unref(obj);
        }
        aurora_backend_free_results(results, count);
    }

    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(state->results_store));
    
    gtk_revealer_set_reveal_child(GTK_REVEALER(state->results_revealer), n_items > 0);
    
    if (n_items > 0) {
        GtkListBoxRow *first_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(state->listbox), 0);
        if (first_row) {
            gtk_list_box_select_row(GTK_LIST_BOX(state->listbox), first_row);
            // Reset scroll to top on new search
            GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(state->scrolled_window);
            gtk_adjustment_set_value(adj, 0);
        }
    }
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
                g_app_info_launch(target_app, NULL, NULL, NULL);
            }
            g_list_free_full(all_apps, g_object_unref);
            break;
        }
        case AURORA_RESULT_CALCULATOR:
            gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(box)), result->payload);
            break;
        case AURORA_RESULT_COMMAND:
            g_spawn_async(NULL, (gchar*[]){"foot", "-e", "sh", "-c", result->payload, NULL}, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
            break;
        default: break;
    }
    g_object_unref(result);
    
    GtkWidget *toplevel = gtk_widget_get_ancestor(GTK_WIDGET(row), GTK_TYPE_WINDOW);
    if (toplevel) gtk_widget_set_visible(toplevel, FALSE);
}

static void on_widget_mapped(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    LauncherState *state = (LauncherState *)user_data;
    gtk_editable_set_text(GTK_EDITABLE(state->entry), "");
    gtk_widget_grab_focus(state->entry);
}

static void on_entry_changed(GtkEditable *entry, gpointer user_data) {
    LauncherState *state = (LauncherState *)user_data;
    update_search_results(state, gtk_editable_get_text(entry));
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
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down) {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(state->results_store));
        if (n == 0) return GDK_EVENT_PROPAGATE;
        
        GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(state->listbox));
        gint idx = row ? gtk_list_box_row_get_index(row) : -1;
        gint next = (keyval == GDK_KEY_Down) ? (idx + 1) % n : (idx - 1 + n) % n;
        
        GtkListBoxRow *next_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(state->listbox), next);
        gtk_list_box_select_row(GTK_LIST_BOX(state->listbox), next_row);
        
        // --- NEW: Force scroll to ensure visibility ---
        if (next_row) {
            ensure_row_visible(state, GTK_WIDGET(next_row));
        }
        
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

// ===================================================================
//  UI Construction
// ===================================================================

static GtkWidget* create_result_row_ui(AuroraResultObject *result) {
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    // REMOVED MARGINS to fix height calculation issues
    gtk_widget_set_margin_start(main_box, 8);
    gtk_widget_set_margin_end(main_box, 8);

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
    GtkWidget *content = create_result_row_ui(AURORA_RESULT_OBJECT(item));
    gtk_list_box_row_set_child(row, content);
    return GTK_WIDGET(row);
}

G_MODULE_EXPORT GtkWidget* create_widget(const char *config_string) {
    (void)config_string;
    LauncherState *state = g_new0(LauncherState, 1);
    state->results_store = g_list_store_new(AURORA_TYPE_RESULT_OBJECT);
    
    state->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(state->main_box, "launcher-box");
    g_object_set_data_full(G_OBJECT(state->main_box), "launcher-state", state, free_launcher_state);
    gtk_widget_set_valign(state->main_box, GTK_ALIGN_CENTER);
    g_signal_connect(state->main_box, "map", G_CALLBACK(on_widget_mapped), state);

    // --- FORCE HIDE SCROLLBAR VIA CSS INJECTION ---
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
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->entry), "Search Apps, Math, or > Commands");
    
    state->listbox = gtk_list_box_new();
    gtk_widget_add_css_class(state->listbox, "results-listbox");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(state->listbox), GTK_SELECTION_SINGLE);
    gtk_list_box_bind_model(GTK_LIST_BOX(state->listbox), G_LIST_MODEL(state->results_store), bind_model_create_widget_func, NULL, NULL);

    state->scrolled_window = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->scrolled_window), "results-scroller");
    gtk_widget_add_css_class(GTK_WIDGET(state->scrolled_window), "launcher-scroll");

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(state->scrolled_window), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(state->scrolled_window), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(state->scrolled_window), 400);
    
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(state->scrolled_window), GTK_WIDGET(state->listbox));

    state->results_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(state->results_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(state->results_revealer), 200);
    gtk_revealer_set_child(GTK_REVEALER(state->results_revealer), GTK_WIDGET(state->scrolled_window));

    gtk_box_append(GTK_BOX(state->main_box), state->entry);
    gtk_box_append(GTK_BOX(state->main_box), state->results_revealer);

    g_signal_connect(state->entry, "changed", G_CALLBACK(on_entry_changed), state);
    g_signal_connect(state->entry, "activate", G_CALLBACK(on_entry_activated), state);
    g_signal_connect(state->listbox, "row-activated", G_CALLBACK(on_result_activated), state);

    GtkEventController *nav = gtk_event_controller_key_new();
    g_signal_connect(nav, "key-pressed", G_CALLBACK(on_key_pressed_nav), state);
    gtk_widget_add_controller(state->main_box, nav);

    return state->main_box;
}