#include <gtk/gtk.h>
#include <glib-unix.h>

#include "wifi_scanner.h"
#include "bluetooth_scanner.h"
#include "network_manager.h"
#include "bluetooth_manager.h"
#include "audio_manager.h"
#include "brightness_manager.h"
#include "system_monitor.h"
#include "qr.h"


const guint WIFI_SCAN_INTERVAL_SECONDS = 10;
const guint BT_SCAN_INTERVAL_SECONDS = 15;
const int LIST_REQUESTED_HEIGHT = 250;

// --- Main State Struct ---
typedef struct {
    GtkRevealer *stack_revealer;
    GtkStack *main_stack;
    GtkWidget *wifi_toggle, *bt_toggle, *audio_toggle;
    gulong wifi_toggle_handler_id, bt_toggle_handler_id, audio_toggle_handler_id;
    WifiScanner *wifi_scanner;
    BluetoothScanner *bt_scanner;
    SystemMonitor *system_monitor;
    
    GtkWidget *wifi_connected_header;
    GtkWidget *wifi_connected_list_box;
    GtkWidget *wifi_available_header;
    GtkWidget *wifi_available_list_box;
    
    // --- MODIFICATION: Updated Bluetooth widget pointers ---
    GtkWidget *bt_connected_header;
    GtkWidget *bt_connected_list_box;
    GtkWidget *bt_available_header;
    GtkWidget *bt_available_list_box;
    GtkWidget *bt_header_spinner;
    GtkWidget *bt_list_overlay; // Keep the overlay for the main spinner
    GtkWidget *bt_list_spinner;  // Keep the main spinner for initial load
    // --- END MODIFICATION ---

    GtkWidget *audio_list_box;
    GtkWidget *system_volume_slider;
    gulong system_volume_handler_id;
    GtkWidget *brightness_slider;
    gulong brightness_slider_handler_id;
    gboolean airplane_mode_active;
    gboolean wifi_was_on_before_airplane;
    gboolean bt_was_on_before_airplane;
    
} AppWidgets;

typedef struct {
    AppWidgets *widgets;
    GtkWidget *row_widget; // The widget that was clicked to initiate the connection
       gboolean was_hidden_for_auth; 
} WifiOpContext;




typedef struct {
    AppWidgets *widgets;
    GtkWidget *row_widget;
} BluetoothOperationContext;




// --- Forward Declarations ---
static void on_wifi_network_clicked(GtkButton *button, gpointer user_data);
static void on_bluetooth_device_clicked(GtkButton *button, gpointer user_data);
static void on_wifi_operation_finished(gboolean success, gpointer user_data);
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
static void on_bluetooth_right_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
static void on_wifi_scan_results(GList *networks, gpointer user_data);
static void on_bt_scan_results(GList *devices, gpointer user_data);


static gboolean initial_state_update(gpointer user_data);

static void on_qr_button_clicked(GtkButton *button, gpointer user_data);
static void on_qr_code_received(GdkPixbuf *pixbuf, gpointer user_data);

static void on_qr_button_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);

// --- NEW/MODIFIED Forward Declarations for Bluetooth ---
static BluetoothDevice* get_bluetooth_device_from_widget(GtkWidget *list_item);
static void update_bt_widget_state(GtkWidget *button, BluetoothDevice *dev);


typedef struct {
    GtkWidget *parent_widget; // The button that was clicked
    GtkPopover *spinner_popover;
    gchar *ssid;
} QRCodeCallbackContext;


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
    wifi_scanner_stop(widgets->wifi_scanner);
    bluetooth_scanner_stop(widgets->bt_scanner);
    wifi_scanner_free(widgets->wifi_scanner);
    bluetooth_scanner_free(widgets->bt_scanner);
    system_monitor_free(widgets->system_monitor);

    // --- NEW: Shutdown the Bluetooth manager ---
    bluetooth_manager_shutdown();
    // --- END NEW ---

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


// NEW HELPER: Updates just the sublabel of a Wi-Fi list row.
static void update_wifi_row_sublabel(GtkWidget *row_widget, const gchar *text) {
    if (!row_widget || !GTK_IS_BUTTON(row_widget)) return;

    GtkWidget *box = gtk_button_get_child(GTK_BUTTON(row_widget));
    if (!box) return;

    // The text container is usually the second widget in the box (after the icon stack).
    GtkWidget *text_container = gtk_widget_get_next_sibling(gtk_widget_get_first_child(box));
    if (text_container && GTK_IS_BOX(text_container)) {
        GtkWidget *sublabel_widget = gtk_widget_get_last_child(text_container);
        if (sublabel_widget && GTK_IS_LABEL(sublabel_widget) && gtk_widget_has_css_class(sublabel_widget, "connected-sublabel")) {
            gtk_label_set_text(GTK_LABEL(sublabel_widget), text);
            gtk_widget_set_visible(sublabel_widget, TRUE);
        }
    }
}

static void on_auth_flow_finished(gboolean success, gpointer user_data) {
    // This callback runs after the user has entered their password or cancelled.
    // Its only job is to restart the scanner.
    (void)success; // We don't care about the result, just that it's over.
    
    WifiOpContext *context = user_data;
    if (!context || !context->widgets) {
        if (context) g_free(context);
        return;
    }

    g_print("UI: Authentication flow finished. Restarting periodic Wi-Fi scanner.\n");
    // The scanner will trigger an immediate scan upon starting. This ensures that
    // the next time the user opens the control center, the network list will be
    // accurate (showing the network as "Connecting...", "Saved", or not at all).
    wifi_scanner_start(context->widgets->wifi_scanner, WIFI_SCAN_INTERVAL_SECONDS);

    g_free(context);
}

