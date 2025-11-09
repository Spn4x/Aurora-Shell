#include <gtk/gtk.h>
#include <glib-unix.h>

#include "wifi_scanner.h"
#include "bluetooth_scanner.h"
#include "network_manager.h"
#include "bluetooth_manager.h"
#include "audio_manager.h"
#include "brightness_manager.h"
#include "system_monitor.h"

const guint WIFI_SCAN_INTERVAL_SECONDS = 10;
const guint BT_SCAN_INTERVAL_SECONDS = 15;
const int LIST_REQUESTED_HEIGHT = 155;

// --- Main State Struct ---
typedef struct {
    GtkRevealer *stack_revealer;
    GtkStack *main_stack;
    GtkWidget *wifi_toggle, *bt_toggle, *audio_toggle;
    gulong wifi_toggle_handler_id, bt_toggle_handler_id, audio_toggle_handler_id;
    WifiScanner *wifi_scanner;
    BluetoothScanner *bt_scanner;
    SystemMonitor *system_monitor;
    
    // --- ADD THESE TWO LINES BACK ---
    GtkWidget *wifi_list_box, *wifi_list_overlay, *wifi_list_spinner;
    
    GtkWidget *bt_list_box, *bt_list_overlay, *bt_list_spinner;
    GtkWidget *audio_list_box;
    GtkWidget *system_volume_slider;
    gulong system_volume_handler_id;
    GtkWidget *brightness_slider;
    gulong brightness_slider_handler_id;
    gboolean airplane_mode_active;
    gboolean wifi_was_on_before_airplane;
    gboolean bt_was_on_before_airplane;
    
} AppWidgets;

// --- Password Prompt Structs ---
typedef void (*PasswordCancelCallback)(gpointer user_data);
typedef void (*PasswordReadyCallback)(const gchar *password, gpointer user_data);
typedef struct {
    GtkWindow *window;
    GtkPasswordEntry *password_entry;
    PasswordReadyCallback ready_callback;
    PasswordCancelCallback cancel_callback;
    gpointer user_data;
} PasswordDialogState;
typedef struct {
    AppWidgets *widgets;
    WifiNetwork *network;
    GtkWidget *connecting_widget;
} PasswordDialogContext;


// --- Forward Declarations ---
static void on_wifi_network_clicked(GtkButton *button, gpointer user_data);
static void on_wifi_operation_finished(gboolean success, gpointer user_data);
static void on_password_ready(const gchar *password, gpointer user_data);
static void on_password_cancelled(gpointer user_data);
static void show_password_prompt(GtkWindow *parent_window, const gchar *ssid, PasswordReadyCallback ready_callback, PasswordCancelCallback cancel_callback, gpointer user_data);
static const char* get_wifi_icon_name_for_signal(int strength);
static WifiNetwork* get_wifi_network_from_widget(GtkWidget *list_item);
static void update_wifi_widget_state(GtkWidget *button, WifiNetwork *net);
static void on_wifi_right_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
static void on_popover_closed(GtkPopover *popover, gpointer user_data);
static GtkWidget* create_popover_action_button(const char* icon_name, const char* label);
static void on_details_button_clicked(GtkButton *button, GtkPopover *parent_popover);
static void on_connect_button_clicked(GtkButton *button, GtkPopover *popover);
static void on_disconnect_button_clicked(GtkButton *button, GtkPopover *popover);
static void on_forget_button_clicked(GtkButton *button, GtkPopover *popover);
static GtkWidget* create_list_entry(const char* icon, const char* label_text, gboolean is_active);
static void on_audio_sink_clicked(GtkButton *button, AudioSink *sink);
static void toggle_airplane_mode(GtkToggleButton *button, AppWidgets *widgets);
static void update_audio_device_list(AppWidgets *widgets);
static AppWidgets* get_widgets_from_child(GtkWidget *child_widget);
static void on_expandable_toggle_toggled(GtkToggleButton *toggled_button, AppWidgets *widgets);
static void on_bt_disconnect_button_clicked(GtkButton *button, GtkPopover *popover);
static void on_bluetooth_right_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
static void on_device_state_changed(GDBusConnection *connection, const gchar *sender_name, const gchar *object_path, const gchar *interface_name, const gchar *signal_name, GVariant *parameters, gpointer user_data);
static void subscribe_to_wifi_device_signals(AppWidgets *widgets);
static void on_wifi_scan_results(GList *networks, gpointer user_data);
static gboolean initial_state_update(gpointer user_data);

typedef struct {
    GtkPopover *parent_popover;
    GtkWidget *parent_widget;
} DetailsCallbackContext;

// NEW HANDLER: This is called when the details are ready
static void on_details_received(WifiNetworkDetails *details, gpointer user_data) {
    DetailsCallbackContext *context = user_data;
    if (!details) {
        g_warning("Failed to receive network details.");
        g_free(context);
        return;
    }

    // This is the UI-building logic moved from the old click handler
    GtkPopover *details_popover = GTK_POPOVER(gtk_popover_new());
    g_signal_connect(details_popover, "closed", G_CALLBACK(on_popover_closed), NULL);

    GtkWidget *grid = gtk_grid_new();
    gtk_widget_add_css_class(grid, "details-grid");
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Signal:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(g_strdup_printf("%d%%", details->strength)), 1, 0, 1, 1);
    
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Security:"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(details->security), 1, 1, 1, 1);
    
    if (details->ip_address) {
        gtk_grid_attach(GTK_GRID(grid), gtk_label_new("IP Address:"), 0, 2, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), gtk_label_new(details->ip_address), 1, 2, 1, 1);
    }
    if (details->mac_address) {
        gtk_grid_attach(GTK_GRID(grid), gtk_label_new("MAC Address:"), 0, 3, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), gtk_label_new(details->mac_address), 1, 3, 1, 1);
    }

    gtk_popover_set_child(details_popover, grid);
    gtk_widget_set_parent(GTK_WIDGET(details_popover), context->parent_widget);
    gtk_popover_popup(details_popover);
    
    free_wifi_network_details(details);
    g_free(context);
}

