#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

static sqlite3* db;
static char current_app_class[256] = {0};
static time_t focus_start_time = 0;
static char trigger_path[256]; // Global path for the trigger file

// --- Trigger UI Update ---
static void trigger_ui_refresh() {
    // Touching the file natively in C is 100x more reliable than spawning a shell command
    FILE *tf = fopen(trigger_path, "w");
    if (tf) {
        fprintf(tf, "update\n");
        fclose(tf);
    }
}

// --- Database Functions ---
int db_init(const char* db_path) {
    if (sqlite3_open(db_path, &db)) {
        g_printerr("[Insight] ERROR: Can't open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    
    // THE FIX: Enable WAL mode so the UI can read the DB while the daemon writes to it
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", 0, 0, 0);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", 0, 0, 0);

    const char* sql_create_table =
        "CREATE TABLE IF NOT EXISTS app_usage ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  app_class TEXT NOT NULL,"
        "  date TEXT NOT NULL,"
        "  usage_seconds INTEGER NOT NULL,"
        "  UNIQUE(app_class, date)"
        ");";
        
    char* err_msg = 0;
    if (sqlite3_exec(db, sql_create_table, 0, 0, &err_msg) != SQLITE_OK) {
        g_printerr("[Insight] ERROR: SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return 1;
    }
    g_print("[Insight] Database initialized successfully at %s\n", db_path);
    return 0;
}

void db_log_usage(const char* app_class, long duration) {
    if (duration <= 0 || strlen(app_class) == 0) return;

    char today_str[11];
    time_t now = time(NULL);
    strftime(today_str, sizeof(today_str), "%Y-%m-%d", localtime(&now));
    
    const char* sql_upsert =
        "INSERT INTO app_usage (app_class, date, usage_seconds) VALUES (?, ?, ?)"
        "ON CONFLICT(app_class, date) DO UPDATE SET usage_seconds = usage_seconds + excluded.usage_seconds;";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql_upsert, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, app_class, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, today_str, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, duration);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            g_printerr("[Insight] ERROR: Failed to execute statement: %s\n", sqlite3_errmsg(db));
        } else {
            g_print("[Insight] DB_LOG: Added %ld seconds to '%s' for %s\n", duration, app_class, today_str);
            trigger_ui_refresh(); // Call our native C touch function
        }
        sqlite3_finalize(stmt);
    }
}

// --- Session Tracking ---
void log_current_session() {
    if (focus_start_time > 0 && strlen(current_app_class) > 0) {
        long time_spent = time(NULL) - focus_start_time;
        
        if (time_spent > 60) {
            time_spent = 30; // Cap ghost time
        }
        db_log_usage(current_app_class, time_spent);
    }
    focus_start_time = time(NULL);
}

static gboolean on_heartbeat(gpointer data) {
    (void)data;
    log_current_session();
    return G_SOURCE_CONTINUE;
}

static void handle_exit_signal(int sig) {
    (void)sig;
    g_print("\n[Insight] Caught exit signal, logging final session and exiting...\n");
    log_current_session();
    sqlite3_close(db);
    exit(0);
}

// --- Niri API Helper ---
static char* get_focused_app_id() {
    gchar *stdout_buf = NULL;
    if (!g_spawn_command_line_sync("niri msg -j focused-window", &stdout_buf, NULL, NULL, NULL)) {
        return NULL;
    }
    
    char *result = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    if (json_parser_load_from_data(parser, stdout_buf, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);
            const char *app_id = json_object_get_string_member_with_default(obj, "app_id", NULL);
            if (app_id) {
                result = g_strdup(app_id);
            }
        }
    }
    g_free(stdout_buf);
    return result;
}

// --- Niri JSON Event Parser ---
static void process_niri_event(const char* json_line) {
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_line, -1, NULL)) return;

    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) return;
    JsonObject *root_obj = json_node_get_object(root);

    if (json_object_has_member(root_obj, "WindowFocusChanged")) {
        JsonObject *focus_event = json_object_get_object_member(root_obj, "WindowFocusChanged");
        JsonNode *id_node = json_object_get_member(focus_event, "id");
        
        const char *new_app_class = "";
        g_autofree char *fetched_app_id = NULL;

        if (id_node != NULL && !json_node_is_null(id_node)) {
            fetched_app_id = get_focused_app_id();
            if (fetched_app_id) new_app_class = fetched_app_id;
            else new_app_class = "Unknown";
        }

        if (g_strcmp0(current_app_class, new_app_class) != 0) {
            log_current_session();
            strncpy(current_app_class, new_app_class, sizeof(current_app_class) - 1);
            focus_start_time = time(NULL);
        }
    }
}

// --- Async Stream Reader ---
static void on_line_read(GObject *source, GAsyncResult *res, gpointer user_data) {
    GDataInputStream *stream = G_DATA_INPUT_STREAM(source);
    g_autoptr(GError) error = NULL;
    gsize length;
    
    char *line = g_data_input_stream_read_line_finish(stream, res, &length, &error);

    if (line) {
        process_niri_event(line);
        g_free(line);
        g_data_input_stream_read_line_async(stream, G_PRIORITY_DEFAULT, NULL, on_line_read, user_data);
    } else {
        g_main_loop_quit((GMainLoop*)user_data);
    }
}

int main() {
    g_print("[Insight] Starting Aurora Insight Daemon for Niri...\n");

    // Establish paths
    snprintf(trigger_path, sizeof(trigger_path), "%s/.local/share/aurora-insight.trigger", g_getenv("HOME"));
    char db_path[256];
    snprintf(db_path, sizeof(db_path), "%s/.local/share/aurora-insight.db", g_getenv("HOME"));
    
    // Ensure trigger file exists on boot so the GTK widget can attach to it
    trigger_ui_refresh();

    if (db_init(db_path) != 0) return 1;

    signal(SIGINT, handle_exit_signal);
    signal(SIGTERM, handle_exit_signal);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    g_autoptr(GError) error = NULL;
    GSubprocess *proc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error, 
        "niri", "msg", "--json", "event-stream", NULL
    );

    if (!proc) {
        g_printerr("[Insight] FATAL: Failed to start Niri event stream: %s\n", error->message);
        return 1;
    }

    GInputStream *stdout_stream = g_subprocess_get_stdout_pipe(proc);
    GDataInputStream *data_stream = g_data_input_stream_new(stdout_stream);

    // Start Async Read Loop
    g_data_input_stream_read_line_async(data_stream, G_PRIORITY_DEFAULT, NULL, on_line_read, loop);

    // Heartbeat every 30 seconds
    g_timeout_add_seconds(30, on_heartbeat, NULL);

    g_main_loop_run(loop);

    handle_exit_signal(0);
    return 0;
}