static void on_wifi_operation_finished(gboolean success, gpointer user_data) {
    WifiOpContext *context = user_data;
    if (!context || !context->widgets) {
        g_warning("Context or widgets are NULL in on_wifi_operation_finished. Aborting.");
        if (context) g_free(context);
        return;
    }
    
    // The periodic scanner continues to run as normal.
    g_print("UI: Wi-Fi operation finished. Triggering immediate UI refresh.\n");
    wifi_scanner_trigger_scan(context->widgets->wifi_scanner);

    if (!success) {
        g_print("UI: Wi-Fi operation reported failure or cancellation.\n");
        if (context->row_widget) {
            // Visually reset the row that was trying to connect, if applicable.
            GtkWidget* box = gtk_button_get_child(GTK_BUTTON(context->row_widget));
            GtkWidget* icon_stack = gtk_widget_get_first_child(box);
            if (icon_stack && GTK_IS_STACK(icon_stack)) {
                gtk_stack_set_visible_child_name(GTK_STACK(icon_stack), "icon");
            }
            // Optionally provide feedback on the row itself
            // update_wifi_row_sublabel(context->row_widget, "Operation failed");
        }
    }

    // The context for this single operation is no longer needed.
    g_free(context);
}


// The callback that runs AFTER the background connect/disconnect thread finishes.
static void on_bt_operation_finished(gboolean success, gpointer user_data) {
    g_print("[UI DEBUG] on_bt_operation_finished callback received. Success: %s\n", success ? "TRUE" : "FALSE");

    (void)success; // We don't use the success flag here, but it's good to see it.
    BluetoothOperationContext *context = user_data;
    if(!context || !context->widgets) {
        if (context) g_free(context);
        return;
    }

    // A refresh is needed for both connect and disconnect to update the list state.
    // The new `on_bt_scan_results` will handle resetting any spinners.
    g_print("[UI DEBUG] Triggering UI refresh from on_bt_operation_finished.\n");
    bluetooth_scanner_trigger_scan(context->widgets->bt_scanner);
    
    g_free(context);
}

static void on_sink_set_finished(gboolean success, gpointer user_data) {
    if (success) update_audio_device_list(user_data);
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
static void on_qr_button_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; (void)x; (void)y; (void)user_data;

    // Use the GTK_GESTURE() macro to cast the GtkGestureClick* to a GtkGesture*
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

    g_print("[UI DEBUG] Gesture captured click on QR button!\n");
    GtkWidget *button = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    on_qr_button_clicked(GTK_BUTTON(button), NULL);
}

static void on_wifi_network_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    WifiNetwork *net = get_wifi_network_from_widget(GTK_WIDGET(button));
    AppWidgets *widgets = get_widgets_from_child(GTK_WIDGET(button));
    if (!widgets || !net || net->is_active) return;

    g_autofree gchar *ssid_copy = g_strdup(net->ssid);
    g_autofree gchar *object_path_copy = g_strdup(net->object_path);
    gboolean is_secure = net->is_secure;

    g_autofree gchar *existing_connection_path = find_connection_for_ssid(ssid_copy);

    // Show a spinner on the clicked row for all connection attempts.
    GtkWidget* box = gtk_button_get_child(GTK_BUTTON(button));
    GtkWidget* icon_stack = gtk_widget_get_first_child(box);
    if (icon_stack && GTK_IS_STACK(icon_stack)) {
        GtkWidget* spinner = gtk_stack_get_child_by_name(GTK_STACK(icon_stack), "spinner");
        if(spinner) gtk_spinner_start(GTK_SPINNER(spinner));
        gtk_stack_set_visible_child_name(GTK_STACK(icon_stack), "spinner");
    }
    
    // Check if this is the specific case that requires a password prompt
    if (is_secure && !existing_connection_path) {
        // --- THIS IS THE CRITICAL PATH FOR PASSWORD PROMPTS ---
        g_print("UI: New secure network. Hiding UI for password prompt.\n");
        
        wifi_scanner_stop(widgets->wifi_scanner);
        g_spawn_command_line_async("aurora-shell --toggle control-center", NULL);
        
        // Create a context for our new callback. We don't have a valid row_widget
        // because the UI is being destroyed, so we set it to NULL.
        WifiOpContext *auth_context = g_new0(WifiOpContext, 1);
        auth_context->widgets = widgets;
        auth_context->row_widget = NULL; 

        // Call the activation function with our NEW, specialized callback.
        add_and_activate_wifi_connection_async(ssid_copy, object_path_copy, NULL, is_secure, on_auth_flow_finished, auth_context);

    } else {
        // --- THIS IS THE PATH FOR ALL OTHER CONNECTIONS (known, open, etc.) ---
        WifiOpContext *op_context = g_new0(WifiOpContext, 1);
        op_context->widgets = widgets;
        op_context->row_widget = GTK_WIDGET(button);

        if (existing_connection_path) {
            activate_wifi_connection_async(existing_connection_path, object_path_copy, on_wifi_operation_finished, op_context);
        } else {
            add_and_activate_wifi_connection_async(ssid_copy, object_path_copy, NULL, is_secure, on_wifi_operation_finished, op_context);
        }
    }
}

// --- NEW CLICK HANDLER for Bluetooth rows ---
static void on_bluetooth_device_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    BluetoothDevice *dev = get_bluetooth_device_from_widget(GTK_WIDGET(button));
    AppWidgets *widgets = get_widgets_from_child(GTK_WIDGET(button));
    
    if (!widgets || !dev || dev->is_connected) {
        return;
    }

    // Show a spinner on the clicked row.
    GtkWidget* box = gtk_button_get_child(GTK_BUTTON(button));
    GtkWidget* icon_stack = gtk_widget_get_first_child(box);
    if (icon_stack && GTK_IS_STACK(icon_stack)) {
        GtkWidget* spinner = gtk_stack_get_child_by_name(GTK_STACK(icon_stack), "spinner");
        if(spinner) gtk_spinner_start(GTK_SPINNER(spinner));
        gtk_stack_set_visible_child_name(GTK_STACK(icon_stack), "spinner");
    }

    // Create a context for the operation callback
    BluetoothOperationContext *context = g_new0(BluetoothOperationContext, 1);
    context->widgets = widgets;
    context->row_widget = GTK_WIDGET(button);

    connect_to_bluetooth_device_async(dev->address, on_bt_operation_finished, context);
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
    // Get the widgets pointer BEFORE popping down the popover.
    AppWidgets *widgets = get_widgets_from_child(GTK_WIDGET(button));
    
    // Now it's safe to popdown.
    gtk_popover_popdown(popover);

    if (!widgets) {
        g_warning("FORGET_BTN_CLICK: Could not find AppWidgets from button!");
        return;
    }

    const char *ssid = g_object_get_data(G_OBJECT(button), "ssid-to-forget");
    g_print("\n\n[FORGET DEBUG] 1. 'Forget' button clicked for SSID: '%s'\n", ssid);
    
    GtkWidget *row_widget = g_object_get_data(G_OBJECT(button), "parent-widget");

    WifiOpContext *op_context = g_new0(WifiOpContext, 1);
    op_context->widgets = widgets;
    op_context->row_widget = row_widget;
    forget_wifi_connection_async(ssid, on_wifi_operation_finished, op_context);
}

