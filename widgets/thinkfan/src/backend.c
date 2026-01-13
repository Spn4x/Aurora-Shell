#include "backend.h"
#include <sensors/sensors.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <glib.h> 

#define PROC_FAN "/proc/acpi/ibm/fan"

void backend_init(void) {
    sensors_init(NULL);
}

GList* backend_get_temperatures(void) {
    GList *list = NULL;
    const sensors_chip_name *cn;
    int c = 0;

    while ((cn = sensors_get_detected_chips(NULL, &c))) {
        const sensors_feature *feat;
        int f = 0;

        while ((feat = sensors_get_features(cn, &f))) {
            if (feat->type == SENSORS_FEATURE_TEMP) {
                const sensors_subfeature *sub = sensors_get_subfeature(cn, feat, SENSORS_SUBFEATURE_TEMP_INPUT);
                if (sub) {
                    double val;
                    if (sensors_get_value(cn, sub->number, &val) == 0) {
                        char *label = sensors_get_label(cn, feat);
                        if (!label) label = g_strdup("Temp");

                        SensorData *data = g_new(SensorData, 1);
                        data->label = g_strdup(label); 
                        data->value = val;
                        
                        list = g_list_append(list, data);
                        free(label);
                    }
                }
            }
        }
    }
    return list;
}

char* backend_get_fan_status_raw(void) {
    char *content = NULL;
    GError *error = NULL;
    if (!g_file_get_contents(PROC_FAN, &content, NULL, &error)) {
        if (error) g_error_free(error);
        return NULL;
    }
    return content;
}

gboolean backend_set_fan_level(const char *level) {
    FILE *fp = fopen(PROC_FAN, "w");
    if (!fp) return FALSE;

    fprintf(fp, "level %s", level);
    fclose(fp);
    return TRUE;
}

gboolean backend_check_permissions(void) {
    return access(PROC_FAN, W_OK) == 0;
}

void backend_request_permissions(void) {
    GError *error = NULL;
    
    // Explicit Argument Vector
    // This is safer and more reliable than passing a raw string
    char *argv[] = {
        "pkexec",
        "chmod", // pkexec will find this in PATH
        "666",
        PROC_FAN,
        NULL
    };
    
    g_print("[Backend] Spawning pkexec for chmod 666...\n");
    
    // G_SPAWN_SEARCH_PATH allows finding 'pkexec' and 'chmod' in the user's path.
    // Passing NULL for envp inherits the current environment (DISPLAY, etc).
    gboolean success = g_spawn_async(
        NULL,       // Working directory
        argv,       // Argument vector
        NULL,       // Environment (NULL = inherit)
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, 
        NULL,       // Child setup func
        NULL,       // User data
        NULL,       // Child PID
        &error
    );

    if (!success) {
        g_printerr("[Backend] Failed to spawn pkexec: %s\n", error->message);
        g_error_free(error);
    }
}