// --- Cleanup & Helpers ---
static void control_center_cleanup(gpointer data) {
    AppWidgets *widgets = (AppWidgets *)data;
    if (!widgets) return;
    //if (widgets->dbus_connection && widgets->device_state_changed_handler_id > 0) {
        //g_dbus_connection_signal_unsubscribe(widgets->dbus_connection, //widgets->device_state_changed_handler_id);
    //}
    //g_clear_object(&widgets->dbus_connection);
    wifi_scanner_stop(widgets->wifi_scanner);
    bluetooth_scanner_stop(widgets->bt_scanner);
    wifi_scanner_free(widgets->wifi_scanner);
    bluetooth_scanner_free(widgets->bt_scanner);
    system_monitor_free(widgets->system_monitor);
    g_free(widgets);
}

static void on_popover_closed(GtkPopover *popover, gpointer user_data) {
    (void)user_data;
    if (gtk_widget_get_parent(GTK_WIDGET(popover))) {
        gtk_widget_unparent(GTK_WIDGET(popover));
    }
}

static AppWidgets* get_widgets_from_child(GtkWidget *child_widget) {
    GtkWidget *ancestor = child_widget;
    while (ancestor) {
        const char* name = gtk_widget_get_name(ancestor);
        if (name && g_strcmp0(name, "aurora-control-center") == 0) {
            return g_object_get_data(G_OBJECT(ancestor), "app-widgets");
        }
        ancestor = gtk_widget_get_parent(ancestor);
    }
    return NULL;
}

// --- Password Prompt UI & Logic ---
static void on_password_cancelled(gpointer user_data) {
    PasswordDialogContext *context = user_data;
    if (!context) return;
    g_print("[UI] Password prompt cancelled. Reverting spinner.\n");
    if (context->connecting_widget) {
        GtkWidget* box = gtk_button_get_child(GTK_BUTTON(context->connecting_widget));
        GtkWidget* icon_stack = gtk_widget_get_first_child(box);
        if (icon_stack && GTK_IS_STACK(icon_stack)) {
            gtk_stack_set_visible_child_name(GTK_STACK(icon_stack), "icon");
        }
    }
    g_free(context);
}

static void on_password_dialog_connect_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    PasswordDialogState *dialog_state = user_data;
    const gchar *password = gtk_editable_get_text(GTK_EDITABLE(dialog_state->password_entry));
    if (dialog_state->cancel_callback) {
        g_signal_handlers_disconnect_by_func(dialog_state->window, G_CALLBACK(dialog_state->cancel_callback), dialog_state->user_data);
    }
    if (dialog_state->ready_callback) {
        dialog_state->ready_callback(password, dialog_state->user_data);
    }
    gtk_window_destroy(dialog_state->window);
}

static void show_password_prompt(GtkWindow *parent_window, const gchar *ssid, PasswordReadyCallback ready_callback, PasswordCancelCallback cancel_callback, gpointer user_data) {
    GtkWindow *window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_transient_for(window, parent_window);
    gtk_window_set_modal(window, TRUE);
    gtk_window_set_title(window, "Connect to Wi-Fi Network");
    gtk_window_set_default_size(window, 350, -1);
    gtk_window_set_resizable(window, FALSE);
    PasswordDialogState *dialog_state = g_new0(PasswordDialogState, 1);
    dialog_state->window = window;
    dialog_state->ready_callback = ready_callback;
    dialog_state->cancel_callback = cancel_callback;
    dialog_state->user_data = user_data;
    g_object_set_data_full(G_OBJECT(window), "dialog-state", dialog_state, g_free);
    g_signal_connect_swapped(window, "destroy", G_CALLBACK(cancel_callback), user_data);
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(main_box, 15);
    gtk_widget_set_margin_end(main_box, 15);
    gtk_widget_set_margin_top(main_box, 15);
    gtk_widget_set_margin_bottom(main_box, 15);
    gtk_window_set_child(window, main_box);
    GtkWidget *header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(header), "<span weight='bold'>Password required</span>");
    gtk_label_set_xalign(GTK_LABEL(header), 0.0);
    GtkWidget *ssid_label = gtk_label_new(ssid);
    gtk_label_set_xalign(GTK_LABEL(ssid_label), 0.0);
    dialog_state->password_entry = GTK_PASSWORD_ENTRY(gtk_password_entry_new());
    gtk_widget_grab_focus(GTK_WIDGET(dialog_state->password_entry));
    GtkWidget *action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(action_box, GTK_ALIGN_END);
    GtkWidget *cancel_button = gtk_button_new_with_label("Cancel");
    GtkWidget *connect_button = gtk_button_new_with_label("Connect");
    gtk_widget_add_css_class(connect_button, "suggested-action");
    g_signal_connect_swapped(cancel_button, "clicked", G_CALLBACK(gtk_window_destroy), window);
    g_signal_connect(connect_button, "clicked", G_CALLBACK(on_password_dialog_connect_clicked), dialog_state);
    gtk_window_set_default_widget(window, connect_button);
    gtk_box_append(GTK_BOX(action_box), cancel_button);
    gtk_box_append(GTK_BOX(action_box), connect_button);
    gtk_box_append(GTK_BOX(main_box), header);
    gtk_box_append(GTK_BOX(main_box), ssid_label);
    gtk_box_append(GTK_BOX(main_box), GTK_WIDGET(dialog_state->password_entry));
    gtk_box_append(GTK_BOX(main_box), action_box);
    gtk_window_present(window);
}





// ADD this small helper function near the top of main.c, with the other forward declarations.
static gboolean rescan_after_delay(gpointer user_data) {
    AppWidgets *widgets = user_data;
    if (widgets && widgets->wifi_scanner) {
        wifi_scanner_trigger_scan(widgets->wifi_scanner);
    }
    return G_SOURCE_REMOVE; // Run only once
}


static void on_wifi_operation_finished(gboolean success, gpointer user_data) {
    (void)success;
    AppWidgets *widgets = user_data;
    if (!widgets) {
        return;
    }

    // Stop the spinner and re-enable the UI immediately
    gtk_spinner_stop(GTK_SPINNER(widgets->wifi_list_spinner));
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->wifi_list_overlay), TRUE);
    
    // Schedule a single, reliable scan to happen after a short delay.
    // This gives NetworkManager time to update its state before we query it.
    g_timeout_add(500, rescan_after_delay, widgets);
}

static void on_bt_operation_finished(gboolean success, gpointer user_data) {
    (void)success;
    AppWidgets *widgets = user_data;
    if(!widgets) return;
    gtk_spinner_stop(GTK_SPINNER(widgets->bt_list_spinner));
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->bt_list_overlay), TRUE);
    bluetooth_scanner_trigger_scan(widgets->bt_scanner);
}