static void on_disconnect_button_clicked(GtkButton *button, GtkPopover *popover) {
    // Get the widgets pointer BEFORE popping down the popover.
    AppWidgets *widgets = get_widgets_from_child(GTK_WIDGET(button));

    // Now it's safe to popdown.
    gtk_popover_popdown(popover);

    if (!widgets) {
        g_warning("DISCONNECT_BTN_CLICK: Could not find AppWidgets from button!");
        return;
    }

    WifiOpContext *op_context = g_new0(WifiOpContext, 1);
    op_context->widgets = widgets;
    op_context->row_widget = NULL; // No specific row for this operation
    disconnect_wifi_async(on_wifi_operation_finished, op_context);
}

// The helper function that actually calls the disconnect logic.
static void on_bt_do_disconnect(const gchar* address, GtkWidget* row_widget, AppWidgets* widgets) {
    g_print("[UI DEBUG] on_bt_do_disconnect called for address: %s\n", address);

    if (!widgets || !row_widget) return;
    
    GtkWidget* box = gtk_button_get_child(GTK_BUTTON(row_widget));
    GtkWidget* icon_stack = gtk_widget_get_first_child(box);
    if (icon_stack && GTK_IS_STACK(icon_stack)) {
        GtkWidget* spinner = gtk_stack_get_child_by_name(GTK_STACK(icon_stack), "spinner");
        if(spinner) gtk_spinner_start(GTK_SPINNER(spinner));
        gtk_stack_set_visible_child_name(GTK_STACK(icon_stack), "spinner");
    }

    BluetoothOperationContext *context = g_new0(BluetoothOperationContext, 1);
    context->widgets = widgets;
    context->row_widget = row_widget;
    
    g_print("[UI DEBUG] -> Calling disconnect_bluetooth_device_async...\n");
    disconnect_bluetooth_device_async(address, on_bt_operation_finished, context);
}


// --- REMOVED `on_bt_row_connect_button_clicked` ---

// NEW HANDLER for the "Connect" button in the right-click menu
static void on_connect_button_clicked(GtkButton *button, GtkPopover *popover) {
    gtk_popover_popdown(popover); // Close the menu first

    // Retrieve the data we attached to the button
    WifiNetwork *net = g_object_get_data(G_OBJECT(button), "wifi-network-data");
    AppWidgets *widgets = g_object_get_data(G_OBJECT(button), "app-widgets-data");
    // Get the original row widget that the popover is attached to
    GtkWidget *row_widget = gtk_widget_get_parent(GTK_WIDGET(popover));

    if (!widgets || !net || net->is_active) {
        return;
    }

    // Create context for the callback
    WifiOpContext *op_context = g_new0(WifiOpContext, 1);
    op_context->widgets = widgets;
    op_context->row_widget = row_widget; // Pass the original row widget

    g_autofree gchar *existing_connection_path = find_connection_for_ssid(net->ssid);
    if (existing_connection_path) {
        activate_wifi_connection_async(existing_connection_path, net->object_path, on_wifi_operation_finished, op_context);
    } else {
        // We'll need to add a password prompt here in the future, but for now, it will
        // attempt to connect to open networks or use a previously known password.
        add_and_activate_wifi_connection_async(net->ssid, net->object_path, NULL, net->is_secure, on_wifi_operation_finished, op_context);
    }
}

// Helper to get the BluetoothDevice data back from a row widget.
static BluetoothDevice* get_bluetooth_device_from_widget(GtkWidget *list_item) {
    if (!list_item) return NULL;
    return g_object_get_data(G_OBJECT(list_item), "device-data");
}

// --- MODIFIED HELPER: Updates a BT row in-place ---
static void update_bt_widget_state(GtkWidget *button, BluetoothDevice *dev) {
    GtkWidget *box = gtk_button_get_child(GTK_BUTTON(button));
    if (!box) return;

    GtkWidget *icon_stack = gtk_widget_get_first_child(box);
    if (icon_stack && GTK_IS_STACK(icon_stack)) {
        gtk_stack_set_visible_child_name(GTK_STACK(icon_stack), "icon");
    }

    if (dev->is_connected) {
        gtk_widget_add_css_class(button, "active-network");
    } else {
        gtk_widget_remove_css_class(button, "active-network");
    }

    // --- MODIFICATION: Update sublabel for Paired/Connected state ---
    const gchar *sublabel_text = NULL;
    if (dev->is_connected) {
        sublabel_text = "Connected";
    } else if (dev->is_paired) {
        sublabel_text = "Paired";
    }

    GtkWidget *text_container = gtk_widget_get_next_sibling(icon_stack);
    if (text_container && GTK_IS_BOX(text_container)) {
        GtkWidget *sublabel_widget = gtk_widget_get_last_child(text_container);
        if (sublabel_widget && GTK_IS_LABEL(sublabel_widget) && gtk_widget_has_css_class(sublabel_widget, "connected-sublabel")) {
            if (sublabel_text) {
                gtk_label_set_text(GTK_LABEL(sublabel_widget), sublabel_text);
                gtk_widget_set_visible(sublabel_widget, TRUE);
            } else {
                gtk_widget_set_visible(sublabel_widget, FALSE);
            }
        }
    }
    // --- END MODIFICATION ---
}


// HELPER 1: Correctly gets the WifiNetwork data back from a list item's controller.
static WifiNetwork* get_wifi_network_from_widget(GtkWidget *list_item) {
    return g_object_get_data(G_OBJECT(list_item), "wifi-network-data");
}

