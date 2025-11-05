#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "calculator.h"
// This include is necessary to find the definition for AuroraResultObject
// and its constructor, aurora_result_object_new.
#include "../launcher.h"

// A simple check to see if a string looks like a math expression.
// This prevents us from running the `bc` command on every single keypress.
static gboolean is_potential_math(const gchar *str) {
    if (!str || strlen(str) < 2) return FALSE;

    gboolean has_digit = FALSE;
    gboolean has_operator = FALSE;
    for (int i = 0; str[i] != '\0'; i++) {
        if (isdigit(str[i])) {
            has_digit = TRUE;
        }
        if (strchr("+-*/()^.", str[i])) { // Added '.' for decimals
            has_operator = TRUE;
        }
    }
    return has_digit && has_operator;
}

// The public function that the main launcher calls.
GList* get_calculator_results(const gchar *query) {
    if (!is_potential_math(query)) {
        return NULL;
    }

    // Use `bc -l`. The `-l` flag loads the standard math library,
    // which gives us access to functions like sqrt(), sin(), cos(), etc.
    // 'scale=4' sets the number of decimal places in the result.
    g_autofree gchar *command = g_strdup_printf("echo 'scale=4; %s' | bc -l", query);
    
    // popen is a standard C library function that runs a command
    // and lets us read its standard output like a file.
    FILE *pipe = popen(command, "r");
    if (!pipe) {
        return NULL;
    }

    char buffer[128];
    char *result_str = fgets(buffer, sizeof(buffer), pipe);
    pclose(pipe);

    // If fgets returns NULL or an empty string, there was no valid output.
    if (!result_str || strlen(result_str) <= 1) { // Check for empty or newline-only result
        return NULL;
    }

    // `bc`'s output includes a newline character (\n) at the end.
    // This line finds that newline and replaces it with a null terminator (\0).
    result_str[strcspn(result_str, "\n")] = 0;
    
    // Sometimes `bc` will just echo our input back if the expression isn't
    // complete yet (e.g., for "5*"). We don't want to show that as a result.
    if (g_strcmp0(result_str, query) == 0) {
        return NULL;
    }

    GList *results_list = NULL;
    g_autofree gchar *description = g_strdup_printf("Copy '%s' to clipboard", result_str);

    results_list = g_list_prepend(results_list, aurora_result_object_new(
        AURORA_RESULT_CALCULATOR,
        result_str,
        description,
        "accessories-calculator-symbolic", // A more standard icon name
        g_strdup(result_str),              // The data is the result string itself
        g_free,                            // Provide the function to free the data
        120                                // <<< FIX IS HERE: Added the score parameter
    ));

    return results_list;
}