static void on_sink_set_finished(gboolean success, gpointer user_data) {
    if (success) update_audio_device_list(user_data);
}

static void on_password_ready(const gchar *password, gpointer user_data) {
    PasswordDialogContext *context = user_data;
    AppWidgets *widgets = context->widgets;
    WifiNetwork *net = context->network;
    // Pass our new, smarter callback
    add_and_activate_wifi_connection_async(net->ssid, net->object_path, password, net->is_secure, on_wifi_operation_finished, widgets);
    // The context is freed by the cancel handler on the dialog destroy signal
}

// NEW HELPER: Creates a styled button for our new popover menu.
static GtkWidget* create_popover_action_button(const char* icon_name, const char* label) {
    GtkWidget *button = gtk_button_new();
    gtk_widget_add_css_class(button, "popover-action-button");
    
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_button_set_child(GTK_BUTTON(button), box);
    
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_widget_add_css_class(icon, "dim-label");
    
    GtkWidget *label_widget = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(label_widget), 0.0);
    
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label_widget);
    
    return button;
}

// NEW HANDLER: This is called when the "Details" button is clicked.
static void on_details_button_clicked(GtkButton *button, GtkPopover *parent_popover) {
    // Close the first menu immediately
    gtk_popover_popdown(parent_popover);
    
    // Retrieve the data we attached earlier
    WifiNetwork *net = g_object_get_data(G_OBJECT(button), "wifi-network-data");
    GtkWidget *parent_widget = g_object_get_data(G_OBJECT(button), "parent-widget");
    if (!net || !parent_widget) return;

    // Create a context to pass to our callback
    DetailsCallbackContext *context = g_new0(DetailsCallbackContext, 1);
    context->parent_popover = parent_popover;
    context->parent_widget = parent_widget;

    // Start the asynchronous operation. The UI will be updated in on_details_received.
    get_wifi_network_details_async(net->object_path, on_details_received, context);
}

// --- Click Handlers (Left-Click) ---
static void on_wifi_network_clicked(GtkButton *button, gpointer user_data) {
    // The user_data is now unused, we get the data from the button.
    (void)user_data;
    WifiNetwork *net = get_wifi_network_from_widget(GTK_WIDGET(button));
    AppWidgets *widgets = get_widgets_from_child(GTK_WIDGET(button));
    if (!widgets || !net || net->is_active) return; // Added !net check

    GtkWidget* box = gtk_button_get_child(GTK_BUTTON(button));
    GtkWidget* icon_stack = gtk_widget_get_first_child(box);
    // ... the rest of the function is identical to your existing code ...
    if (icon_stack && GTK_IS_STACK(icon_stack)) {
        GtkWidget* spinner = gtk_stack_get_child_by_name(GTK_STACK(icon_stack), "spinner");
        if(spinner) gtk_spinner_start(GTK_SPINNER(spinner));
        gtk_stack_set_visible_child_name(GTK_STACK(icon_stack), "spinner");
    }
    
    g_autofree gchar *existing_connection_path = find_connection_for_ssid(net->ssid);
    if (existing_connection_path) {
        activate_wifi_connection_async(existing_connection_path, net->object_path, on_wifi_operation_finished, widgets);
    } else {
        if (net->is_secure) {
            PasswordDialogContext *context = g_new0(PasswordDialogContext, 1);
            context->widgets = widgets;
            context->network = net;
            context->connecting_widget = GTK_WIDGET(button);
            GtkWindow *parent_window = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button)));
            show_password_prompt(parent_window, net->ssid, on_password_ready, on_password_cancelled, context);
        } else {
            add_and_activate_wifi_connection_async(net->ssid, net->object_path, NULL, net->is_secure, on_wifi_operation_finished, widgets);
        }
    }
}

static void on_bluetooth_device_clicked(GtkButton *button, gpointer user_data) {
    BluetoothDevice *dev = (BluetoothDevice*)user_data;
    AppWidgets *widgets = get_widgets_from_child(GTK_WIDGET(button));
    if (!widgets || dev->is_connected) {
        return;
    }

    gtk_widget_set_sensitive(GTK_WIDGET(widgets->bt_list_overlay), FALSE);
    gtk_spinner_start(GTK_SPINNER(widgets->bt_list_spinner));
    connect_to_bluetooth_device_async(dev->address, on_bt_operation_finished, widgets);
}

static void on_audio_sink_clicked(GtkButton *button, AudioSink *sink) {
    AppWidgets *widgets = get_widgets_from_child(GTK_WIDGET(button));
    if(!widgets) {
        return;
    }
    set_default_sink_async(sink->id, on_sink_set_finished, widgets);
}

static void toggle_airplane_mode(GtkToggleButton *button, AppWidgets *widgets) {
    gboolean is_active = gtk_toggle_button_get_active(button);

    if (is_active) {
        // --- Turning Airplane Mode ON ---
        widgets->airplane_mode_active = TRUE;
        
        // Store the current state of Wi-Fi and Bluetooth
        widgets->wifi_was_on_before_airplane = is_wifi_enabled();
        widgets->bt_was_on_before_airplane = is_bluetooth_powered();

        // Turn them off if they are currently on
        if (widgets->wifi_was_on_before_airplane) {
            set_wifi_enabled_async(FALSE, NULL, NULL);
        }
        if (widgets->bt_was_on_before_airplane) {
            set_bluetooth_powered_async(FALSE, NULL, NULL);
        }
        
    } else {
        // --- Turning Airplane Mode OFF ---
        widgets->airplane_mode_active = FALSE;
        
        // Restore their previous states
        if (widgets->wifi_was_on_before_airplane) {
            set_wifi_enabled_async(TRUE, NULL, NULL);
        }
        if (widgets->bt_was_on_before_airplane) {
            set_bluetooth_powered_async(TRUE, NULL, NULL);
        }
    }
}

// --- Click Handlers (Right-Click Popover Actions) ---
static void on_forget_button_clicked(GtkButton *button, GtkPopover *popover) {
    gtk_popover_popdown(popover);
    AppWidgets *widgets = get_widgets_from_child(GTK_WIDGET(button));
    if (!widgets) return;

    const char *ssid = g_object_get_data(G_OBJECT(button), "ssid-to-forget");
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->wifi_list_overlay), FALSE);
    gtk_spinner_start(GTK_SPINNER(widgets->wifi_list_spinner));
    forget_wifi_connection_async(ssid, on_wifi_operation_finished, widgets);
}

