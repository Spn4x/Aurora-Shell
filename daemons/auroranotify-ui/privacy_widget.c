#include "privacy_widget.h"
#include <json-glib/json-glib.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    guint32 pid;
    gchar *name;
    gboolean uses_mic;
    gboolean uses_cam;
} PrivacyApp;

typedef struct {
    guint32 pid;
    gchar *name;
    int type;
} RawApp;

static GList *privacy_apps = NULL;      
static GList *ignored_pids = NULL;      
static GList *ignored_names = NULL; 
static GtkWidget *privacy_stack = NULL; 
static guint privacy_cycle_id = 0;
static gboolean privacy_showing_view_a = TRUE;
static gboolean has_announced_privacy = FALSE; 

static GtkWidget *pill_label_a = NULL;
static GtkWidget *pill_label_b = NULL;
static GtkWidget *pill_dot = NULL;
static GtkWidget *dash_list_box = NULL;
static GtkWidget *dash_kill_all_btn = NULL;

static PrivacyStateChangedCb on_state_changed = NULL;

void privacy_widget_init(PrivacyStateChangedCb callback) {
    on_state_changed = callback;
}

static void free_privacy_app(gpointer data) {
    if (!data) return;
    PrivacyApp *app = (PrivacyApp*)data;
    g_free(app->name);
    g_free(app);
}

static void clear_ui_refs() {
    pill_label_a = NULL; pill_label_b = NULL; pill_dot = NULL;
    dash_list_box = NULL; dash_kill_all_btn = NULL; privacy_stack = NULL;
}

static void on_pill_destroyed(GtkWidget *w, gpointer d) { (void)w; (void)d; clear_ui_refs(); }
static void on_dash_destroyed(GtkWidget *w, gpointer d) { (void)w; (void)d; dash_list_box = NULL; dash_kill_all_btn = NULL; }

void privacy_widget_cleanup(void) {
    g_list_free_full(privacy_apps, free_privacy_app);
    g_list_free(ignored_pids);
    g_list_free_full(ignored_names, g_free);
    privacy_apps = NULL; ignored_pids = NULL; ignored_names = NULL;
    if (privacy_cycle_id > 0) { g_source_remove(privacy_cycle_id); privacy_cycle_id = 0; }
    clear_ui_refs();
}

gboolean privacy_widget_has_active_apps(void) { return g_list_length(privacy_apps) > 0; }
gboolean privacy_widget_has_dashboard(void) { return dash_list_box != NULL; }

static void notify_state_changed() {
    if (on_state_changed) on_state_changed();
}

static void on_privacy_ignore_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    guint32 pid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "app-pid"));
    const char *name = g_object_get_data(G_OBJECT(btn), "app-name");
    
    if (pid > 0) ignored_pids = g_list_append(ignored_pids, GUINT_TO_POINTER(pid));
    else if (name) ignored_names = g_list_append(ignored_names, g_strdup(name));
    
    for (GList *l = privacy_apps; l != NULL; l = l->next) {
        PrivacyApp *p = (PrivacyApp*)l->data;
        if ((pid > 0 && p->pid == pid) || (pid == 0 && p->pid == 0 && g_strcmp0(p->name, name) == 0)) {
            free_privacy_app(p);
            privacy_apps = g_list_delete_link(privacy_apps, l);
            break;
        }
    }
    privacy_widget_refresh_ui();
    notify_state_changed();
}