// HELPER 2: Updates the UI of an existing row widget without destroying it.
static void update_wifi_widget_state(GtkWidget *button, WifiNetwork *net) {
    GtkWidget *box = gtk_button_get_child(GTK_BUTTON(button));
    if (!box) return;
 
    // --- Update the Icon and Spinner ---
    GtkWidget *icon_stack = gtk_widget_get_first_child(box);
    if (icon_stack && GTK_IS_STACK(icon_stack)) {
        GtkWidget *icon = gtk_stack_get_child_by_name(GTK_STACK(icon_stack), "icon");
        if (icon && GTK_IS_IMAGE(icon)) {
            const char *icon_name = get_wifi_icon_name_for_signal(net->strength);
            gtk_image_set_from_icon_name(GTK_IMAGE(icon), icon_name);
        }
        // Ensure spinner is always hidden on a refresh. It's only shown on click.
        gtk_stack_set_visible_child_name(GTK_STACK(icon_stack), "icon");
    }
 
    // --- Update the Active State ---
    if (net->is_active) {
        gtk_widget_add_css_class(button, "active-network");
    } else {
        gtk_widget_remove_css_class(button, "active-network");
    }
    
    // --- FIX: Update the sublabel text, which was previously skipped ---
    const gchar *sublabel_text = NULL;
    if (net->is_active) {
        switch (net->connectivity) {
            case WIFI_STATE_CONNECTED: sublabel_text = "Connected"; break;
            case WIFI_STATE_LIMITED: sublabel_text = "Connected / No Internet Access"; break;
            case WIFI_STATE_CONNECTING: sublabel_text = "Connecting..."; break;
            default: sublabel_text = "Connected"; break;
        }
    } else if (net->is_known) {
        sublabel_text = "Saved";
    }

    // Find the container for the text labels (it's the next sibling after the icon_stack)
    GtkWidget *text_container = icon_stack ? gtk_widget_get_next_sibling(icon_stack) : NULL;
    
    // We can only update the sublabel if the widget was created with one (i.e., text_container is a GtkBox).
    if (text_container && GTK_IS_BOX(text_container)) {
        GtkWidget *sublabel_widget = gtk_widget_get_last_child(text_container);
        if (sublabel_widget && GTK_IS_LABEL(sublabel_widget) && gtk_widget_has_css_class(sublabel_widget, "connected-sublabel")) {
            if (sublabel_text) {
                gtk_label_set_text(GTK_LABEL(sublabel_widget), sublabel_text);
                gtk_widget_set_visible(sublabel_widget, TRUE);
            } else {
                gtk_widget_set_visible(sublabel_widget, FALSE);
            }
        }
    }
}

// --- Gesture Handlers (Right-Click) ---
static void on_wifi_right_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; (void)x; (void)y; (void)user_data;

    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

    GtkWidget *button_widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    WifiNetwork *net = get_wifi_network_from_widget(button_widget);
    AppWidgets *widgets = get_widgets_from_child(button_widget);
    if (!net) return;
    
    GtkPopover *popover = GTK_POPOVER(gtk_popover_new());
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

        if (net->is_known) {
            GtkWidget *forget_button = create_popover_action_button("edit-delete-symbolic", "Forget");
            g_object_set_data(G_OBJECT(forget_button), "ssid-to-forget", (gpointer)net->ssid);
            g_object_set_data(G_OBJECT(forget_button), "parent-widget", button_widget);
            
            g_print(">>> DEBUG: Connecting 'clicked' signal for FORGET button.\n");

            g_signal_connect(forget_button, "clicked", G_CALLBACK(on_forget_button_clicked), popover);
            gtk_box_append(GTK_BOX(menu_box), forget_button);
        }
    }
    
    gtk_widget_set_parent(GTK_WIDGET(popover), button_widget);
    gtk_popover_popup(popover);
}

// The handler for the "Disconnect" button in the right-click popover menu.
static void on_bt_popover_disconnect_clicked(GtkButton *button, GtkPopover *popover) {
    // Get the widgets pointer BEFORE popping down the popover.
    AppWidgets *widgets = get_widgets_from_child(GTK_WIDGET(button));

    // Now it's safe to popdown.
    gtk_popover_popdown(popover);

    if (!widgets) {
        g_warning("BT_DISCONNECT_BTN_CLICK: Could not find AppWidgets from button!");
        return;
    }

    const char *address = g_object_get_data(G_OBJECT(button), "address-to-disconnect");
    GtkWidget *row_widget = g_object_get_data(G_OBJECT(button), "parent-row-widget");

    g_print("[UI DEBUG] Popover Disconnect clicked for address: %s\n", address);
    g_print("[UI DEBUG] -> Calling on_bt_do_disconnect....\n");
    on_bt_do_disconnect(address, row_widget, widgets);
}

static void on_bluetooth_right_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; (void)x; (void)y;
     (void)user_data; 
    GtkWidget *button_widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    BluetoothDevice *dev = get_bluetooth_device_from_widget(button_widget);
    
    if (!dev || !dev->is_connected) {
        return;
    }
    
    GtkPopover *popover = GTK_POPOVER(gtk_popover_new());
    g_signal_connect(popover, "closed", G_CALLBACK(on_popover_closed), NULL);

    GtkWidget *menu_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_popover_set_child(popover, menu_box);

    GtkWidget *disconnect_button = create_popover_action_button("network-wired-disconnected-symbolic", "Disconnect");
    g_object_set_data(G_OBJECT(disconnect_button), "address-to-disconnect", (gpointer)dev->address);
    // Store a reference to the main row widget
    g_object_set_data(G_OBJECT(disconnect_button), "parent-row-widget", button_widget);
    g_signal_connect(disconnect_button, "clicked", G_CALLBACK(on_bt_popover_disconnect_clicked), popover);
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

    if (is_active) {
        GtkWidget *check_icon = gtk_image_new_from_icon_name("object-select-symbolic");
        gtk_widget_add_css_class(check_icon, "active-symbol");
        gtk_box_append(GTK_BOX(box), check_icon);
    }

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

// NEW HANDLER: Called by the bluetooth_manager when the device list changes.
static void on_bt_devices_updated(GList *devices, gpointer user_data) {
    // This function simply forwards the updated list to your existing UI logic.
    // We keep the old on_bt_scan_results function to avoid rewriting all the UI code.
    on_bt_scan_results(devices, user_data);
}

