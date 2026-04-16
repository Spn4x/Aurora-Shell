#include <gtk/gtk.h>
#include <gio/gio.h>
#include "systray.h"
#include "popover_anim.h" 

#define WATCHER_BUS "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define ITEM_IFACE "org.kde.StatusNotifierItem"
#define MENU_IFACE "com.canonical.dbusmenu"

static const gchar introspection_xml[] =
"<node>"
"  <interface name='org.kde.StatusNotifierWatcher'>"
"    <method name='RegisterStatusNotifierItem'><arg type='s' name='service' direction='in'/></method>"
"    <method name='RegisterStatusNotifierHost'><arg type='s' name='service' direction='in'/></method>"
"    <property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
"    <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
"    <property name='ProtocolVersion' type='i' access='read'/>"
"    <signal name='StatusNotifierItemRegistered'><arg type='s' name='service'/></signal>"
"    <signal name='StatusNotifierItemUnregistered'><arg type='s' name='service'/></signal>"
"  </interface>"
"</node>";

typedef struct {
    GtkWidget *container;
    GDBusConnection *dbus_conn;
    guint watcher_owner_id;
    GHashTable *items; 
} SystrayModule;

typedef struct {
    SystrayModule *module;
    gchar *service;
    gchar *bus_name;
    gchar *object_path;
    gchar *menu_path; 
    GDBusProxy *item_proxy;
    GtkWidget *button;
    GtkWidget *icon;
    GtkWidget *active_popover; 
    guint watch_id;
    guint menu_watch_id;
    GCancellable *cancellable;
} TrayItem;

// --- Forward Declarations ---
static void free_tray_item(gpointer data);
static void on_menu_layout_ready(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_menu_signal(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface, const gchar *signal, GVariant *params, gpointer user_data);

// --- PIXMAP EXTRACTION (Fixes OBS & Flatpak Icons) ---
static GdkPixbuf* extract_pixmap(GVariant *pixmap_var) {
    GdkPixbuf *best_pixbuf = NULL;
    int best_width = 0;

    if (!g_variant_is_of_type(pixmap_var, G_VARIANT_TYPE("a(iiay)"))) return NULL;

    GVariantIter iter;
    g_variant_iter_init(&iter, pixmap_var);
    int width, height;
    GVariant *bytes_var;

    while (g_variant_iter_loop(&iter, "(ii@ay)", &width, &height, &bytes_var)) {
        // Cap size to 128x128 to prevent massive memory usage
        if (width > best_width && width <= 128) {
            gsize n_bytes;
            gconstpointer data = g_variant_get_fixed_array(bytes_var, &n_bytes, sizeof(guint8));
            if (n_bytes == (gsize)(width * height * 4)) {
                guchar *pixels = g_malloc(n_bytes);
                const guchar *src = data;
                for (gsize i = 0; i < n_bytes; i += 4) {
                    // DBus SNI uses ARGB32 network byte order (A, R, G, B)
                    // GdkPixbuf expects RGBA
                    pixels[i+0] = src[i+1]; // R
                    pixels[i+1] = src[i+2]; // G
                    pixels[i+2] = src[i+3]; // B
                    pixels[i+3] = src[i+0]; // A
                }
                if (best_pixbuf) g_object_unref(best_pixbuf);
                best_pixbuf = gdk_pixbuf_new_from_data(pixels, GDK_COLORSPACE_RGB, TRUE, 8, width, height, width * 4, (GdkPixbufDestroyNotify)g_free, NULL);
                best_width = width;
            }
        }
    }
    return best_pixbuf;
}

// --- Dynamic Icon Updater ---
static void apply_icon_name(TrayItem *item, const gchar *icon_name) {
    if (!item || !item->icon) return;

    GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());

    // 1. Try IconName from Theme (Native Apps)
    if (icon_name && strlen(icon_name) > 0) {
        g_autofree gchar *sym_name = g_strdup_printf("%s-symbolic", icon_name);
        if (gtk_icon_theme_has_icon(theme, sym_name)) { 
            gtk_image_set_from_icon_name(GTK_IMAGE(item->icon), sym_name); 
            return;
        } else if (gtk_icon_theme_has_icon(theme, icon_name)) { 
            gtk_image_set_from_icon_name(GTK_IMAGE(item->icon), icon_name); 
            return;
        }
    }

    // 2. Try IconPixmap fallback (Flatpaks / OBS / Steam)
    if (item->item_proxy) {
        g_autoptr(GVariant) pixmap_var = g_dbus_proxy_get_cached_property(item->item_proxy, "IconPixmap");
        if (pixmap_var) {
            GdkPixbuf *pixbuf = extract_pixmap(pixmap_var);
            if (pixbuf) {
                GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pixbuf, 20, 20, GDK_INTERP_BILINEAR);
                GdkTexture *texture = gdk_texture_new_for_pixbuf(scaled);
                gtk_image_set_from_paintable(GTK_IMAGE(item->icon), GDK_PAINTABLE(texture));
                g_object_unref(texture);
                g_object_unref(scaled);
                g_object_unref(pixbuf);
                return;
            }
        }
    }

    // 3. Absolute Last Resort Fallbacks
    gchar *final_icon = "application-x-executable-symbolic";
    if (icon_name) {
        if (g_strrstr(icon_name, "no-connection") || g_strrstr(icon_name, "disconnected")) final_icon = "network-offline-symbolic";
        else if (g_strrstr(icon_name, "wired") || g_strrstr(icon_name, "ethernet")) final_icon = "network-wired-symbolic";
        else if (g_strrstr(item->service, "nm-applet")) final_icon = "network-wireless-symbolic";
    } else {
        g_autoptr(GVariant) id_var = g_dbus_proxy_get_cached_property(item->item_proxy, "Id");
        const gchar *id_str = id_var ? g_variant_get_string(id_var, NULL) : "";
        if (g_strrstr(id_str, "nm-applet") || g_strrstr(item->service, "nm-applet")) final_icon = "network-wireless-symbolic";
        else if (g_strrstr(id_str, "blueman") || g_strrstr(item->service, "blueman")) final_icon = "bluetooth-active-symbolic";
    }
    
    gtk_image_set_from_icon_name(GTK_IMAGE(item->icon), final_icon);
}