static void on_disconnect_button_clicked(GtkButton *button, GtkPopover *popover) {
    gtk_popover_popdown(popover);
    AppWidgets *widgets = get_widgets_from_child(GTK_WIDGET(button));
    if (!widgets) return;

    gtk_widget_set_sensitive(GTK_WIDGET(widgets->wifi_list_overlay), FALSE);
    gtk_spinner_start(GTK_SPINNER(widgets->wifi_list_spinner));
    disconnect_wifi_async(on_wifi_operation_finished, widgets);
}

static void on_bt_disconnect_button_clicked(GtkButton *button, GtkPopover *popover) {
    gtk_popover_popdown(popover);
    AppWidgets *widgets = get_widgets_from_child(GTK_WIDGET(button));
    if (!widgets) return;

    const char *address = g_object_get_data(G_OBJECT(button), "address-to-disconnect");
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->bt_list_overlay), FALSE);
    gtk_spinner_start(GTK_SPINNER(widgets->bt_list_spinner));
    disconnect_bluetooth_device_async(address, on_bt_operation_finished, widgets);
}

// NEW HANDLER for the "Connect" button in the right-click menu
static void on_connect_button_clicked(GtkButton *button, GtkPopover *popover) {
    gtk_popover_popdown(popover); // Close the menu first

    // Retrieve the data we attached to the button
    WifiNetwork *net = g_object_get_data(G_OBJECT(button), "wifi-network-data");
    AppWidgets *widgets = g_object_get_data(G_OBJECT(button), "app-widgets-data");

    if (!widgets || !net || net->is_active) {
        return;
    }

    // This is the exact same logic from on_wifi_network_clicked()
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->wifi_list_overlay), FALSE);
    gtk_spinner_start(GTK_SPINNER(widgets->wifi_list_spinner));

    g_autofree gchar *existing_connection_path = find_connection_for_ssid(net->ssid);
    if (existing_connection_path) {
        activate_wifi_connection_async(existing_connection_path, net->object_path, on_wifi_operation_finished, widgets);
    } else {
        // We'll need to add a password prompt here in the future, but for now, it will
        // attempt to connect to open networks or use a previously known password.
        add_and_activate_wifi_connection_async(net->ssid, net->object_path, NULL, net->is_secure, on_wifi_operation_finished, widgets);
    }
}


// HELPER 1: Correctly gets the WifiNetwork data back from a list item's controller.
static WifiNetwork* get_wifi_network_from_widget(GtkWidget *list_item) {
    return g_object_get_data(G_OBJECT(list_item), "wifi-network-data");
}

// HELPER 2: Updates the UI of an existing row widget without destroying it.
static void update_wifi_widget_state(GtkWidget *button, WifiNetwork *net) {
    GtkWidget *box = gtk_button_get_child(GTK_BUTTON(button));
    if (!box) return;

    // --- Update the Icon ---
    GtkWidget *icon_stack = gtk_widget_get_first_child(box);
    if (icon_stack && GTK_IS_STACK(icon_stack)) {
        GtkWidget *icon = gtk_stack_get_child_by_name(GTK_STACK(icon_stack), "icon");
        if (icon && GTK_IS_IMAGE(icon)) {
            const char *icon_name = get_wifi_icon_name_for_signal(net->strength);
            gtk_image_set_from_icon_name(GTK_IMAGE(icon), icon_name);
        }
        // Ensure spinner is hidden
        gtk_stack_set_visible_child_name(GTK_STACK(icon_stack), "icon");
    }

    // --- Update the Active Checkmark ---
    gboolean has_check = FALSE;
    GtkWidget *child = gtk_widget_get_first_child(box);
    while(child) {
        if (gtk_widget_get_name(child) && g_strcmp0(gtk_widget_get_name(child), "active-check") == 0) {
            has_check = TRUE;
            break;
        }
        child = gtk_widget_get_next_sibling(child);
    }

    if (net->is_active && !has_check) {
        GtkWidget *check_icon = gtk_image_new_from_icon_name("object-select-symbolic");
        gtk_widget_add_css_class(check_icon, "active-symbol");
        gtk_widget_set_name(check_icon, "active-check");
        gtk_box_append(GTK_BOX(box), check_icon);
        gtk_widget_add_css_class(button, "active-network");
    } else if (!net->is_active && has_check) {
        // Find and remove the checkmark specifically
        GtkWidget *to_remove = NULL;
        child = gtk_widget_get_first_child(box);
         while(child) {
            if (gtk_widget_get_name(child) && g_strcmp0(gtk_widget_get_name(child), "active-check") == 0) {
                to_remove = child;
                break;
            }
            child = gtk_widget_get_next_sibling(child);
        }
        if (to_remove) {
            gtk_box_remove(GTK_BOX(box), to_remove);
        }
        gtk_widget_remove_css_class(button, "active-network");
    }
}