static void execute_smart_kill(guint32 pid, const char *name) {
    if (pid > 0) {
        kill(pid, SIGTERM);
    } else if (name) {
        g_autofree gchar *lower_name = g_ascii_strdown(name, -1);
        g_autofree gchar *cmd = NULL;

        if (strstr(lower_name, "brave")) cmd = g_strdup("pkill -i brave");
        else if (strstr(lower_name, "obs")) cmd = g_strdup("pkill -i obs");
        else if (strstr(lower_name, "discord") || strstr(lower_name, "webcord")) cmd = g_strdup("pkill -i discord || pkill -i webcord");
        else if (strstr(lower_name, "cava")) cmd = g_strdup("pkill -i cava");
        else if (strstr(lower_name, "firefox")) cmd = g_strdup("pkill -i firefox");
        else if (strstr(lower_name, "chrome")) cmd = g_strdup("pkill -i chrome");
        else {
            gchar **parts = g_strsplit(name, " ", 2);
            if (parts && parts[0]) cmd = g_strdup_printf("pkill -i '%s'", parts[0]);
            g_strfreev(parts);
        }
        if (cmd) {
            system(cmd);
        }
    }
}

static void on_privacy_kill_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    guint32 pid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "app-pid"));
    const char *name = g_object_get_data(G_OBJECT(btn), "app-name");
    
    execute_smart_kill(pid, name);
    
    for (GList *l = privacy_apps; l != NULL; l = l->next) {
        PrivacyApp *p = (PrivacyApp*)l->data;
        if ((pid > 0 && p->pid == pid) || (pid == 0 && p->pid == 0 && g_strcmp0(p->name, name) == 0)) {
            free_privacy_app(p);
            privacy_apps = g_list_delete_link(privacy_apps, l);
            break;
        }
    }
    privacy_widget_refresh_ui();
    notify_state_changed();
}

static void on_privacy_kill_all_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    GList *l = privacy_apps;
    while (l != NULL) {
        PrivacyApp *p = (PrivacyApp*)l->data;
        GList *next = l->next;
        execute_smart_kill(p->pid, p->name);
        free_privacy_app(p);
        privacy_apps = g_list_delete_link(privacy_apps, l);
        l = next;
    }
    privacy_widget_refresh_ui();
    notify_state_changed();
}

static gboolean on_privacy_cycle_tick(gpointer data) {
    (void)data;
    if (!privacy_stack) {
        privacy_cycle_id = 0;
        return G_SOURCE_REMOVE;
    }
    privacy_showing_view_a = FALSE;
    gtk_stack_set_visible_child_name(GTK_STACK(privacy_stack), "view_b");
    privacy_cycle_id = 0; 
    return G_SOURCE_REMOVE; 
}