static void on_icon_fetched(GObject *source, GAsyncResult *res, gpointer user_data) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return;

    TrayItem *item = (TrayItem*)user_data;
    if (result) {
        g_autoptr(GVariant) val = g_variant_get_child_value(result, 0);
        if (val) {
            g_autoptr(GVariant) inner = g_variant_get_variant(val);
            const gchar *icon_name = g_variant_get_string(inner, NULL);
            apply_icon_name(item, icon_name);
            return;
        }
    }
    g_autoptr(GVariant) cached = g_dbus_proxy_get_cached_property(item->item_proxy, "IconName");
    apply_icon_name(item, cached ? g_variant_get_string(cached, NULL) : NULL);
}

static void update_item_icon(TrayItem *item) {
    if (!item || !item->item_proxy || !item->icon) return;
    
    g_dbus_connection_call(item->module->dbus_conn, item->bus_name, item->object_path,
                           "org.freedesktop.DBus.Properties", "Get",
                           g_variant_new("(ss)", ITEM_IFACE, "IconName"),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, item->cancellable,
                           on_icon_fetched, item);
}

static void on_item_properties_changed(GDBusProxy *proxy, GVariant *changed_properties, const gchar *const *invalidated_properties, gpointer user_data) {
    (void)proxy; (void)invalidated_properties;
    TrayItem *item = (TrayItem*)user_data;
    update_item_icon(item);

    if (changed_properties) {
        g_autoptr(GVariant) menu_v = g_variant_lookup_value(changed_properties, "Menu", G_VARIANT_TYPE_OBJECT_PATH);
        if (menu_v) {
            const gchar *new_menu = g_variant_get_string(menu_v, NULL);
            if (g_strcmp0(item->menu_path, new_menu) != 0) {
                if (item->menu_watch_id > 0) {
                    g_dbus_connection_signal_unsubscribe(item->module->dbus_conn, item->menu_watch_id);
                    item->menu_watch_id = 0;
                }
                g_free(item->menu_path);
                item->menu_path = g_strdup(new_menu);

                if (item->menu_path && strlen(item->menu_path) > 0) {
                    item->menu_watch_id = g_dbus_connection_signal_subscribe(
                        item->module->dbus_conn, item->bus_name, MENU_IFACE, NULL, item->menu_path,
                        NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_menu_signal, item, NULL);
                }
            }
        }
    }
}