// --- Gesture Handlers (Right-Click) ---
static void on_wifi_right_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; (void)x; (void)y; (void)user_data; // user_data is now unused
    GtkWidget *button_widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    WifiNetwork *net = get_wifi_network_from_widget(button_widget);
    AppWidgets *widgets = get_widgets_from_child(button_widget);
    if (!net) return; // Added !net check
    
    GtkPopover *popover = GTK_POPOVER(gtk_popover_new());
    // ... the rest of the function is identical to your existing code ...
    g_signal_connect(popover, "closed", G_CALLBACK(on_popover_closed), NULL);

    GtkWidget *menu_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_popover_set_child(popover, menu_box);

    GtkWidget *details_button = create_popover_action_button("dialog-information-symbolic", "Details");
    g_object_set_data(G_OBJECT(details_button), "wifi-network-data", net);
    g_object_set_data(G_OBJECT(details_button), "parent-widget", button_widget);
    g_signal_connect(details_button, "clicked", G_CALLBACK(on_details_button_clicked), popover);
    gtk_box_append(GTK_BOX(menu_box), details_button);

    gtk_box_append(GTK_BOX(menu_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    if (net->is_active) {
        GtkWidget *disconnect_button = create_popover_action_button("network-wired-disconnected-symbolic", "Disconnect");
        g_signal_connect(disconnect_button, "clicked", G_CALLBACK(on_disconnect_button_clicked), popover);
        gtk_box_append(GTK_BOX(menu_box), disconnect_button);
    } else {
        GtkWidget *connect_button = create_popover_action_button("network-wired-symbolic", "Connect");
        g_object_set_data(G_OBJECT(connect_button), "wifi-network-data", net);
        g_object_set_data(G_OBJECT(connect_button), "app-widgets-data", widgets);
        g_signal_connect(connect_button, "clicked", G_CALLBACK(on_connect_button_clicked), popover);
        gtk_box_append(GTK_BOX(menu_box), connect_button);

        g_autofree gchar *existing_connection_path = find_connection_for_ssid(net->ssid);
        if (existing_connection_path && is_connection_forgettable(existing_connection_path, net->is_secure)) {
            GtkWidget *forget_button = create_popover_action_button("edit-delete-symbolic", "Forget");
            g_object_set_data(G_OBJECT(forget_button), "ssid-to-forget", (gpointer)net->ssid);
            g_signal_connect(forget_button, "clicked", G_CALLBACK(on_forget_button_clicked), popover);
            gtk_box_append(GTK_BOX(menu_box), forget_button);
        }
    }
    
    gtk_widget_set_parent(GTK_WIDGET(popover), button_widget);
    gtk_popover_popup(popover);
}

static void on_bluetooth_right_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; (void)x; (void)y;
    BluetoothDevice *dev = user_data;
    if (!dev->is_connected) {
        return;
    }

    GtkWidget *button_widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    
    GtkPopover *popover = GTK_POPOVER(gtk_popover_new());
    // CRITICAL FIX: Connect the cleanup signal to prevent memory leaks.
    g_signal_connect(popover, "closed", G_CALLBACK(on_popover_closed), NULL);

    // For now, we only have one action. We use the same popover structure for consistency.
    GtkWidget *menu_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_popover_set_child(popover, menu_box);

    GtkWidget *disconnect_button = create_popover_action_button("network-wired-disconnected-symbolic", "Disconnect");
    g_object_set_data(G_OBJECT(disconnect_button), "address-to-disconnect", (gpointer)dev->address);
    g_signal_connect(disconnect_button, "clicked", G_CALLBACK(on_bt_disconnect_button_clicked), popover);
    gtk_box_append(GTK_BOX(menu_box), disconnect_button);

    gtk_widget_set_parent(GTK_WIDGET(popover), button_widget);
    gtk_popover_popup(popover);
}

// --- UI Update & Change Handlers ---
static void on_system_volume_changed(GtkRange *range, gpointer user_data) {
    (void)user_data;
    gint value = (gint)gtk_range_get_value(range);
    set_default_sink_volume_async(value, NULL, NULL);
}

static void on_brightness_changed(GtkRange *range, gpointer user_data) {
    (void)user_data;
    gint value = (gint)gtk_range_get_value(range);
    set_brightness_async(value);
}

static void on_system_event(SystemEventType type, gpointer user_data) {
    AppWidgets *widgets = user_data;
    if(!widgets) {
        return;
    }

    switch (type) {
        case SYSTEM_EVENT_VOLUME_CHANGED: {
            AudioSinkState *state = get_default_sink_state();
            if (state) {
                g_signal_handler_block(widgets->system_volume_slider, widgets->system_volume_handler_id);
                gtk_range_set_value(GTK_RANGE(widgets->system_volume_slider), state->volume);
                g_signal_handler_unblock(widgets->system_volume_slider, widgets->system_volume_handler_id);
                g_free(state);
            }
            break;
        }
        case SYSTEM_EVENT_BRIGHTNESS_CHANGED: {
            gint brightness = get_current_brightness();
            if (brightness >= 0) {
                g_signal_handler_block(widgets->brightness_slider, widgets->brightness_slider_handler_id);
                gtk_range_set_value(GTK_RANGE(widgets->brightness_slider), brightness);
                g_signal_handler_unblock(widgets->brightness_slider, widgets->brightness_slider_handler_id);
            }
            break;
        }
    }
}

// --- UI Construction Helpers ---
static GtkWidget* create_list_entry(const char* icon, const char* label_text, gboolean is_active) {
    GtkWidget *button = gtk_button_new();
    gtk_widget_add_css_class(button, "list-item-button");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_button_set_child(GTK_BUTTON(button), box);

    gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(icon));

    GtkWidget *label = gtk_label_new(label_text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), label);

    // --- MODIFICATION IS HERE ---
    if (is_active) {
        // Replace the GtkLabel with a GtkImage using a standard checkmark icon.
        GtkWidget *check_icon = gtk_image_new_from_icon_name("object-select-symbolic");
        gtk_widget_add_css_class(check_icon, "active-symbol");
        gtk_box_append(GTK_BOX(box), check_icon);
    }
    // --- END MODIFICATION ---

    return button;
}

static const char* get_wifi_icon_name_for_signal(int strength) {
    if (strength > 80) return "network-wireless-signal-excellent-symbolic";
    if (strength > 55) return "network-wireless-signal-good-symbolic";
    if (strength > 30) return "network-wireless-signal-ok-symbolic";
    if (strength > 5)  return "network-wireless-signal-weak-symbolic";
    return "network-wireless-signal-none-symbolic";
}

// --- List Update Functions ---
static void update_audio_device_list(AppWidgets *widgets) {
    if (!widgets) return;
    GtkWidget *list_box = widgets->audio_list_box;
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(list_box))) {
        gtk_box_remove(GTK_BOX(list_box), child);
    }
    GList *sinks = get_audio_sinks();
    if (g_list_length(sinks) == 0) {
        gtk_box_append(GTK_BOX(list_box), gtk_label_new("No audio devices found."));
    } else {
        for (GList *l = sinks; l != NULL; l = l->next) {
            AudioSink *sink_from_scan = l->data;
            AudioSink *sink_copy = g_new0(AudioSink, 1);
            *sink_copy = *sink_from_scan;
            sink_copy->name = g_strdup(sink_from_scan->name);

            GtkWidget *entry_button = create_list_entry("audio-card-symbolic", sink_copy->name, sink_copy->is_default);
            g_signal_connect(entry_button, "clicked", G_CALLBACK(on_audio_sink_clicked), sink_copy);
            g_signal_connect_swapped(entry_button, "destroy", G_CALLBACK(audio_sink_free), sink_copy);
            gtk_box_append(GTK_BOX(list_box), entry_button);
        }
    }
    free_audio_sink_list(sinks);
}

