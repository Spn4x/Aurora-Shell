#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>
#include <gio/gio.h>
#include "zen.h"

typedef struct {
    GtkWidget *main_button;
    GtkWidget *content_box;
    GtkWidget *glyph_label;
    GtkWidget *text_label;
    gboolean current_ui_is_dnd;
} ZenModule;

// --- Forward Declarations ---
static void update_zen_ui(ZenModule *module, gboolean is_dnd);
static void on_zen_clicked(GtkButton *button, gpointer user_data);
static gboolean on_zen_scroll(GtkEventControllerScroll* controller, double dx, double dy, gpointer user_data);
static void zen_module_cleanup(gpointer data);
static void check_zen_state(gpointer user_data);

static void update_zen_ui(ZenModule *module, gboolean is_dnd) {
    if (is_dnd) {
        gtk_label_set_text(GTK_LABEL(module->glyph_label), "󰂛");
        gtk_label_set_text(GTK_LABEL(module->text_label), " Zen");
        gtk_widget_add_css_class(module->main_button, "zen-active");
    } else {
        gtk_label_set_text(GTK_LABEL(module->glyph_label), "󰂚");
        gtk_label_set_text(GTK_LABEL(module->text_label), " Alert");
        gtk_widget_remove_css_class(module->main_button, "zen-active");
    }
    module->current_ui_is_dnd = is_dnd;
}

static void check_zen_state_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)task; (void)source_object; (void)cancellable; (void)task_data;
    FILE *fp = popen("swaync-client --get-dnd", "r");
    if (!fp) { g_task_return_pointer(task, GINT_TO_POINTER(-1), NULL); return; }
    char result[16]; gboolean is_dnd = FALSE;
    if (fgets(result, sizeof(result), fp) != NULL) { g_strstrip(result); is_dnd = (g_strcmp0(result, "true") == 0); }
    pclose(fp);
    g_task_return_pointer(task, GINT_TO_POINTER(is_dnd), NULL);
}

static void on_check_state_finished(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object;
    ZenModule *module = (ZenModule*)user_data;
    gintptr actual_state_ptr = (gintptr)g_task_propagate_pointer(G_TASK(res), NULL);
    if (actual_state_ptr == -1) return;
    gboolean actual_is_dnd = (gboolean)actual_state_ptr;
    if (module->current_ui_is_dnd != actual_is_dnd) {
        g_print("Zen module out of sync! Correcting UI.\n");
        update_zen_ui(module, actual_is_dnd);
    }
}

static void check_zen_state(gpointer user_data) {
    ZenModule *module = (ZenModule*)user_data;
    g_autoptr(GTask) task = g_task_new(module->main_button, NULL, on_check_state_finished, module);
    g_task_run_in_thread(task, check_zen_state_thread);
}

static void on_zen_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    ZenModule *module = (ZenModule*)user_data;
    update_zen_ui(module, !module->current_ui_is_dnd);
    system("swaync-client --toggle-dnd &");
    g_timeout_add_once(250, (GSourceOnceFunc)check_zen_state, module);
}

static gboolean on_zen_scroll(GtkEventControllerScroll* controller, double dx, double dy, gpointer user_data) {
    (void)controller; (void)dx;
    ZenModule *module = (ZenModule*)user_data;
    if (dy < 0) {
        update_zen_ui(module, !module->current_ui_is_dnd);
        system("swaync-client --toggle-dnd &");
        g_timeout_add_once(250, (GSourceOnceFunc)check_zen_state, module);
    } else if (dy > 0) {
        system("swaync-client --toggle-panel &");
    }
    return G_SOURCE_CONTINUE;
}

static void zen_module_cleanup(gpointer data) {
    g_free((ZenModule *)data);
}

GtkWidget* create_zen_module() {
    ZenModule *module = g_new0(ZenModule, 1);
    module->main_button = gtk_button_new();
    gtk_widget_add_css_class(module->main_button, "zen-module");
    gtk_widget_add_css_class(module->main_button, "module");
    gtk_widget_add_css_class(module->main_button, "flat");
    gtk_widget_add_css_class(module->main_button, "group-start");
    module->content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_button_set_child(GTK_BUTTON(module->main_button), module->content_box);
    module->glyph_label = gtk_label_new("?");
    gtk_widget_add_css_class(module->glyph_label, "glyph-label");
    module->text_label = gtk_label_new("...");
    gtk_box_append(GTK_BOX(module->content_box), module->glyph_label);
    gtk_box_append(GTK_BOX(module->content_box), module->text_label);
    g_signal_connect(module->main_button, "clicked", G_CALLBACK(on_zen_clicked), module);
    
    // =======================================================================
    // THE FIX: Corrected the typo from two underscores to one.
    GtkEventController *scroll_controller = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    // =======================================================================

    g_signal_connect(scroll_controller, "scroll", G_CALLBACK(on_zen_scroll), module);
    gtk_widget_add_controller(module->main_button, scroll_controller);
    check_zen_state(module);
    g_object_set_data_full(G_OBJECT(module->main_button), "module-state", module, zen_module_cleanup);
    return module->main_button;
}