static void on_item_signal(GDBusProxy *proxy, const gchar *sender_name, const gchar *signal_name, GVariant *parameters, gpointer user_data) {
    (void)proxy; (void)sender_name; (void)parameters;
    if (g_strcmp0(signal_name, "NewIcon") == 0 || g_strcmp0(signal_name, "NewAttentionIcon") == 0 || g_strcmp0(signal_name, "NewStatus") == 0) {
        update_item_icon((TrayItem*)user_data);
    }
}

// --- Menu Action Handlers ---
static void on_menu_item_clicked(GtkButton *btn, gpointer user_data) {
    TrayItem *item = (TrayItem*)g_object_get_data(G_OBJECT(btn), "tray-item");
    int id = GPOINTER_TO_INT(user_data);
    
    if (!item || !item->menu_path) return;

    g_dbus_connection_call(item->module->dbus_conn, item->bus_name, item->menu_path,
                           MENU_IFACE, "Event", g_variant_new("(isvu)", id, "clicked", g_variant_new_string(""), (guint32)0),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, item->cancellable, NULL, NULL);

    if (item->active_popover) {
        gtk_popover_popdown(GTK_POPOVER(item->active_popover));
    }
}

static void on_submenu_button_clicked(GtkButton *btn, gpointer user_data) {
    TrayItem *item = (TrayItem*)g_object_get_data(G_OBJECT(btn), "tray-item");
    int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "menu-id"));
    GtkWidget *popover = GTK_WIDGET(user_data);

    if (item && item->menu_path) {
        g_dbus_connection_call(item->module->dbus_conn, item->bus_name, item->menu_path,
                               MENU_IFACE, "Event", g_variant_new("(isvu)", id, "opened", g_variant_new_string(""), (guint32)0),
                               NULL, G_DBUS_CALL_FLAGS_NONE, -1, item->cancellable, NULL, NULL);
    }

    gtk_popover_popup(GTK_POPOVER(popover));
}