void privacy_widget_update_from_json(const gchar *json_payload) {
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_payload, -1, NULL)) return;
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_ARRAY(root)) return;
    
    JsonArray *array = json_node_get_array(root);
    GList *raw_apps = NULL;
    
    // 1. Gather absolute source of truth from PipeWire
    for (guint i = 0; i < json_array_get_length(array); i++) {
        JsonObject *obj = json_array_get_object_element(array, i);
        RawApp *ra = g_new0(RawApp, 1);
        ra->pid = json_object_get_int_member(obj, "pid");
        ra->name = g_strdup(json_object_get_string_member(obj, "name"));
        ra->type = json_object_get_int_member(obj, "type");
        raw_apps = g_list_append(raw_apps, ra);
    }
    
    // 2. TEMPORARY IGNORES: Clean up ignores for apps that closed their streams
    GList *lp = ignored_pids;
    while (lp != NULL) {
        guint32 ig_pid = GPOINTER_TO_UINT(lp->data);
        gboolean still_running = FALSE;
        for (GList *r = raw_apps; r != NULL; r = r->next) {
            if (((RawApp*)r->data)->pid == ig_pid) { still_running = TRUE; break; }
        }
        GList *next = lp->next;
        if (!still_running) {
            ignored_pids = g_list_delete_link(ignored_pids, lp);
        }
        lp = next;
    }

    GList *ln = ignored_names;
    while (ln != NULL) {
        const char *ig_name = (const char*)ln->data;
        gboolean still_running = FALSE;
        for (GList *r = raw_apps; r != NULL; r = r->next) {
            RawApp *ra = (RawApp*)r->data;
            if (ra->pid == 0 && g_strcmp0(ra->name, ig_name) == 0) { still_running = TRUE; break; }
        }
        GList *next = ln->next;
        if (!still_running) {
            g_free(ln->data);
            ignored_names = g_list_delete_link(ignored_names, ln);
        }
        ln = next;
    }

    // 3. Build filtered visual list
    GList *new_apps = NULL;
    for (GList *r = raw_apps; r != NULL; r = r->next) {
        RawApp *ra = (RawApp*)r->data;
        
        if (ra->pid > 0 && g_list_find(ignored_pids, GUINT_TO_POINTER(ra->pid)) != NULL) continue;
        if (ra->pid == 0 && g_list_find_custom(ignored_names, ra->name, (GCompareFunc)g_strcmp0) != NULL) continue;
        
        PrivacyApp *existing = NULL;
        for (GList *l = new_apps; l != NULL; l = l->next) {
            PrivacyApp *p = (PrivacyApp*)l->data;
            if (ra->pid > 0 && p->pid == ra->pid) { existing = p; break; }
            if (ra->pid == 0 && p->pid == 0 && g_strcmp0(p->name, ra->name) == 0) { existing = p; break; }
        }
        
        if (existing) {
            if (ra->type == 0) existing->uses_mic = TRUE;
            if (ra->type == 1) existing->uses_cam = TRUE;
        } else {
            PrivacyApp *p = g_new0(PrivacyApp, 1);
            p->pid = ra->pid; 
            p->name = g_strdup(ra->name);
            if (ra->type == 0) p->uses_mic = TRUE;
            if (ra->type == 1) p->uses_cam = TRUE;
            new_apps = g_list_append(new_apps, p);
        }
    }

    // Free temporary raw list
    for (GList *r = raw_apps; r != NULL; r = r->next) {
        RawApp *ra = (RawApp*)r->data;
        g_free(ra->name);
        g_free(ra);
    }
    g_list_free(raw_apps);
    
    // 4. Check for changes
    gboolean changed = FALSE;
    gboolean app_added = FALSE;

    if (g_list_length(new_apps) != g_list_length(privacy_apps)) {
        changed = TRUE;
    } else {
        for (GList *l = new_apps; l != NULL; l = l->next) {
            PrivacyApp *n = (PrivacyApp*)l->data;
            gboolean found_match = FALSE;
            for (GList *o = privacy_apps; o != NULL; o = o->next) {
                PrivacyApp *old = (PrivacyApp*)o->data;
                if (n->pid == old->pid && g_strcmp0(n->name, old->name) == 0 && n->uses_mic == old->uses_mic && n->uses_cam == old->uses_cam) {
                    found_match = TRUE; break;
                }
            }
            if (!found_match) { changed = TRUE; break; }
        }
    }

    if (changed) {
        for (GList *l = new_apps; l != NULL; l = l->next) {
            PrivacyApp *n = l->data;
            gboolean found = FALSE;
            for (GList *o = privacy_apps; o != NULL; o = o->next) {
                PrivacyApp *old = o->data;
                if ((n->pid > 0 && n->pid == old->pid) || (n->pid == 0 && g_strcmp0(n->name, old->name) == 0)) { 
                    found = TRUE; break; 
                }
            }
            if (!found) { app_added = TRUE; break; }
        }

        g_list_free_full(privacy_apps, free_privacy_app);
        privacy_apps = new_apps; 
        
        if (privacy_apps == NULL) {
            has_announced_privacy = FALSE;
        } else if (app_added) {
            if (privacy_stack) {
                privacy_showing_view_a = TRUE;
                gtk_stack_set_visible_child_name(GTK_STACK(privacy_stack), "view_a");
                if (privacy_cycle_id > 0) g_source_remove(privacy_cycle_id);
                privacy_cycle_id = g_timeout_add_seconds(3, on_privacy_cycle_tick, NULL);
            }
            has_announced_privacy = TRUE;
        }
        notify_state_changed();
    } else {
        g_list_free_full(new_apps, free_privacy_app); 
    }
}