// The final callback that displays the QR code
static void on_qr_code_received(GdkPixbuf *pixbuf, gpointer user_data) {
    QRCodeCallbackContext *context = user_data;
    
    if (context->spinner_popover) {
        gtk_popover_popdown(context->spinner_popover);
    }

    GtkPopover *qr_popover = GTK_POPOVER(gtk_popover_new());
    gtk_widget_add_css_class(GTK_WIDGET(qr_popover), "qr-code-popover"); // Add class to popover
    g_signal_connect(qr_popover, "closed", G_CALLBACK(on_popover_closed), NULL);

    if (pixbuf) {
        GtkWidget *popover_content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        
        GdkPaintable *paintable = GDK_PAINTABLE(gdk_texture_new_for_pixbuf(pixbuf));
        GtkWidget *qr_image = gtk_image_new_from_paintable(paintable);
        gtk_image_set_pixel_size(GTK_IMAGE(qr_image), 256);
        
        g_autofree gchar *label_text = g_strdup_printf("Scan to connect to \"%s\"", context->ssid);
        GtkWidget *info_label = gtk_label_new(label_text);
        gtk_label_set_wrap(GTK_LABEL(info_label), TRUE);
        gtk_widget_add_css_class(info_label, "dim-label"); // Keep for fallback
        gtk_widget_add_css_class(info_label, "qr-info-label"); // Add our specific class

        gtk_box_append(GTK_BOX(popover_content_box), qr_image);
        gtk_box_append(GTK_BOX(popover_content_box), info_label);

        gtk_widget_set_margin_top(popover_content_box, 15);
        gtk_widget_set_margin_bottom(popover_content_box, 15);
        gtk_widget_set_margin_start(popover_content_box, 15);
        gtk_widget_set_margin_end(popover_content_box, 15);

        gtk_popover_set_child(qr_popover, popover_content_box);

    } else {
        GtkWidget *error_label = gtk_label_new("Could not retrieve Wi-Fi password.\nCheck terminal for D-Bus or Polkit errors.");
        gtk_label_set_wrap(GTK_LABEL(error_label), TRUE);
        gtk_widget_set_margin_start(error_label, 15);
        gtk_widget_set_margin_end(error_label, 15);
        gtk_widget_set_margin_top(error_label, 15);
        gtk_widget_set_margin_bottom(error_label, 15);
        gtk_popover_set_child(qr_popover, error_label);
    }

    gtk_widget_set_parent(GTK_WIDGET(qr_popover), context->parent_widget);
    gtk_popover_popup(qr_popover);
    
    g_free(context->ssid);
    g_free(context);
}


static void on_qr_button_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    WifiNetwork *net = g_object_get_data(G_OBJECT(button), "wifi-network-data");
    if (!net) return;
    
    GtkPopover *spinner_popover = GTK_POPOVER(gtk_popover_new());
    GtkWidget *spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_widget_set_margin_start(spinner, 15);
    gtk_widget_set_margin_end(spinner, 15);
    gtk_widget_set_margin_top(spinner, 15);
    gtk_widget_set_margin_bottom(spinner, 15);
    gtk_popover_set_child(spinner_popover, spinner);
    gtk_widget_set_parent(GTK_WIDGET(spinner_popover), GTK_WIDGET(button));
    gtk_popover_popup(spinner_popover);

    QRCodeCallbackContext *context = g_new0(QRCodeCallbackContext, 1);
    context->parent_widget = GTK_WIDGET(button);
    context->spinner_popover = spinner_popover;
    context->ssid = g_strdup(net->ssid); 
    
    generate_wifi_qr_code_async(net->ssid, on_qr_code_received, context);
}




