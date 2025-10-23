#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sqlite3.h>
#include <signal.h>
#include <sys/select.h>

static sqlite3* db;
static char current_app_class[256] = {0};
static time_t focus_start_time = 0;

// --- Database Functions ---
int db_init(const char* db_path) {
    if (sqlite3_open(db_path, &db)) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }
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
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return 1;
    }
    printf("Database initialized successfully at %s\n", db_path);
    return 0;
}

void db_log_usage(const char* app_class, long duration) {
    if (duration <= 0 || strlen(app_class) == 0) {
        return;
    }
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
            fprintf(stderr, "Failed to execute statement: %s\n", sqlite3_errmsg(db));
        } else {
            printf("DB_LOG: Added %ld seconds to '%s' for %s\n", duration, app_class, today_str);
            system("touch ~/.local/share/aurora-insight.trigger > /dev/null 2>&1");
        }
        sqlite3_finalize(stmt);
    } else {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
    }
}

// --- Event & Signal Handling ---
void log_current_session() {
    if (focus_start_time > 0 && strlen(current_app_class) > 0) {
        long time_spent = time(NULL) - focus_start_time;
        db_log_usage(current_app_class, time_spent);
    }
    focus_start_time = time(NULL);
}

void handle_exit_signal(int sig) {
    (void)sig;
    printf("\nCaught signal, logging final session and exiting...\n");
    log_current_session();
    sqlite3_close(db);
    exit(0);
}

void process_event(char* event_line) {
    const char* prefix = "activewindow>>";
    if (strncmp(event_line, prefix, strlen(prefix)) != 0) return;
    char* app_data = event_line + strlen(prefix);
    char* comma = strchr(app_data, ',');
    if (!comma) return;
    int class_len = comma - app_data;
    char new_app_class[256];
    strncpy(new_app_class, app_data, class_len);
    new_app_class[class_len] = '\0';
    if (strcmp(current_app_class, new_app_class) != 0) {
        log_current_session();
        strcpy(current_app_class, new_app_class);
        focus_start_time = time(NULL);
        if (strlen(current_app_class) > 0) {
            printf("FOCUS: '%s'\n", current_app_class);
        }
    }
}

// --- Main Loop ---
int main() {
    char db_path[256];
    snprintf(db_path, sizeof(db_path), "%s/.local/share/aurora-insight.db", getenv("HOME"));
    if (db_init(db_path) != 0) return 1;
    signal(SIGINT, handle_exit_signal);
    signal(SIGTERM, handle_exit_signal);
    const char* instance_signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!instance_signature) { fprintf(stderr, "ERROR: HYPRLAND_INSTANCE_SIGNATURE is not set.\n"); return 1; }
    char socket_path[256];
    snprintf(socket_path, sizeof(socket_path), "%s/hypr/%s/.socket2.sock", getenv("XDG_RUNTIME_DIR"), instance_signature);
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd == -1) { perror("socket"); return 1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("connect"); close(sock_fd); return 1;
    }
    printf("Successfully connected! Tracking app focus...\n");
    while (1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock_fd, &fds);
        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        int retval = select(sock_fd + 1, &fds, NULL, NULL, &tv);
        if (retval == -1) {
            perror("select()");
            break;
        } else if (retval > 0) {
            if (FD_ISSET(sock_fd, &fds)) {
                char buffer[4096];
                ssize_t bytes_read = read(sock_fd, buffer, sizeof(buffer) - 1);
                if (bytes_read > 0) {
                    buffer[bytes_read] = '\0';
                    char* line = strtok(buffer, "\n");
                    while (line != NULL) {
                        process_event(line);
                        line = strtok(NULL, "\n");
                    }
                } else {
                    if (bytes_read == 0) printf("Connection closed by peer.\n");
                    else perror("read");
                    break;
                }
            }
        } else {
            printf("PERIODIC LOG: 30s timeout reached.\n");
            log_current_session();
        }
    }
    handle_exit_signal(0);
    return 0;
}