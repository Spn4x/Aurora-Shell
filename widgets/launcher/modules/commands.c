#include <glib.h>
#include "commands.h"
#include "../launcher.h"

// Define your preferred terminal here
#define TERMINAL_COMMAND "kitty"

GList* get_command_results(const gchar *search_text) {
    if (!g_str_has_prefix(search_text, "> ")) {
        return NULL;
    }

    const gchar *command = search_text + 2; // Skip the "> "
    if (*command == '\0') {
        return NULL; // Don't show a result for an empty command
    }

    g_autofree gchar *description = g_strdup_printf("Run '%s' in terminal", command);
    GList *results = NULL;

    // The 'data' for this result is the command itself.
    results = g_list_prepend(results, aurora_result_object_new(
        AURORA_RESULT_COMMAND,
        command,
        description,
        "utilities-terminal-symbolic",
        g_strdup(command),
        g_free
    ));

    return results;
}