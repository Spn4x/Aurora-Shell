#include <gio/gio.h>
#include <glib.h>

const char* UI_BUS_NAME = "com.meismeric.auranotify.UI";
const char* UI_OBJECT_PATH = "/com/meismeric/auranotify/UI";
const char* CENTER_BUS_NAME = "com.meismeric.auranotify.Center";
const char* CENTER_OBJECT_PATH = "/com/meismeric/auranotify/Center";

static gboolean is_center_visible = FALSE;
static gboolean is_dnd_active = FALSE; 

static const gchar introspection_xml[] =
    "<node>"
    "  <interface name='org.freedesktop.Notifications'>"
    "    <method name='Notify'>"
    "      <arg type='s' name='app_name' direction='in'/>"
    "      <arg type='u' name='replaces_id' direction='in'/>"
    "      <arg type='s' name='app_icon' direction='in'/>"
    "      <arg type='s' name='summary' direction='in'/>"
    "      <arg type='s' name='body' direction='in'/>"
    "      <arg type='as' name='actions' direction='in'/>"
    "      <arg type='a{sv}' name='hints' direction='in'/>"
    "      <arg type='i' name='expire_timeout' direction='in'/>"
    "      <arg type='u' name='id' direction='out'/>"
    "    </method>"
    "    <method name='SetCenterVisible'>"
    "      <arg type='b' name='visible' direction='in'/>"
    "    </method>"
    "    <method name='SetDND'>"
    "      <arg type='b' name='active' direction='in'/>"
    "    </method>"
    "    <method name='ToggleDND'></method>"
    "    <method name='GetCapabilities'><arg type='as' name='caps' direction='out'/></method>"
    "    <method name='CloseNotification'><arg type='u' name='id' direction='in'/></method>"
    "    <method name='GetServerInformation'><arg type='s' name='name' direction='out'/><arg type='s' name='vendor' direction='out'/><arg type='s' name='version' direction='out'/><arg type='s' name='spec_version' direction='out'/></method>"
    "    <property name='DND' type='b' access='readwrite'/>"
    "  </interface>"
    "</node>";

static void emit_dnd_state_changed(GDBusConnection *connection, gboolean state) {
    g_dbus_connection_emit_signal(
        connection, NULL, "/org/freedesktop/Notifications",
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        g_variant_new("(sa{sv}as)", "org.freedesktop.Notifications",
                      g_variant_new_parsed("{'DND': <%b>}", state),
                      (const gchar* const[]){ NULL }),
        NULL);
    g_print("Daemon: Emitted DND state change signal: %s\n", state ? "ON" : "OFF");
}

static void handle_method_call(GDBusConnection *connection, const gchar *sender G_GNUC_UNUSED,
                               const gchar *object_path G_GNUC_UNUSED, const gchar *interface_name,
                               const gchar *method_name, GVariant *parameters,
                               GDBusMethodInvocation *invocation, gpointer user_data G_GNUC_UNUSED) {
    
    if (g_strcmp0(interface_name, "org.freedesktop.DBus.Properties") == 0) {
        if (g_strcmp0(method_name, "Get") == 0) {
            gchar *iface, *prop;
            g_variant_get(parameters, "(&s&s)", &iface, &prop);
            if (g_strcmp0(iface, "org.freedesktop.Notifications") == 0 && g_strcmp0(prop, "DND") == 0) {
                g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", g_variant_new_boolean(is_dnd_active)));
                return;
            }
        }
        if (g_strcmp0(method_name, "Set") == 0) {
            gchar *iface, *prop;
            g_autoptr(GVariant) value_variant = NULL;
            g_variant_get(parameters, "(&s&sv)", &iface, &prop, &value_variant);
            if (g_strcmp0(iface, "org.freedesktop.Notifications") == 0 && g_strcmp0(prop, "DND") == 0) {
                gboolean new_state = g_variant_get_boolean(value_variant);
                 if (is_dnd_active != new_state) {
                    is_dnd_active = new_state;
                    g_print("Daemon: Do Not Disturb set via property to: %s\n", is_dnd_active ? "ON" : "OFF");
                    emit_dnd_state_changed(connection, is_dnd_active);
                }
                g_dbus_method_invocation_return_value(invocation, NULL);
                return;
            }
        }
    }
    
    if (g_strcmp0(method_name, "Notify") == 0) {
        gchar *app_name, *app_icon, *summary, *body;
        g_variant_get(parameters, "(&su&s&s&sas@a{sv}i)", &app_name, NULL, &app_icon, &summary, &body, NULL, NULL, NULL);

        if (!is_center_visible && !is_dnd_active) {
            g_dbus_connection_call(connection, UI_BUS_NAME, UI_OBJECT_PATH, UI_BUS_NAME,
                                   "ShowNotification", g_variant_new("(sss)", app_icon, summary, body),
                                   NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        }

        g_dbus_connection_call(connection, CENTER_BUS_NAME, CENTER_OBJECT_PATH, CENTER_BUS_NAME,
                               "AddNotification", g_variant_new("(sss)", app_icon, summary, body),
                               NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);

        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", 1234));
    } else if (g_strcmp0(method_name, "SetCenterVisible") == 0) {
        gboolean visible;
        g_variant_get(parameters, "(b)", &visible);
        is_center_visible = visible;
        g_dbus_method_invocation_return_value(invocation, NULL);
    } 
    else if (g_strcmp0(method_name, "SetDND") == 0) {
        gboolean active;
        g_variant_get(parameters, "(b)", &active);
        if (is_dnd_active != active) {
            is_dnd_active = active;
            g_print("Daemon: Do Not Disturb set to: %s\n", is_dnd_active ? "ON" : "OFF");
            emit_dnd_state_changed(connection, is_dnd_active);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    else if (g_strcmp0(method_name, "ToggleDND") == 0) {
        is_dnd_active = !is_dnd_active;
        g_print("Daemon: Do Not Disturb toggled to: %s\n", is_dnd_active ? "ON" : "OFF");
        emit_dnd_state_changed(connection, is_dnd_active);
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    else if (g_strcmp0(method_name, "GetCapabilities") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(as)", (const gchar*[]){"body", "dbus-properties", NULL}));
    } else if (g_strcmp0(method_name, "GetServerInformation") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(ssss)", "aurora-notify", "meismeric", "1.1", "1.2"));
    } else {
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
}

static const GDBusInterfaceVTable interface_vtable = { .method_call = handle_method_call };

static void on_bus_acquired(GDBusConnection *connection, const gchar *name G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED) {
    GDBusNodeInfo *node_info = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
    g_dbus_connection_register_object(connection, "/org/freedesktop/Notifications",
                                      node_info->interfaces[0], &interface_vtable,
                                      NULL, NULL, NULL);
    g_dbus_node_info_unref(node_info);
    g_print("Daemon: Service is running.\n");
}

int main(void) {
    g_bus_own_name(G_BUS_TYPE_SESSION, "org.freedesktop.Notifications", G_BUS_NAME_OWNER_FLAGS_NONE,
                   on_bus_acquired, NULL, NULL, NULL, NULL);
    g_main_loop_run(g_main_loop_new(NULL, FALSE));
    return 0;
}