static void on_wifi_scan_results(GList *networks, gpointer user_data) {
    AppWidgets *widgets = (AppWidgets*)user_data;
    if (!widgets || !widgets->wifi_connected_list_box || !widgets->wifi_available_list_box) { 
        free_wifi_network_list(networks); 
        return; 
    }

    GtkWidget *recent_list = widgets->wifi_connected_list_box;
    GtkWidget *available_list = widgets->wifi_available_list_box;

    GHashTable *existing_widgets = g_hash_table_new(g_str_hash, g_str_equal);
    for (GtkWidget *child = gtk_widget_get_first_child(recent_list); child != NULL; child = gtk_widget_get_next_sibling(child)) {
        WifiNetwork *net = get_wifi_network_from_widget(child);
        if (net && net->ssid) g_hash_table_insert(existing_widgets, (gpointer)net->ssid, child);
    }
    for (GtkWidget *child = gtk_widget_get_first_child(available_list); child != NULL; child = gtk_widget_get_next_sibling(child)) {
        WifiNetwork *net = get_wifi_network_from_widget(child);
        if (net && net->ssid) g_hash_table_insert(existing_widgets, (gpointer)net->ssid, child);
    }

    GHashTable *seen_ssids = g_hash_table_new(g_str_hash, g_str_equal);
    for (GList *l = networks; l != NULL; l = l->next) {
        WifiNetwork *net_from_scan = l->data;
        if (!net_from_scan || !net_from_scan->ssid) continue;

        g_hash_table_insert(seen_ssids, (gpointer)net_from_scan->ssid, GINT_TO_POINTER(1));
        GtkWidget *existing_button = g_hash_table_lookup(existing_widgets, net_from_scan->ssid);

        if (existing_button) {
            WifiNetwork *net_copy = get_wifi_network_from_widget(existing_button);
            net_copy->strength = net_from_scan->strength;
            net_copy->is_active = net_from_scan->is_active;
            net_copy->connectivity = net_from_scan->connectivity;
            net_copy->is_known = net_from_scan->is_known;
            update_wifi_widget_state(existing_button, net_copy);
            
            GtkWidget *box = gtk_button_get_child(GTK_BUTTON(existing_button));
            GtkWidget *last_child = gtk_widget_get_last_child(box);
            gboolean qr_button_exists = (last_child && gtk_widget_has_css_class(last_child, "wifi-qr-button"));
            if (net_copy->is_active && !qr_button_exists) {
                GtkWidget *qr_button = gtk_button_new_from_icon_name("view-grid-symbolic");
                gtk_widget_add_css_class(qr_button, "wifi-qr-button");
                gtk_widget_set_tooltip_text(qr_button, "Show QR code to connect");
                gtk_widget_set_valign(qr_button, GTK_ALIGN_CENTER);
                g_object_set_data(G_OBJECT(qr_button), "wifi-network-data", net_copy);
                GtkGesture *qr_click_gesture = gtk_gesture_click_new();
                g_signal_connect(qr_click_gesture, "pressed", G_CALLBACK(on_qr_button_pressed), NULL);
                gtk_widget_add_controller(qr_button, GTK_EVENT_CONTROLLER(qr_click_gesture));
                gtk_box_append(GTK_BOX(box), qr_button);
            } else if (!net_copy->is_active && qr_button_exists) {
                gtk_box_remove(GTK_BOX(box), last_child);
            }
            
            // --- MODIFICATION: Logic for moving between lists ---
            GtkWidget *current_parent = gtk_widget_get_parent(existing_button);
            if ((net_copy->is_active || net_copy->is_known) && current_parent != recent_list) {
                gtk_widget_unparent(existing_button);
                gtk_box_append(GTK_BOX(recent_list), existing_button);
            } else if (!net_copy->is_active && !net_copy->is_known && current_parent != available_list) {
                gtk_widget_unparent(existing_button);
                gtk_box_append(GTK_BOX(available_list), existing_button);
            }
            // --- END MODIFICATION ---

        } else {
            WifiNetwork *net_copy = g_new0(WifiNetwork, 1);
            *net_copy = *net_from_scan;
            net_copy->ssid = g_strdup(net_from_scan->ssid);
            net_copy->object_path = g_strdup(net_from_scan->object_path);
            GtkWidget *entry_button = gtk_button_new();
            g_object_set_data_full(G_OBJECT(entry_button), "wifi-network-data", net_copy, (GDestroyNotify)wifi_network_free);
            gtk_widget_add_css_class(entry_button, "list-item-button");
            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_button_set_child(GTK_BUTTON(entry_button), box);
            GtkWidget *icon_stack = gtk_stack_new();
            gtk_widget_set_valign(icon_stack, GTK_ALIGN_CENTER);
            const char *icon_name = get_wifi_icon_name_for_signal(net_copy->strength);
            gtk_stack_add_named(GTK_STACK(icon_stack), gtk_image_new_from_icon_name(icon_name), "icon");
            gtk_stack_add_named(GTK_STACK(icon_stack), gtk_spinner_new(), "spinner");
            gtk_stack_set_visible_child_name(GTK_STACK(icon_stack), "icon");
            gtk_box_append(GTK_BOX(box), icon_stack);
            GtkWidget *text_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            gtk_widget_set_hexpand(text_vbox, TRUE);
            gtk_box_append(GTK_BOX(box), text_vbox);
            GtkWidget *label = gtk_label_new(net_copy->ssid);
            gtk_label_set_xalign(GTK_LABEL(label), 0.0);
            gtk_box_append(GTK_BOX(text_vbox), label);
            GtkWidget *sublabel = gtk_label_new("");
            gtk_widget_add_css_class(sublabel, "connected-sublabel");
            gtk_label_set_xalign(GTK_LABEL(sublabel), 0.0);
            gtk_box_append(GTK_BOX(text_vbox), sublabel);
            
            update_wifi_widget_state(entry_button, net_copy); // Call helper to set sublabel text

            if (net_copy->is_secure) {
                GtkWidget *secure_icon = gtk_image_new_from_icon_name("network-wireless-encrypted-symbolic");
                gtk_widget_set_valign(secure_icon, GTK_ALIGN_CENTER);
                gtk_box_append(GTK_BOX(box), secure_icon);
            }
            g_signal_connect(entry_button, "clicked", G_CALLBACK(on_wifi_network_clicked), NULL);

            GtkGesture *right_click = gtk_gesture_click_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
            g_signal_connect(right_click, "pressed", G_CALLBACK(on_wifi_right_click), NULL);
            gtk_widget_add_controller(entry_button, GTK_EVENT_CONTROLLER(right_click));

            // --- MODIFICATION: Logic for placing new widgets ---
            if (net_copy->is_active || net_copy->is_known) {
                if (net_copy->is_active) {
                    GtkWidget *qr_button = gtk_button_new_from_icon_name("view-grid-symbolic");
                    gtk_widget_add_css_class(qr_button, "wifi-qr-button");
                    gtk_widget_set_tooltip_text(qr_button, "Show QR code to connect");
                    gtk_widget_set_valign(qr_button, GTK_ALIGN_CENTER);
                    g_object_set_data(G_OBJECT(qr_button), "wifi-network-data", net_copy);
                    GtkGesture *qr_click_gesture = gtk_gesture_click_new();
                    g_signal_connect(qr_click_gesture, "pressed", G_CALLBACK(on_qr_button_pressed), NULL);
                    gtk_widget_add_controller(qr_button, GTK_EVENT_CONTROLLER(qr_click_gesture));
                    gtk_box_append(GTK_BOX(box), qr_button);
                }
                gtk_box_append(GTK_BOX(recent_list), entry_button);
            } else {
                gtk_box_append(GTK_BOX(available_list), entry_button);
            }
            // --- END MODIFICATION ---
        }
    }

    GList *widgets_to_remove = NULL;
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, existing_widgets);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (!g_hash_table_contains(seen_ssids, key)) {
            widgets_to_remove = g_list_prepend(widgets_to_remove, value);
        }
    }
    for (GList *l = widgets_to_remove; l != NULL; l = l->next) {
        GtkWidget *widget_to_remove = l->data;
        if (GTK_IS_WIDGET(widget_to_remove)) {
            gtk_widget_unparent(widget_to_remove);
        }
    }
    g_list_free(widgets_to_remove);
    g_hash_table_destroy(existing_widgets);
    g_hash_table_destroy(seen_ssids);

    gboolean has_recent = (gtk_widget_get_first_child(recent_list) != NULL);
    gboolean has_available = (gtk_widget_get_first_child(available_list) != NULL);

    if (!is_wifi_enabled()) {
        GtkWidget *child;
        while ((child = gtk_widget_get_first_child(recent_list))) { gtk_box_remove(GTK_BOX(recent_list), child); }
        while ((child = gtk_widget_get_first_child(available_list))) { gtk_box_remove(GTK_BOX(available_list), child); }
        GtkWidget *label = gtk_label_new("Wi-Fi is turned off");
        gtk_widget_set_vexpand(label, TRUE);
        gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(available_list), label);
        has_recent = FALSE;
        has_available = TRUE; // Show the "Wi-Fi is turned off" message
    }

    gtk_widget_set_visible(widgets->wifi_connected_header, has_recent);
    gtk_widget_set_visible(widgets->wifi_available_header, has_available);
    free_wifi_network_list(networks);
}


