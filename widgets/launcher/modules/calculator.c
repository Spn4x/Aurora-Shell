// widgets/launcher/modules/calculator.c
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "calculator.h"
#include "../launcher.h"

// Strict check: Only allow digits, operators, and spaces. 
// If there is ANY letter, return FALSE immediately.
static gboolean is_strictly_math(const gchar *str) {
    if (!str || strlen(str) < 3) return FALSE; // Don't calc "1+1" until length 3

    gboolean has_digit = FALSE;
    gboolean has_operator = FALSE;
    
    for (int i = 0; str[i] != '\0'; i++) {
        char c = str[i];
        if (isdigit(c)) {
            has_digit = TRUE;
        } else if (strchr("+-*/^%(). ", c)) {
            if (strchr("+-*/^%", c)) has_operator = TRUE;
        } else {
            // Found a letter or unknown symbol -> NOT MATH
            return FALSE; 
        }
    }
    return has_digit && has_operator;
}

GList* get_calculator_results(const gchar *query) {
    // Optimization: Don't spawn process unless it's strictly math
    if (!is_strictly_math(query)) {
        return NULL;
    }

    // Optimization: Add "quit" to echo to force bc to exit immediately
    g_autofree gchar *command = g_strdup_printf("echo '%s' | bc -l", query);
    
    FILE *pipe = popen(command, "r");
    if (!pipe) return NULL;

    char buffer[128];
    char *result_str = fgets(buffer, sizeof(buffer), pipe);
    pclose(pipe);

    if (!result_str || strlen(result_str) <= 1) return NULL;

    result_str[strcspn(result_str, "\n")] = 0;
    
    // Clean up trailing zeros for floats (e.g. 4.5000 -> 4.5)
    if (strchr(result_str, '.')) {
        int len = strlen(result_str);
        while (len > 0 && result_str[len-1] == '0') {
            result_str[len-1] = '\0';
            len--;
        }
        if (len > 0 && result_str[len-1] == '.') {
            result_str[len-1] = '\0';
        }
    }

    if (g_strcmp0(result_str, query) == 0) return NULL;

    GList *results_list = NULL;
    g_autofree gchar *description = g_strdup_printf("Result: %s", result_str);

    results_list = g_list_prepend(results_list, aurora_result_object_new(
        AURORA_RESULT_CALCULATOR,
        result_str,
        description,
        "accessories-calculator-symbolic",
        g_strdup(result_str),
        g_free,
        120 
    ));

    return results_list;
}