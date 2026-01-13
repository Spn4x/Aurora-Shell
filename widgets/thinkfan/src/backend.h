#pragma once
#include <glib.h>

typedef struct {
    char *label;
    double value;
} SensorData;

void backend_init(void);
void backend_cleanup(void);
GList* backend_get_temperatures(void);
char* backend_get_fan_status_raw(void);
gboolean backend_set_fan_level(const char *level);
gboolean backend_check_permissions(void);
void backend_request_permissions(void);