#include <gio/gio.h>
#include <glib.h>
#include <sqlite3.h>
#include <glib/gstdio.h>

const char* UI_BUS_NAME = "com.meismeric.auranotify.UI";
const char* UI_OBJECT_PATH = "/com/meismeric/auranotify/UI";
const char* UI_INTERFACE_NAME = "com.meismeric.auranotify.UI";

const char* CENTER_BUS_NAME = "com.meismeric.auranotify.Center";
const char* CENTER_OBJECT_PATH = "/com/meismeric/auranotify/Center";
const char* CENTER_INTERFACE_NAME = "com.meismeric.auranotify.Center";

static gboolean is_dnd_active = FALSE; 
static gboolean is_center_visible = FALSE;

// FIX: Renamed DB to force a clean schema creation
#define DB_NAME "notifications_v2.db"

static void log_notification_to_db(const char *app, const char *summary, const char *body, const char *icon) {
    sqlite3 *db;
    char *path = g_build_filename(g_get_user_data_dir(), "aurora-shell", DB_NAME, NULL);
    
    g_mkdir_with_parents(g_path_get_dirname(path), 0755);

    if (sqlite3_open(path, &db) == SQLITE_OK) {
        // CLEAN SCHEMA (No virtual columns)
        const char *schema = 
            "CREATE TABLE IF NOT EXISTS history ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "app_name TEXT, summary TEXT, body TEXT, icon TEXT, "
            "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP"
            ");";
        
        char *err_msg = NULL;
        if (sqlite3_exec(db, schema, 0, 0, &err_msg) != SQLITE_OK) {
            g_warning("DB Error (Create): %s", err_msg);
            sqlite3_free(err_msg);
        } else {
            sqlite3_stmt *stmt;
            const char *sql = "INSERT INTO history (app_name, summary, body, icon) VALUES (?, ?, ?, ?)";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, app ? app : "System", -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, summary ? summary : "", -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, body ? body : "", -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, icon ? icon : "", -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_DONE) {
                    g_print("DB Success: Logged '%s'\n", summary);
                } else {
                    g_warning("DB Error (Insert): %s", sqlite3_errmsg(db));
                }
            }
            sqlite3_finalize(stmt);
        }
    }
    sqlite3_close(db);
    g_free(path);
}

// ... (DBus logic remains identical to previous, copy-paste below if needed, otherwise keep existing) ...

// --- DBUS LOGIC START ---
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
    "    <method name='SetCenterVisible'><arg type='b' name='visible' direction='in'/></method>"
    "    <method name='SetDND'><arg type='b' name='active' direction='in'/></method>"
    "    <method name='ToggleDND'></method>"
    "    <method name='GetDNDState'><arg type='b' name='is_active' direction='out'/></method>"
    "    <method name='GetCapabilities'><arg type='as' name='caps' direction='out'/></method>"
    "    <method name='CloseNotification'><arg type='u' name='id' direction='in'/></method>"
    "    <method name='GetServerInformation'><arg type='s' name='name' direction='out'/><arg type='s' name='vendor' direction='out'/><arg type='s' name='version' direction='out'/><arg type='s' name='spec_version' direction='out'/></method>"
    "    <signal name='DNDStateChanged'><arg type='b' name='is_active'/></signal>"
    "  </interface>"
    "</node>";

static void emit_dnd_signal(GDBusConnection *connection) {
    g_dbus_connection_emit_signal(connection, NULL, "/org/freedesktop/Notifications",
                                  "org.freedesktop.Notifications", "DNDStateChanged",
                                  g_variant_new("(b)", is_dnd_active), NULL);
}

static void handle_method_call(GDBusConnection *connection, const gchar *sender, const gchar *object_path, const gchar *interface_name, const gchar *method_name, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data) {
    (void)sender; (void)object_path; (void)interface_name; (void)user_data;
    
    if (g_strcmp0(method_name, "Notify") == 0) {
        gchar *app_name, *app_icon, *summary, *body;
        g_variant_get(parameters, "(su&s&s&sas@a{sv}i)", &app_name, NULL, &app_icon, &summary, &body, NULL, NULL, NULL);

        log_notification_to_db(app_name, summary, body, app_icon);

        if (!is_center_visible && !is_dnd_active) {
            g_dbus_connection_call(connection, UI_BUS_NAME, UI_OBJECT_PATH, UI_INTERFACE_NAME, "ShowNotification", g_variant_new("(sss)", app_icon, summary, body), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        }
        g_dbus_connection_call(connection, CENTER_BUS_NAME, CENTER_OBJECT_PATH, CENTER_INTERFACE_NAME, "AddNotification", g_variant_new("(sss)", app_icon, summary, body), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", g_random_int()));
    } 
    else if (g_strcmp0(method_name, "SetDND") == 0) {
        gboolean active;
        g_variant_get(parameters, "(b)", &active);
        if (is_dnd_active != active) {
            is_dnd_active = active;
            emit_dnd_signal(connection);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    else if (g_strcmp0(method_name, "ToggleDND") == 0) {
        is_dnd_active = !is_dnd_active;
        emit_dnd_signal(connection);
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    else if (g_strcmp0(method_name, "GetDNDState") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", is_dnd_active));
    }
    else if (g_strcmp0(method_name, "SetCenterVisible") == 0) {
        g_variant_get(parameters, "(b)", &is_center_visible);
        g_dbus_method_invocation_return_value(invocation, NULL);
    } 
    else if (g_strcmp0(method_name, "GetCapabilities") == 0) {
        const char *capabilities[] = { "body", NULL };
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(^as)", capabilities));
    }
    else if (g_strcmp0(method_name, "GetServerInformation") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(ssss)", "aurora-notify", "meismeric", "1.1", "1.2"));
    } else {
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
}

static const GDBusInterfaceVTable interface_vtable = { .method_call = handle_method_call };

static void on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    (void)name; (void)user_data;
    GDBusNodeInfo *node_info = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
    g_dbus_connection_register_object(connection, "/org/freedesktop/Notifications", node_info->interfaces[0], &interface_vtable, NULL, NULL, NULL);
    g_dbus_node_info_unref(node_info);
    g_print("Daemon: Service running with V2 Schema (notifications_v2.db).\n");
}

int main(void) {
    g_bus_own_name(G_BUS_TYPE_SESSION, "org.freedesktop.Notifications", G_BUS_NAME_OWNER_FLAGS_NONE, on_bus_acquired, NULL, NULL, NULL, NULL);
    g_main_loop_run(g_main_loop_new(NULL, FALSE));
    return 0;
}