static void on_wifi_scan_results(GList *networks, gpointer user_data) {
    AppWidgets *widgets = (AppWidgets*)user_data;
    if(!widgets) { 
        free_wifi_network_list(networks); 
        return; 
    }

    GtkWidget *list_box = widgets->wifi_list_box;

    // Handle case where Wi-Fi is disabled
    if (!is_wifi_enabled()) {
        GtkWidget *child;
        while ((child = gtk_widget_get_first_child(list_box))) { 
            gtk_box_remove(GTK_BOX(list_box), child); 
        }
        GtkWidget *label = gtk_label_new("Wi-Fi is turned off");
        gtk_widget_set_vexpand(label, TRUE);
        gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(list_box), label);
        free_wifi_network_list(networks);
        return;
    }

    // --- Smart Refresh / Reconciliation Logic ---
    GHashTable *new_ssids = g_hash_table_new(g_str_hash, g_str_equal);
    
    // 1. First pass: Update existing widgets and identify new ones.
    for (GList *l = networks; l != NULL; l = l->next) {
        WifiNetwork *net_from_scan = l->data;
        g_hash_table_insert(new_ssids, net_from_scan->ssid, net_from_scan); // Keep track of all SSIDs in the new scan
        
        gboolean found_and_updated = FALSE;
        GtkWidget *child = gtk_widget_get_first_child(list_box);
        while (child) {
            WifiNetwork *old_net_data = get_wifi_network_from_widget(child);
            if (old_net_data && g_strcmp0(old_net_data->ssid, net_from_scan->ssid) == 0) {
                // Found a match! Update it in place.
                // Note: We don't need to g_free anything here, just update the data.
                old_net_data->strength = net_from_scan->strength;
                old_net_data->is_active = net_from_scan->is_active;
                // Update the UI state of the existing widget
                update_wifi_widget_state(child, old_net_data);
                found_and_updated = TRUE;
                break;
            }
            child = gtk_widget_get_next_sibling(child);
        }

        if (!found_and_updated) {
            // This is a new network, create a new widget for it.
            WifiNetwork *net_copy = g_new0(WifiNetwork, 1);
            net_copy->ssid = g_strdup(net_from_scan->ssid);
            net_copy->object_path = g_strdup(net_from_scan->object_path);
            net_copy->strength = net_from_scan->strength;
            net_copy->is_secure = net_from_scan->is_secure;
            net_copy->is_active = net_from_scan->is_active;

            GtkWidget *entry_button = gtk_button_new();
            
            // --- THE CORE FIX ---
            // Attach the data directly to the button. This also manages its memory.
            g_object_set_data_full(G_OBJECT(entry_button), "wifi-network-data", net_copy, (GDestroyNotify)wifi_network_free);
            
            gtk_widget_add_css_class(entry_button, "list-item-button");
            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_button_set_child(GTK_BUTTON(entry_button), box);
            
            GtkWidget *icon_stack = gtk_stack_new();
            gtk_widget_set_name(icon_stack, "icon-stack");
            const char *icon_name = get_wifi_icon_name_for_signal(net_copy->strength);
            GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
            GtkWidget *spinner = gtk_spinner_new();
            gtk_stack_add_named(GTK_STACK(icon_stack), icon, "icon");
            gtk_stack_add_named(GTK_STACK(icon_stack), spinner, "spinner");
            gtk_stack_set_visible_child_name(GTK_STACK(icon_stack), "icon");
            gtk_box_append(GTK_BOX(box), icon_stack);
            
            GtkWidget *label = gtk_label_new(net_copy->ssid);
            gtk_widget_set_halign(label, GTK_ALIGN_START);
            gtk_widget_set_hexpand(label, TRUE);
            gtk_box_append(GTK_BOX(box), label);

            if (net_copy->is_secure) {
                gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name("network-wireless-encrypted-symbolic"));
            }
            if (net_copy->is_active) {
                GtkWidget *check_icon = gtk_image_new_from_icon_name("object-select-symbolic");
                gtk_widget_add_css_class(check_icon, "active-symbol");
                gtk_widget_set_name(check_icon, "active-check");
                gtk_box_append(GTK_BOX(box), check_icon);
                gtk_widget_add_css_class(entry_button, "active-network");
            }
            
            // Connect signals. The data is now ignored in the "clicked" handler's user_data.
            g_signal_connect(entry_button, "clicked", G_CALLBACK(on_wifi_network_clicked), NULL);
            
            GtkGesture *right_click = gtk_gesture_click_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
            // The data is now ignored in the "pressed" handler's user_data as well.
            g_signal_connect(right_click, "pressed", G_CALLBACK(on_wifi_right_click), NULL);
            gtk_widget_add_controller(entry_button, GTK_EVENT_CONTROLLER(right_click));

            gtk_box_append(GTK_BOX(list_box), entry_button);
        }
    }

    // 2. Second pass: Remove any widgets that are no longer in the new scan results.
    GList *children_to_remove = NULL;
    GtkWidget *child = gtk_widget_get_first_child(list_box);
    while (child) {
        WifiNetwork *net = get_wifi_network_from_widget(child);
        // If the widget has data but its SSID is not in the new scan, mark it for removal.
        if (net && !g_hash_table_contains(new_ssids, net->ssid)) {
            children_to_remove = g_list_prepend(children_to_remove, child);
        }
        child = gtk_widget_get_next_sibling(child);
    }

    for (GList *l = children_to_remove; l != NULL; l = l->next) {
        gtk_box_remove(GTK_BOX(list_box), l->data);
    }
    
    g_list_free(children_to_remove);
    g_hash_table_destroy(new_ssids);
    free_wifi_network_list(networks);
}