static void on_bt_scan_results(GList *devices, gpointer user_data) {
    AppWidgets *widgets = (AppWidgets*)user_data;
    if(!widgets || !widgets->bt_connected_list_box || !widgets->bt_available_list_box) {
        free_bluetooth_device_list(devices);
        return;
    }

    gtk_spinner_stop(GTK_SPINNER(widgets->bt_header_spinner));
    GtkWidget *recent_list = widgets->bt_connected_list_box;
    GtkWidget *available_list = widgets->bt_available_list_box;

    GHashTable *existing_widgets = g_hash_table_new(g_str_hash, g_str_equal);
    for (GtkWidget *child = gtk_widget_get_first_child(recent_list); child != NULL; child = gtk_widget_get_next_sibling(child)) {
        BluetoothDevice *dev = get_bluetooth_device_from_widget(child);
        if (dev && dev->address) g_hash_table_insert(existing_widgets, (gpointer)dev->address, child);
    }
    for (GtkWidget *child = gtk_widget_get_first_child(available_list); child != NULL; child = gtk_widget_get_next_sibling(child)) {
        BluetoothDevice *dev = get_bluetooth_device_from_widget(child);
        if (dev && dev->address) g_hash_table_insert(existing_widgets, (gpointer)dev->address, child);
    }

    GHashTable *seen_addresses = g_hash_table_new(g_str_hash, g_str_equal);
    for (GList *l = devices; l != NULL; l = l->next) {
        BluetoothDevice *dev_from_scan = l->data;
        if (!dev_from_scan || !dev_from_scan->address) continue;

        g_hash_table_insert(seen_addresses, (gpointer)dev_from_scan->address, GINT_TO_POINTER(1));
        GtkWidget *existing_button = g_hash_table_lookup(existing_widgets, dev_from_scan->address);

        if (existing_button) {
            BluetoothDevice *dev_copy = get_bluetooth_device_from_widget(existing_button);
            dev_copy->is_connected = dev_from_scan->is_connected;
            dev_copy->is_paired = dev_from_scan->is_paired;
            
            update_bt_widget_state(existing_button, dev_copy);
            
            GtkWidget *current_parent = gtk_widget_get_parent(existing_button);
            if ((dev_copy->is_connected || dev_copy->is_paired) && current_parent != recent_list) {
                gtk_widget_unparent(existing_button);
                gtk_box_append(GTK_BOX(recent_list), existing_button);
            } else if (!dev_copy->is_connected && !dev_copy->is_paired && current_parent != available_list) {
                gtk_widget_unparent(existing_button);
                gtk_box_append(GTK_BOX(available_list), existing_button);
            }

        } else {
            // --- START OF CRASH FIX ---
            // Instead of a shallow copy, we now explicitly duplicate all members.
            BluetoothDevice *dev_copy = g_new0(BluetoothDevice, 1);
            dev_copy->address = g_strdup(dev_from_scan->address);
            dev_copy->name = g_strdup(dev_from_scan->name);
            dev_copy->object_path = g_strdup(dev_from_scan->object_path); // THIS WAS THE MISSING LINE
            dev_copy->is_connected = dev_from_scan->is_connected;
            dev_copy->is_paired = dev_from_scan->is_paired;
            // --- END OF CRASH FIX ---
            
            GtkWidget *entry_button = gtk_button_new();
            g_object_set_data_full(G_OBJECT(entry_button), "device-data", dev_copy, (GDestroyNotify)bluetooth_device_free);
            gtk_widget_add_css_class(entry_button, "list-item-button");

            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_button_set_child(GTK_BUTTON(entry_button), box);
            
            GtkWidget *icon_stack = gtk_stack_new();
            gtk_widget_set_valign(icon_stack, GTK_ALIGN_CENTER);
            gtk_stack_add_named(GTK_STACK(icon_stack), gtk_image_new_from_icon_name("bluetooth-active-symbolic"), "icon");
            gtk_stack_add_named(GTK_STACK(icon_stack), gtk_spinner_new(), "spinner");
            gtk_stack_set_visible_child_name(GTK_STACK(icon_stack), "icon");
            gtk_box_append(GTK_BOX(box), icon_stack);

            GtkWidget *text_container;
            if (dev_copy->is_connected || dev_copy->is_paired) {
                text_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
                gtk_widget_set_valign(text_container, GTK_ALIGN_CENTER);

                GtkWidget *name_label = gtk_label_new(dev_copy->name);
                gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);

                GtkWidget *sublabel = gtk_label_new("");
                gtk_widget_add_css_class(sublabel, "connected-sublabel");
                gtk_label_set_xalign(GTK_LABEL(sublabel), 0.0);

                gtk_box_append(GTK_BOX(text_container), name_label);
                gtk_box_append(GTK_BOX(text_container), sublabel);
            } else {
                text_container = gtk_label_new(dev_copy->name);
                gtk_widget_set_valign(text_container, GTK_ALIGN_CENTER);
            }
            
            if (GTK_IS_LABEL(text_container)) {
                 gtk_label_set_xalign(GTK_LABEL(text_container), 0.0);
            }
            
            gtk_widget_set_hexpand(text_container, TRUE);
            gtk_box_append(GTK_BOX(box), text_container);
            
            update_bt_widget_state(entry_button, dev_copy);

            g_signal_connect(entry_button, "clicked", G_CALLBACK(on_bluetooth_device_clicked), NULL);

            GtkGesture *right_click = gtk_gesture_click_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
            g_signal_connect(right_click, "pressed", G_CALLBACK(on_bluetooth_right_click), NULL);
            gtk_widget_add_controller(entry_button, GTK_EVENT_CONTROLLER(right_click));

            if (dev_copy->is_connected || dev_copy->is_paired) {
                gtk_box_append(GTK_BOX(recent_list), entry_button);
            } else {
                gtk_box_append(GTK_BOX(available_list), entry_button);
            }
        }
    }

    GList *widgets_to_remove = NULL;
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, existing_widgets);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (!g_hash_table_contains(seen_addresses, key)) {
            widgets_to_remove = g_list_prepend(widgets_to_remove, value);
        }
    }
    for (GList *l = widgets_to_remove; l != NULL; l = l->next) {
        gtk_widget_unparent(l->data);
    }
    g_list_free(widgets_to_remove);
    g_hash_table_destroy(existing_widgets);
    g_hash_table_destroy(seen_addresses);

    gboolean has_recent = (gtk_widget_get_first_child(recent_list) != NULL);
    gboolean has_available = (gtk_widget_get_first_child(available_list) != NULL);
    gtk_widget_set_visible(widgets->bt_connected_header, has_recent);
    gtk_widget_set_visible(widgets->bt_available_header, has_available);

    free_bluetooth_device_list(devices);
}