// --- Recursive Menu Builder ---
static void build_menu_recursive(GVariant *children_list, TrayItem *item, GtkWidget *container_vbox) {
    GVariantIter iter;
    g_variant_iter_init(&iter, children_list);
    GVariant *child_v;

    while (g_variant_iter_loop(&iter, "v", &child_v)) {
        int id;
        GVariantIter *props_iter;
        GVariant *sub_children_v = NULL;

        g_variant_get(child_v, "(ia{sv}@av)", &id, &props_iter, &sub_children_v);

        const char *label = NULL;
        const char *type = "standard";
        const char *toggle_type = NULL;
        const char *children_display = NULL;
        int toggle_state = 0;
        gboolean enabled = TRUE;
        gboolean visible = TRUE;

        const char *prop_key;
        GVariant *prop_val;
        
        while (g_variant_iter_loop(props_iter, "{&sv}", &prop_key, &prop_val)) {
            if (g_strcmp0(prop_key, "label") == 0) label = g_variant_get_string(prop_val, NULL);
            else if (g_strcmp0(prop_key, "type") == 0) type = g_variant_get_string(prop_val, NULL);
            else if (g_strcmp0(prop_key, "enabled") == 0) enabled = g_variant_get_boolean(prop_val);
            else if (g_strcmp0(prop_key, "visible") == 0) visible = g_variant_get_boolean(prop_val);
            else if (g_strcmp0(prop_key, "toggle-type") == 0) toggle_type = g_variant_get_string(prop_val, NULL);
            else if (g_strcmp0(prop_key, "toggle-state") == 0) toggle_state = g_variant_get_int32(prop_val);
            else if (g_strcmp0(prop_key, "children-display") == 0) children_display = g_variant_get_string(prop_val, NULL);
        }
        g_variant_iter_free(props_iter);

        if (!visible) {
            if (sub_children_v) g_variant_unref(sub_children_v);
            continue;
        }

        if (g_strcmp0(type, "separator") == 0) {
            GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
            gtk_widget_set_margin_top(sep, 4);
            gtk_widget_set_margin_bottom(sep, 4);
            
            GtkWidget *revealer = gtk_revealer_new();
            gtk_revealer_set_child(GTK_REVEALER(revealer), sep);
            gtk_box_append(GTK_BOX(container_vbox), revealer);
            
        } else if (label) {
            gchar **parts = g_strsplit(label, "_", -1);
            gchar *clean_label = g_strjoinv("", parts);
            g_strfreev(parts);

            gsize num_children = sub_children_v ? g_variant_n_children(sub_children_v) : 0;
            gboolean is_submenu = (num_children > 0) || (g_strcmp0(children_display, "submenu") == 0);

            GtkWidget *btn = gtk_button_new();
            gtk_widget_add_css_class(btn, "flat");
            gtk_widget_add_css_class(btn, "systray-menu-btn");
            gtk_widget_set_sensitive(btn, enabled);

            GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            
            if (toggle_type && strlen(toggle_type) > 0) {
                GtkWidget *check_icon = gtk_image_new_from_icon_name("object-select-symbolic");
                gtk_image_set_pixel_size(GTK_IMAGE(check_icon), 14);
                gtk_widget_set_opacity(check_icon, (toggle_state == 1) ? 1.0 : 0.0);
                gtk_box_append(GTK_BOX(btn_box), check_icon);
            }

            GtkWidget *lbl = gtk_label_new(clean_label);
            gtk_widget_set_hexpand(lbl, TRUE);
            gtk_widget_set_halign(lbl, GTK_ALIGN_START); 
            gtk_box_append(GTK_BOX(btn_box), lbl);

            if (is_submenu) {
                GtkWidget *arrow = gtk_image_new_from_icon_name("go-next-symbolic");
                gtk_image_set_pixel_size(GTK_IMAGE(arrow), 14);
                gtk_widget_set_halign(arrow, GTK_ALIGN_END);
                gtk_box_append(GTK_BOX(btn_box), arrow);

                GtkWidget *sub_popover = gtk_popover_new();
                gtk_popover_set_position(GTK_POPOVER(sub_popover), GTK_POS_RIGHT);
                gtk_popover_set_has_arrow(GTK_POPOVER(sub_popover), FALSE); 
                
                GtkWidget *sub_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
                gtk_popover_set_child(GTK_POPOVER(sub_popover), sub_vbox);
                
                gtk_widget_set_parent(sub_popover, btn);
                
                attach_popover_animation(GTK_POPOVER(sub_popover), btn);

                if (num_children > 0) {
                    build_menu_recursive(sub_children_v, item, sub_vbox);
                }
                
                reset_popover_animation(GTK_POPOVER(sub_popover));
                
                g_object_set_data(G_OBJECT(btn), "tray-item", item);
                g_object_set_data(G_OBJECT(btn), "menu-id", GINT_TO_POINTER(id));
                g_object_set_data(G_OBJECT(btn), "sub-popover", sub_popover); 
                g_signal_connect(btn, "clicked", G_CALLBACK(on_submenu_button_clicked), sub_popover);
            } else {
                g_object_set_data(G_OBJECT(btn), "tray-item", item);
                g_signal_connect(btn, "clicked", G_CALLBACK(on_menu_item_clicked), GINT_TO_POINTER(id));
            }

            gtk_button_set_child(GTK_BUTTON(btn), btn_box);
            
            GtkWidget *revealer = gtk_revealer_new();
            gtk_revealer_set_child(GTK_REVEALER(revealer), btn);
            gtk_box_append(GTK_BOX(container_vbox), revealer);
            
            g_free(clean_label);
        }
        
        if (sub_children_v) g_variant_unref(sub_children_v);
    }
}

// --- Recursive Sub-Popover Cleanup ---
static void cleanup_sub_popovers(GtkWidget *vbox) {
    if (!vbox) return;
    GtkWidget *child = gtk_widget_get_first_child(vbox);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        
        GtkWidget *target_widget = child;
        if (GTK_IS_REVEALER(child)) {
            target_widget = gtk_revealer_get_child(GTK_REVEALER(child));
        }
        
        if (GTK_IS_BUTTON(target_widget)) {
            GtkWidget *sub_popover = g_object_get_data(G_OBJECT(target_widget), "sub-popover");
            if (sub_popover) {
                GtkWidget *sub_vbox = gtk_popover_get_child(GTK_POPOVER(sub_popover));
                if (sub_vbox) {
                    cleanup_sub_popovers(sub_vbox); 
                }
                gtk_widget_unparent(sub_popover); 
            }
        }
        
        child = next;
    }
}