void privacy_widget_refresh_ui(void) {
    gint app_count = g_list_length(privacy_apps);
    gboolean has_mic = FALSE, has_cam = FALSE;
    int killable_count = 0;

    for (GList *l = privacy_apps; l != NULL; l = l->next) {
        PrivacyApp *p = (PrivacyApp*)l->data;
        if (p->uses_mic) has_mic = TRUE;
        if (p->uses_cam) has_cam = TRUE;
        killable_count++;
    }

    if (pill_label_a) {
        g_autofree gchar *title_a = NULL;
        if (app_count == 1) title_a = g_strdup_printf("%s is active", ((PrivacyApp*)privacy_apps->data)->name);
        else title_a = g_strdup_printf("%d Apps active", app_count);
        gtk_label_set_text(GTK_LABEL(pill_label_a), title_a);
    }

    if (pill_dot && pill_label_b) {
        gtk_widget_remove_css_class(pill_dot, "both");
        gtk_widget_remove_css_class(pill_dot, "mic");
        gtk_widget_remove_css_class(pill_dot, "cam");
        
        const char *text_b = "";
        if (has_mic && has_cam) { gtk_widget_add_css_class(pill_dot, "both"); text_b = "Mic & Camera in use"; }
        else if (has_mic) { gtk_widget_add_css_class(pill_dot, "mic"); text_b = "Microphone in use"; }
        else if (has_cam) { gtk_widget_add_css_class(pill_dot, "cam"); text_b = "Camera/Screen in use"; }
        gtk_label_set_text(GTK_LABEL(pill_label_b), text_b);
    }

    if (dash_list_box) {
        GtkWidget *child;
        while ((child = gtk_widget_get_first_child(dash_list_box))) gtk_box_remove(GTK_BOX(dash_list_box), child);

        for (GList *l = privacy_apps; l != NULL; l = l->next) {
            PrivacyApp *p = (PrivacyApp*)l->data;
            
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_add_css_class(row, "privacy-row");
            
            GtkWidget *name = gtk_label_new(p->name);
            gtk_widget_set_hexpand(name, TRUE);
            gtk_widget_set_halign(name, GTK_ALIGN_START);
            gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
            gtk_box_append(GTK_BOX(row), name);
            
            if (p->uses_mic) {
                GtkWidget *mic = gtk_image_new_from_icon_name("audio-input-microphone-symbolic");
                gtk_widget_add_css_class(mic, "privacy-icon-mic");
                gtk_box_append(GTK_BOX(row), mic);
            }
            if (p->uses_cam) {
                GtkWidget *cam = gtk_image_new_from_icon_name("camera-web-symbolic");
                gtk_widget_add_css_class(cam, "privacy-icon-cam");
                gtk_box_append(GTK_BOX(row), cam);
            }

            GtkWidget *btn_ignore = gtk_button_new_with_label("Ignore");
            gtk_widget_add_css_class(btn_ignore, "flat");
            gtk_widget_add_css_class(btn_ignore, "privacy-action-btn");
            g_object_set_data(G_OBJECT(btn_ignore), "app-pid", GUINT_TO_POINTER(p->pid));
            g_object_set_data_full(G_OBJECT(btn_ignore), "app-name", g_strdup(p->name), g_free);
            g_signal_connect(btn_ignore, "clicked", G_CALLBACK(on_privacy_ignore_clicked), NULL);
            gtk_box_append(GTK_BOX(row), btn_ignore);

            GtkWidget *btn_kill = gtk_button_new_with_label("Kill");
            gtk_widget_add_css_class(btn_kill, "flat");
            gtk_widget_add_css_class(btn_kill, "privacy-action-btn");
            gtk_widget_add_css_class(btn_kill, "privacy-kill-btn");
            g_object_set_data(G_OBJECT(btn_kill), "app-pid", GUINT_TO_POINTER(p->pid));
            g_object_set_data_full(G_OBJECT(btn_kill), "app-name", g_strdup(p->name), g_free);
            g_signal_connect(btn_kill, "clicked", G_CALLBACK(on_privacy_kill_clicked), NULL);
            gtk_box_append(GTK_BOX(row), btn_kill);
            
            gtk_box_append(GTK_BOX(dash_list_box), row);
        }
    }

    if (dash_kill_all_btn) {
        gtk_widget_set_visible(dash_kill_all_btn, killable_count > 1);
        gtk_widget_set_sensitive(dash_kill_all_btn, killable_count > 0);
    }
}