static void on_bt_scan_results(GList *devices, gpointer user_data) {
    AppWidgets *widgets = (AppWidgets*)user_data;
    if(!widgets) return;

    GtkWidget *list_box = widgets->bt_list_box;
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(list_box)) != NULL) {
        gtk_box_remove(GTK_BOX(list_box), child);
    }

    if (g_list_length(devices) == 0) {
        gtk_box_append(GTK_BOX(list_box), gtk_label_new("No Bluetooth devices found."));
        free_bluetooth_device_list(devices);
        return;
    }

    for (GList *l = devices; l != NULL; l = l->next) {
        BluetoothDevice *dev_from_scan = l->data;
        BluetoothDevice *dev_copy = g_new0(BluetoothDevice, 1);
        dev_copy->address = g_strdup(dev_from_scan->address);
        dev_copy->name = g_strdup(dev_from_scan->name);
        dev_copy->is_connected = dev_from_scan->is_connected;

        const char *icon_name = "bluetooth-active-symbolic";
        GtkWidget *entry_button = create_list_entry(icon_name, dev_copy->name, dev_copy->is_connected);
        if (dev_copy->is_connected) {
            gtk_widget_add_css_class(entry_button, "active-network");
        }

        g_signal_connect(entry_button, "clicked", G_CALLBACK(on_bluetooth_device_clicked), dev_copy);
        g_signal_connect_swapped(entry_button, "destroy", G_CALLBACK(bluetooth_device_free), dev_copy);

        GtkGesture *right_click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
        g_signal_connect(right_click, "pressed", G_CALLBACK(on_bluetooth_right_click), dev_copy);
        gtk_widget_add_controller(entry_button, GTK_EVENT_CONTROLLER(right_click));

        gtk_box_append(GTK_BOX(list_box), entry_button);
    }
    free_bluetooth_device_list(devices);
}

// --- Page & Section Builders ---
static GtkWidget* create_wifi_page(AppWidgets *widgets) {
    GtkWidget *list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(list_box, 8);
    widgets->wifi_list_box = list_box;

    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), list_box);
    gtk_widget_set_size_request(scrolled_window, -1, LIST_REQUESTED_HEIGHT);
    gtk_widget_set_vexpand(scrolled_window, FALSE);
    gtk_widget_set_valign(scrolled_window, GTK_ALIGN_START);

    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *spinner = gtk_spinner_new();
    gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), scrolled_window);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), spinner);
    widgets->wifi_list_overlay = overlay;
    widgets->wifi_list_spinner = spinner;
    return overlay;
}

static GtkWidget* create_bluetooth_page(AppWidgets *widgets) {
    GtkWidget *list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(list_box, 8);
    widgets->bt_list_box = list_box;

    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), list_box);
    gtk_widget_set_size_request(scrolled_window, -1, LIST_REQUESTED_HEIGHT);
    gtk_widget_set_vexpand(scrolled_window, FALSE);
    gtk_widget_set_valign(scrolled_window, GTK_ALIGN_START);

    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *spinner = gtk_spinner_new();
    gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), scrolled_window);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), spinner);
    widgets->bt_list_overlay = overlay;
    widgets->bt_list_spinner = spinner;
    return overlay;
}

static GtkWidget* create_audio_page(AppWidgets *widgets) {
    GtkWidget *list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(list_box, 8);
    widgets->audio_list_box = list_box;

    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), list_box);
    gtk_widget_set_size_request(scrolled_window, -1, LIST_REQUESTED_HEIGHT);
    gtk_widget_set_vexpand(scrolled_window, FALSE);
    gtk_widget_set_valign(scrolled_window, GTK_ALIGN_START);
    return scrolled_window;
}



static gboolean reveal_on_idle(gpointer user_data) {
    gtk_revealer_set_reveal_child(GTK_REVEALER(user_data), TRUE);
    return G_SOURCE_REMOVE;
}

static void on_expandable_toggle_toggled(GtkToggleButton *toggled_button, AppWidgets *widgets) {
    if (!widgets) return;

    if (!gtk_toggle_button_get_active(toggled_button)) {
        gboolean any_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->wifi_toggle)) ||
                              gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->bt_toggle)) ||
                              gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->audio_toggle));
        if (!any_active) {
            gtk_revealer_set_reveal_child(widgets->stack_revealer, FALSE);
            wifi_scanner_stop(widgets->wifi_scanner);
            bluetooth_scanner_stop(widgets->bt_scanner);
        }
        return;
    }

    const char *target_page = NULL;
    GtkWidget *other_toggle1 = NULL, *other_toggle2 = NULL;
    gulong handler_id1 = 0, handler_id2 = 0;

    wifi_scanner_stop(widgets->wifi_scanner);
    bluetooth_scanner_stop(widgets->bt_scanner);

    if (toggled_button == GTK_TOGGLE_BUTTON(widgets->wifi_toggle)) {
        target_page = "wifi_page";
        other_toggle1 = widgets->bt_toggle;     handler_id1 = widgets->bt_toggle_handler_id;
        other_toggle2 = widgets->audio_toggle;  handler_id2 = widgets->audio_toggle_handler_id;
        wifi_scanner_start(widgets->wifi_scanner, WIFI_SCAN_INTERVAL_SECONDS);
    } else if (toggled_button == GTK_TOGGLE_BUTTON(widgets->bt_toggle)) {
        target_page = "bt_page";
        other_toggle1 = widgets->wifi_toggle;   handler_id1 = widgets->wifi_toggle_handler_id;
        other_toggle2 = widgets->audio_toggle;  handler_id2 = widgets->audio_toggle_handler_id;
        bluetooth_scanner_start(widgets->bt_scanner, BT_SCAN_INTERVAL_SECONDS);
    } else if (toggled_button == GTK_TOGGLE_BUTTON(widgets->audio_toggle)) {
        target_page = "audio_page";
        other_toggle1 = widgets->wifi_toggle;   handler_id1 = widgets->wifi_toggle_handler_id;
        other_toggle2 = widgets->bt_toggle;     handler_id2 = widgets->bt_toggle_handler_id;
        update_audio_device_list(widgets);
    }

    g_signal_handler_block(other_toggle1, handler_id1);
    g_signal_handler_block(other_toggle2, handler_id2);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(other_toggle1), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(other_toggle2), FALSE);
    g_signal_handler_unblock(other_toggle1, handler_id1);
    g_signal_handler_unblock(other_toggle2, handler_id2);

    if (target_page) {
        gtk_stack_set_visible_child_name(widgets->main_stack, target_page);
        g_idle_add(reveal_on_idle, widgets->stack_revealer);
    }
}