// --- Menu Builder ---
static void on_menu_layout_ready(GObject *source, GAsyncResult *res, gpointer user_data) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    TrayItem *item = (TrayItem*)user_data;

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return;
    
    // FIX: ContextMenu Fallback for Apps that expose a Menu path but don't populate it (e.g. OBS)
    if (error || !result) {
        g_warning("TrayItem GetLayout failed. Falling back to native ContextMenu.");
        g_dbus_proxy_call(item->item_proxy, "ContextMenu", g_variant_new("(ii)", 0, 0), G_DBUS_CALL_FLAGS_NONE, -1, item->cancellable, NULL, NULL);
        return;
    }

    g_autoptr(GVariant) layout_tuple = g_variant_get_child_value(result, 1);
    if (!layout_tuple || !g_variant_is_of_type(layout_tuple, G_VARIANT_TYPE("(ia{sv}av)"))) return;

    g_autoptr(GVariant) children_list = g_variant_get_child_value(layout_tuple, 2);

    gboolean already_open = FALSE;
    int current_anim_index = -1;
    PopoverAnimState *anim_state = NULL;

    if (!item->active_popover) {
        item->active_popover = gtk_popover_new();
        gtk_widget_set_parent(item->active_popover, item->button);
        attach_popover_animation(GTK_POPOVER(item->active_popover), item->button);
    } else {
        already_open = gtk_widget_get_mapped(item->active_popover);
        anim_state = g_object_get_data(G_OBJECT(item->active_popover), "anim-state");
        if (anim_state && anim_state->animation_id > 0) {
            current_anim_index = g_list_position(anim_state->revealers, anim_state->current_item);
        }
    }

    GtkWidget *old_vbox = gtk_popover_get_child(GTK_POPOVER(item->active_popover));
    if (old_vbox) {
        cleanup_sub_popovers(old_vbox); 
    }

    gtk_popover_set_child(GTK_POPOVER(item->active_popover), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(vbox, 6); gtk_widget_set_margin_end(vbox, 6);
    gtk_widget_set_margin_top(vbox, 6); gtk_widget_set_margin_bottom(vbox, 6);
    gtk_popover_set_child(GTK_POPOVER(item->active_popover), vbox);

    build_menu_recursive(children_list, item, vbox);

    reset_popover_animation(GTK_POPOVER(item->active_popover));

    if (already_open) {
        if (anim_state) {
            if (anim_state->animation_id > 0 && current_anim_index >= 0) {
                GList *l = anim_state->revealers;
                for (int i = 0; i < current_anim_index && l != NULL; i++, l = l->next) {
                    GtkRevealer *rev = GTK_REVEALER(l->data);
                    gtk_revealer_set_transition_duration(rev, 0);
                    gtk_revealer_set_reveal_child(rev, TRUE);
                    gtk_revealer_set_transition_duration(rev, 150);
                }
                anim_state->current_item = l;
            } else {
                for (GList *l = anim_state->revealers; l != NULL; l = l->next) {
                    GtkRevealer *rev = GTK_REVEALER(l->data);
                    gtk_revealer_set_transition_duration(rev, 0); 
                    gtk_revealer_set_reveal_child(rev, TRUE);     
                    gtk_revealer_set_transition_duration(rev, 250); 
                }
            }
        }
    } else {
        gtk_popover_popup(GTK_POPOVER(item->active_popover));
    }
}

static void on_menu_signal(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface, const gchar *signal, GVariant *params, gpointer user_data) {
    (void)conn; (void)sender; (void)path; (void)iface; (void)params;
    TrayItem *item = user_data;

    if (!item->active_popover || !gtk_widget_get_mapped(item->active_popover)) return;

    if (g_strcmp0(signal, "LayoutUpdated") == 0 || g_strcmp0(signal, "ItemsPropertiesUpdated") == 0) {
        g_dbus_connection_call(item->module->dbus_conn, item->bus_name, item->menu_path,
                               MENU_IFACE, "GetLayout", g_variant_new("(iias)", 0, -1, NULL),
                               NULL, G_DBUS_CALL_FLAGS_NONE, -1, item->cancellable, on_menu_layout_ready, item);
    }
}

static void on_about_to_show_ready(GObject *source, GAsyncResult *res, gpointer user_data) {
    g_autoptr(GError) error = NULL;
    g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    TrayItem *item = (TrayItem*)user_data;

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return;

    // FIX: ContextMenu Fallback
    if (error) {
        g_warning("TrayItem AboutToShow failed: %s. Falling back to native ContextMenu.", error->message);
        g_dbus_proxy_call(item->item_proxy, "ContextMenu", g_variant_new("(ii)", 0, 0), G_DBUS_CALL_FLAGS_NONE, -1, item->cancellable, NULL, NULL);
        return;
    }

    g_dbus_connection_call(item->module->dbus_conn, item->bus_name, item->menu_path,
                           MENU_IFACE, "GetLayout", g_variant_new("(iias)", 0, -1, NULL),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, item->cancellable, on_menu_layout_ready, item);
}