// --- Page & Section Builders ---
static GtkWidget* create_wifi_page(AppWidgets *widgets) {
    // The main container for the page is a vertical box
    GtkWidget *page_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(GTK_WIDGET(page_vbox), 8);

    // --- MODIFICATION: Changed the header text ---
    widgets->wifi_connected_header = gtk_label_new("Recently connected networks");
    // --- END MODIFICATION ---
    
    gtk_widget_add_css_class(widgets->wifi_connected_header, "bt-header"); // Reuse bt-header style
    gtk_label_set_xalign(GTK_LABEL(widgets->wifi_connected_header), 0.0);
    gtk_widget_set_visible(widgets->wifi_connected_header, FALSE); // Hide initially
    gtk_box_append(GTK_BOX(page_vbox), widgets->wifi_connected_header);

    widgets->wifi_connected_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(page_vbox), widgets->wifi_connected_list_box);

    widgets->wifi_available_header = gtk_label_new("Available networks");
    gtk_widget_add_css_class(widgets->wifi_available_header, "bt-header"); // Reuse bt-header style
    gtk_label_set_xalign(GTK_LABEL(widgets->wifi_available_header), 0.0);
    gtk_widget_set_visible(widgets->wifi_available_header, FALSE); // Hide initially
    gtk_box_append(GTK_BOX(page_vbox), widgets->wifi_available_header);
    
    widgets->wifi_available_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(page_vbox), widgets->wifi_available_list_box);

    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), page_vbox);
    gtk_widget_set_size_request(scrolled_window, -1, LIST_REQUESTED_HEIGHT);
    gtk_widget_set_vexpand(scrolled_window, FALSE);
    gtk_widget_set_valign(scrolled_window, GTK_ALIGN_START);

    GtkWidget *overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), scrolled_window);
    
    return overlay;
}

// --- MODIFIED: Creates the new two-section Bluetooth page ---
static GtkWidget* create_bluetooth_page(AppWidgets *widgets) {
    GtkWidget *page_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(GTK_WIDGET(page_vbox), 8);

    // --- MODIFICATION: Change header text ---
    widgets->bt_connected_header = gtk_label_new("Recently connected devices");
    // --- END MODIFICATION ---
    
    gtk_widget_add_css_class(widgets->bt_connected_header, "bt-header");
    gtk_label_set_xalign(GTK_LABEL(widgets->bt_connected_header), 0.0);
    gtk_widget_set_visible(widgets->bt_connected_header, FALSE);
    gtk_box_append(GTK_BOX(page_vbox), widgets->bt_connected_header);

    widgets->bt_connected_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(page_vbox), widgets->bt_connected_list_box);

    GtkWidget *available_header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    widgets->bt_available_header = available_header_box;
    gtk_widget_add_css_class(widgets->bt_available_header, "bt-header");
    gtk_widget_set_visible(widgets->bt_available_header, FALSE);
    
    GtkWidget* available_label = gtk_label_new("Available devices");
    gtk_widget_set_hexpand(available_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(available_label), 0.0);
    
    widgets->bt_header_spinner = gtk_spinner_new();

    gtk_box_append(GTK_BOX(available_header_box), available_label);
    gtk_box_append(GTK_BOX(available_header_box), widgets->bt_header_spinner);
    gtk_box_append(GTK_BOX(page_vbox), widgets->bt_available_header);
    
    widgets->bt_available_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(page_vbox), widgets->bt_available_list_box);

    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), page_vbox);
    gtk_widget_set_size_request(scrolled_window, -1, LIST_REQUESTED_HEIGHT);
    gtk_widget_set_vexpand(scrolled_window, FALSE);
    gtk_widget_set_valign(scrolled_window, GTK_ALIGN_START);

    GtkWidget *overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), scrolled_window);
    
    widgets->bt_list_spinner = gtk_spinner_new();
    gtk_widget_set_halign(widgets->bt_list_spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(widgets->bt_list_spinner, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), widgets->bt_list_spinner);
    widgets->bt_list_overlay = overlay;

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
    }   else if (toggled_button == GTK_TOGGLE_BUTTON(widgets->bt_toggle)) {
        target_page = "bt_page";
        other_toggle1 = widgets->wifi_toggle;   handler_id1 = widgets->wifi_toggle_handler_id;
        other_toggle2 = widgets->audio_toggle;  handler_id2 = widgets->audio_toggle_handler_id;

        // --- MODIFICATION: Show header and start spinner on open ---
        gtk_widget_set_visible(widgets->bt_available_header, TRUE);
        gtk_spinner_start(GTK_SPINNER(widgets->bt_header_spinner));
        // --- END MODIFICATION ---
        
        bluetooth_scanner_start(widgets->bt_scanner);
    }  else if (toggled_button == GTK_TOGGLE_BUTTON(widgets->audio_toggle)) {
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
    
    AppWidgets *widgets = g_new0(AppWidgets, 1);

    if (!network_manager_init()) {
        g_critical("Control Center Plugin: Failed to initialize NetworkManager.");
        g_free(widgets);
        return NULL;
    }
    
    // Initialize the event-driven Bluetooth manager, passing our UI update callback.
    if (!bluetooth_manager_init(on_bt_devices_updated, widgets)) {
        g_critical("Control Center Plugin: Failed to initialize BluetoothManager.");
        network_manager_shutdown(); // Clean up the already-initialized manager
        g_free(widgets);
        return NULL;
    }
    
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