GtkWidget* privacy_widget_create_pill(void) {
    GtkWidget *overlay = gtk_overlay_new();
    g_signal_connect(overlay, "destroy", G_CALLBACK(on_pill_destroyed), NULL);

    GtkWidget *dummy_label = gtk_label_new(" ");
    gtk_widget_add_css_class(dummy_label, "summary");
    gtk_label_set_width_chars(GTK_LABEL(dummy_label), 25);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), dummy_label);

    privacy_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(privacy_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_UP_DOWN);
    gtk_stack_set_transition_duration(GTK_STACK(privacy_stack), 400);

    // View A - Text Announcement only
    GtkWidget *view_a = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(view_a, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(view_a, GTK_ALIGN_CENTER);
    
    pill_label_a = gtk_label_new("");
    gtk_widget_add_css_class(pill_label_a, "summary");
    gtk_box_append(GTK_BOX(view_a), pill_label_a);
    
    // View B - Permanent Indicator
    GtkWidget *view_b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(view_b, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(view_b, GTK_ALIGN_CENTER);
    
    pill_dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(pill_dot, 12, 12);
    gtk_widget_set_valign(pill_dot, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(pill_dot, "privacy-indicator");
    
    pill_label_b = gtk_label_new("");
    gtk_widget_add_css_class(pill_label_b, "summary");

    gtk_box_append(GTK_BOX(view_b), pill_dot);
    gtk_box_append(GTK_BOX(view_b), pill_label_b);

    gtk_stack_add_named(GTK_STACK(privacy_stack), view_a, "view_a");
    gtk_stack_add_named(GTK_STACK(privacy_stack), view_b, "view_b");
    
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), privacy_stack);
    
    privacy_widget_refresh_ui();

    if (!has_announced_privacy) {
        privacy_showing_view_a = TRUE;
        gtk_stack_set_visible_child_name(GTK_STACK(privacy_stack), "view_a");
        if (privacy_cycle_id > 0) g_source_remove(privacy_cycle_id);
        privacy_cycle_id = g_timeout_add_seconds(3, on_privacy_cycle_tick, NULL);
        has_announced_privacy = TRUE;
    } else {
        privacy_showing_view_a = FALSE;
        gtk_stack_set_visible_child_name(GTK_STACK(privacy_stack), "view_b");
    }

    return overlay;
}

GtkWidget* privacy_widget_create_dashboard(void) {
    if (privacy_cycle_id > 0) {
        g_source_remove(privacy_cycle_id);
        privacy_cycle_id = 0;
    }

    GtkWidget *container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    g_signal_connect(container, "destroy", G_CALLBACK(on_dash_destroyed), NULL);
    
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *title = gtk_label_new("Privacy Activity");
    gtk_widget_add_css_class(title, "dim-label");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_widget_set_hexpand(title, TRUE);
    gtk_box_append(GTK_BOX(header_box), title);

    dash_kill_all_btn = gtk_button_new_with_label("Kill All");
    gtk_widget_add_css_class(dash_kill_all_btn, "flat");
    gtk_widget_add_css_class(dash_kill_all_btn, "privacy-kill-btn");
    g_signal_connect(dash_kill_all_btn, "clicked", G_CALLBACK(on_privacy_kill_all_clicked), NULL);
    gtk_box_append(GTK_BOX(header_box), dash_kill_all_btn);
    
    gtk_box_append(GTK_BOX(container), header_box);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 220);
    
    dash_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), dash_list_box);
    gtk_box_append(GTK_BOX(container), scroll);
    
    privacy_widget_refresh_ui();

    return container;
}