// --- Click Handling ---
static void on_click_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; 
    TrayItem *item = (TrayItem*)user_data;
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));

    gboolean is_menu = FALSE;
    g_autoptr(GVariant) is_menu_var = g_dbus_proxy_get_cached_property(item->item_proxy, "ItemIsMenu");
    if (is_menu_var) is_menu = g_variant_get_boolean(is_menu_var);

    if (button == GDK_BUTTON_SECONDARY || (button == GDK_BUTTON_PRIMARY && is_menu)) {
        if (item->menu_path) {
            g_dbus_connection_call(item->module->dbus_conn, item->bus_name, item->menu_path,
                                   MENU_IFACE, "AboutToShow", g_variant_new("(i)", 0),
                                   NULL, G_DBUS_CALL_FLAGS_NONE, -1, item->cancellable, on_about_to_show_ready, item);
        } else {
            g_dbus_proxy_call(item->item_proxy, "ContextMenu", g_variant_new("(ii)", (int)x, (int)y), G_DBUS_CALL_FLAGS_NONE, -1, item->cancellable, NULL, NULL);
        }
    } 
    else if (button == GDK_BUTTON_PRIMARY) {
        g_dbus_proxy_call(item->item_proxy, "Activate", g_variant_new("(ii)", (int)x, (int)y), G_DBUS_CALL_FLAGS_NONE, -1, item->cancellable, NULL, NULL);
    }
}

// --- Item Discovery ---
static void on_item_proxy_ready(GObject *source, GAsyncResult *res, gpointer user_data) {
    (void)source;
    GError *error = NULL;
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(res, &error);

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        if (proxy) g_object_unref(proxy);
        return; 
    }

    TrayItem *item = (TrayItem*)user_data;
    item->item_proxy = proxy;

    if (error) { 
        g_hash_table_remove(item->module->items, item->service); 
        g_error_free(error);
        return; 
    }

    g_autoptr(GVariant) menu_var = g_dbus_proxy_get_cached_property(item->item_proxy, "Menu");
    if (menu_var) {
        item->menu_path = g_variant_dup_string(menu_var, NULL);
        item->menu_watch_id = g_dbus_connection_signal_subscribe(
            item->module->dbus_conn, item->bus_name, MENU_IFACE, NULL, item->menu_path,
            NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_menu_signal, item, NULL);
    }

    g_signal_connect(item->item_proxy, "g-properties-changed", G_CALLBACK(on_item_properties_changed), item);
    g_signal_connect(item->item_proxy, "g-signal", G_CALLBACK(on_item_signal), item);

    item->button = gtk_button_new();
    gtk_widget_add_css_class(item->button, "flat");
    gtk_widget_add_css_class(item->button, "systray-item");
    
    item->icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(item->icon), 16);
    gtk_widget_add_css_class(item->icon, "systray-icon");
    gtk_button_set_child(GTK_BUTTON(item->button), item->icon);
    
    update_item_icon(item);
    
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0); 
    g_signal_connect(click, "pressed", G_CALLBACK(on_click_pressed), item);
    gtk_widget_add_controller(item->button, GTK_EVENT_CONTROLLER(click));
    
    gtk_box_append(GTK_BOX(item->module->container), item->button);
}

// --- FIX: Proper Zombie Cleanup ---
static void on_applet_vanished(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    (void)connection;
    GHashTable *items_table = (GHashTable *)user_data;
    
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, items_table);
    
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        TrayItem *item = (TrayItem *)value;
        // Check against the bus_name, not the full path key
        if (g_strcmp0(item->bus_name, name) == 0) {
            g_hash_table_iter_remove(&iter);
        }
    }
}