static GtkWidget* create_square_toggle(const char* icon_name, const char* text) {
    GtkWidget *button = gtk_toggle_button_new();
    gtk_widget_add_css_class(button, "square-toggle");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_button_set_child(GTK_BUTTON(button), box);

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    GtkWidget *label = gtk_label_new(text);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    return button;
}

static GtkWidget* create_pill_slider(const char* icon_name) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(box, "pill-slider");

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    GtkWidget *slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(slider), FALSE);
    gtk_widget_set_hexpand(slider, TRUE);

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), slider);
    return box;
}

static gboolean initial_state_update(gpointer user_data) {
    AppWidgets *widgets = user_data;
    if(!widgets) {
        return G_SOURCE_REMOVE;
    }
    on_system_event(SYSTEM_EVENT_VOLUME_CHANGED, widgets);
    on_system_event(SYSTEM_EVENT_BRIGHTNESS_CHANGED, widgets);
    return G_SOURCE_REMOVE;
}

// --- Main Plugin Entry Point ---
G_MODULE_EXPORT GtkWidget* create_widget(const char *config_string) {
    (void)config_string;
    
    // 1. Create the state struct FIRST, so we can pass its pointer around.
    AppWidgets *widgets = g_new0(AppWidgets, 1);

    // 2. Initialize the network manager, which is now simple and doesn't need callbacks.
    if (!network_manager_init()) {
        g_critical("Control Center Plugin: Failed to initialize NetworkManager.");
        // We can't continue if this fails. Free the widgets and return NULL.
        g_free(widgets);
        return NULL;
    }

    // 3. Get a connection to the system D-Bus and subscribe to the RELIABLE signal.
   // widgets->dbus_connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    //if (widgets->dbus_connection) {
    //    subscribe_to_wifi_device_signals(widgets);
   // }

    // --- The rest of the function builds the UI as before ---
    
    GtkWidget *root_widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_name(root_widget, "aurora-control-center");
    gtk_widget_add_css_class(root_widget, "control-center-widget");
    g_object_set_data_full(G_OBJECT(root_widget), "app-widgets", widgets, (GDestroyNotify)control_center_cleanup);

    // Top Grid (Wi-Fi, BT, etc.)
    GtkWidget *top_toggle_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(top_toggle_grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(top_toggle_grid), 8);
    gtk_box_append(GTK_BOX(root_widget), top_toggle_grid);

    widgets->wifi_toggle = create_square_toggle("network-wireless-symbolic", "Wi-Fi");
    widgets->bt_toggle = create_square_toggle("bluetooth-active-symbolic", "Bluetooth");
    widgets->audio_toggle = create_square_toggle("audio-card-symbolic", "Audio");
    GtkWidget *airplane_toggle = create_square_toggle("airplane-mode-symbolic", "Airplane");
    gtk_grid_attach(GTK_GRID(top_toggle_grid), widgets->wifi_toggle, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(top_toggle_grid), widgets->bt_toggle, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(top_toggle_grid), widgets->audio_toggle, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(top_toggle_grid), airplane_toggle, 3, 0, 1, 1);
    
    // Expandable content area (Revealer + Stack)
    widgets->stack_revealer = GTK_REVEALER(gtk_revealer_new());
    gtk_revealer_set_transition_type(widgets->stack_revealer, GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(widgets->stack_revealer, 250);
    widgets->main_stack = GTK_STACK(gtk_stack_new());
    gtk_widget_add_css_class(GTK_WIDGET(widgets->main_stack), "expandable-content-area");
    gtk_revealer_set_child(widgets->stack_revealer, GTK_WIDGET(widgets->main_stack));
    gtk_stack_add_named(widgets->main_stack, create_wifi_page(widgets), "wifi_page");
    gtk_stack_add_named(widgets->main_stack, create_bluetooth_page(widgets), "bt_page");
    gtk_stack_add_named(widgets->main_stack, create_audio_page(widgets), "audio_page");
    gtk_box_append(GTK_BOX(root_widget), GTK_WIDGET(widgets->stack_revealer));

    // Sliders Box
    GtkWidget *sliders_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(sliders_box, "sliders-box");
    
    GtkWidget* volume_label = gtk_label_new("System Volume");
    gtk_widget_add_css_class(volume_label, "slider-label");
    gtk_box_append(GTK_BOX(sliders_box), volume_label);
    
    GtkWidget *system_slider_box = create_pill_slider("audio-volume-high-symbolic");
    widgets->system_volume_slider = gtk_widget_get_last_child(system_slider_box);
    gtk_box_append(GTK_BOX(sliders_box), system_slider_box);

    GtkWidget* brightness_label = gtk_label_new("Brightness");
    gtk_widget_add_css_class(brightness_label, "slider-label");
    gtk_box_append(GTK_BOX(sliders_box), brightness_label);

    GtkWidget *brightness_slider_box = create_pill_slider("display-brightness-symbolic");
    widgets->brightness_slider = gtk_widget_get_last_child(brightness_slider_box);
    gtk_box_append(GTK_BOX(sliders_box), brightness_slider_box);
    gtk_box_append(GTK_BOX(root_widget), sliders_box);
    
    // Initialization & Signal Connections
    widgets->wifi_scanner = wifi_scanner_new(on_wifi_scan_results, widgets);
    widgets->bt_scanner = bluetooth_scanner_new(on_bt_scan_results, widgets);
    widgets->system_monitor = system_monitor_new(on_system_event, widgets);

    widgets->wifi_toggle_handler_id = g_signal_connect(widgets->wifi_toggle, "toggled", G_CALLBACK(on_expandable_toggle_toggled), widgets);
    widgets->bt_toggle_handler_id = g_signal_connect(widgets->bt_toggle, "toggled", G_CALLBACK(on_expandable_toggle_toggled), widgets);
    widgets->audio_toggle_handler_id = g_signal_connect(widgets->audio_toggle, "toggled", G_CALLBACK(on_expandable_toggle_toggled), widgets);
    g_signal_connect(airplane_toggle, "toggled", G_CALLBACK(toggle_airplane_mode), widgets);
    widgets->system_volume_handler_id = g_signal_connect(widgets->system_volume_slider, "value-changed", G_CALLBACK(on_system_volume_changed), NULL);
    widgets->brightness_slider_handler_id = g_signal_connect(widgets->brightness_slider, "value-changed", G_CALLBACK(on_brightness_changed), NULL);
    
    g_idle_add(initial_state_update, widgets);

    return root_widget;
}