// --- Boilerplate D-Bus Server ---
static void handle_method_call(GDBusConnection *c, const gchar *s, const gchar *o, const gchar *i, const gchar *method, GVariant *p, GDBusMethodInvocation *inv, gpointer ud) {
    (void)o; (void)i;
    SystrayModule *module = (SystrayModule*)ud;
    if (g_strcmp0(method, "RegisterStatusNotifierItem") == 0) {
        const gchar *service; g_variant_get(p, "(&s)", &service);
        gchar *full_service = g_str_has_prefix(service, "/") ? g_strdup_printf("%s%s", s, service) : g_strdup(service);
        
        if (!g_hash_table_contains(module->items, full_service)) {
            TrayItem *item = g_new0(TrayItem, 1);
            item->module = module; 
            item->service = g_strdup(full_service);
            item->cancellable = g_cancellable_new(); 
            
            gchar **parts = g_strsplit(full_service, "/", 2);
            item->bus_name = g_strdup(parts[0]);
            item->object_path = parts[1] ? g_strdup_printf("/%s", parts[1]) : g_strdup("/StatusNotifierItem");
            g_strfreev(parts);
            
            item->watch_id = g_bus_watch_name_on_connection(c, item->bus_name, 0, NULL, on_applet_vanished, module->items, NULL);
            g_hash_table_insert(module->items, g_strdup(full_service), item);
            
            g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL, item->bus_name, item->object_path, ITEM_IFACE, item->cancellable, on_item_proxy_ready, item);
        }
        g_free(full_service); 
        g_dbus_method_invocation_return_value(inv, NULL);
    } else {
        g_dbus_method_invocation_return_value(inv, NULL);
    }
}

static GVariant *handle_get_prop(GDBusConnection *c, const gchar *s, const gchar *o, const gchar *i, const gchar *p, GError **e, gpointer ud) {
    (void)c; (void)s; (void)o; (void)i; (void)e; (void)ud;
    if (g_strcmp0(p, "IsStatusNotifierHostRegistered") == 0) return g_variant_new_boolean(TRUE);
    if (g_strcmp0(p, "ProtocolVersion") == 0) return g_variant_new_int32(1);
    return NULL;
}

static void free_tray_item(gpointer data) {
    TrayItem *item = (TrayItem*)data;

    if (item->cancellable) {
        g_cancellable_cancel(item->cancellable);
    }

    if (item->active_popover && gtk_widget_get_parent(item->active_popover)) {
        GtkWidget *old_vbox = gtk_popover_get_child(GTK_POPOVER(item->active_popover));
        if (old_vbox) {
            cleanup_sub_popovers(old_vbox);
        }
        gtk_widget_unparent(item->active_popover);
    }

    if (item->button && gtk_widget_get_parent(item->button)) {
        gtk_box_remove(GTK_BOX(item->module->container), item->button);
    }

    if (item->watch_id > 0) g_bus_unwatch_name(item->watch_id);
    if (item->menu_watch_id > 0 && item->module->dbus_conn) {
        g_dbus_connection_signal_unsubscribe(item->module->dbus_conn, item->menu_watch_id);
    }
    
    if (item->item_proxy) {
        g_signal_handlers_disconnect_by_data(item->item_proxy, item);
        g_clear_object(&item->item_proxy);
    }
    
    g_free(item->menu_path); 
    g_free(item->service); 
    g_free(item->bus_name); 
    g_free(item->object_path); 

    if (item->cancellable) {
        g_object_unref(item->cancellable);
    }

    g_free(item);
}

static void systray_cleanup(gpointer data) {
    SystrayModule *module = (SystrayModule*)data;
    if (module->watcher_owner_id > 0) g_bus_unown_name(module->watcher_owner_id);
    g_hash_table_destroy(module->items);
    g_free(module);
}

GtkWidget* create_systray_module() {
    SystrayModule *module = g_new0(SystrayModule, 1);
    module->items = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_tray_item);
    module->container = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(module->container, "systray-module");
    
    g_object_set_data_full(G_OBJECT(module->container), "state", module, systray_cleanup);
    
    GDBusInterfaceVTable vtable = { .method_call = handle_method_call, .get_property = handle_get_prop, .set_property = NULL };
    
    GError *error = NULL;
    module->dbus_conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (error) {
        g_warning("[Systray] Failed to get DBus connection: %s", error->message);
        g_error_free(error);
        return module->container;
    }

    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
    g_dbus_connection_register_object(module->dbus_conn, WATCHER_PATH, info->interfaces[0], &vtable, module, NULL, NULL);
    g_dbus_node_info_unref(info);
    
    module->watcher_owner_id = g_bus_own_name_on_connection(module->dbus_conn, WATCHER_BUS, 0, NULL, NULL, NULL, NULL);
    
    return module->container;
}