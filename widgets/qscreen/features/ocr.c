#include "ocr.h"
#include <stdlib.h>
#include <string.h>
#include <gio/gio.h> // For GTask

GList* run_ocr_on_screenshot(const char *image_path) {
    // Use Page Segmentation Mode 11: "Find as much text as possible in no particular order."
    g_autofree char *command = g_strdup_printf("tesseract \"%s\" stdout -l eng --psm 11 tsv", image_path);
    g_autofree gchar *tsv_output = NULL;
    g_autoptr(GError) error = NULL;

    g_spawn_command_line_sync(command, &tsv_output, NULL, NULL, &error);

    if (error) {
        g_warning("Failed to run Tesseract: %s", error->message);
        g_warning("Please ensure 'tesseract' and 'tesseract-data-eng' are installed.");
        return NULL;
    }

    GList *text_boxes = NULL;
    char **lines = g_strsplit(tsv_output, "\n", -1);
    
    for (int i = 1; lines[i] != NULL; i++) {
        char **fields = g_strsplit(lines[i], "\t", -1);
        
        if (g_strv_length(fields) == 12 && atoi(fields[0]) == 5 && g_strcmp0(g_strstrip(fields[11]), "") != 0) {
            int conf = atoi(fields[10]);
            if (conf > 50) {
                QScreenTextBox *box = g_new(QScreenTextBox, 1);
                box->geometry.x = atoi(fields[6]);
                box->geometry.y = atoi(fields[7]);
                box->geometry.width = atoi(fields[8]);
                box->geometry.height = atoi(fields[9]);
                box->text = g_strdup(fields[11]);
                text_boxes = g_list_prepend(text_boxes, box);
            }
        }
        g_strfreev(fields);
    }
    g_strfreev(lines);

    return g_list_reverse(text_boxes);
}

// Helper function to free a QScreenTextBox struct and its contents
static void qscreen_text_box_free(gpointer data) {
    QScreenTextBox *box = data;
    if (!box) return;
    g_free(box->text);
    g_free(box);
}

// Correctly frees all memory associated with the OCR results
void free_ocr_results(GList *text_boxes) {
    if (!text_boxes) return;
    g_list_free_full(text_boxes, qscreen_text_box_free);
}


// --- START: NEW ASYNCHRONOUS IMPLEMENTATION ---

// This is the data we'll pass to our background thread.
typedef struct {
    char *path;
    OcrResultFunc callback;
    gpointer user_data;
} OcrTaskData;

// This function runs in the background thread.
static void ocr_thread_func(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object; (void)cancellable;
    OcrTaskData *data = task_data;

    // We simply call our original, blocking function here.
    // The GTask runs this in a separate thread, so the UI doesn't freeze.
    GList *result = run_ocr_on_screenshot(data->path);

    // This sends the result back to the main thread.
    g_task_return_pointer(task, result, (GDestroyNotify)free_ocr_results);
}

// This function is called in the main thread after the background thread is finished.
static void on_ocr_task_complete(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object;
    OcrTaskData *data = user_data;
    
    GList *text_boxes = g_task_propagate_pointer(G_TASK(res), NULL);

    // Call the user's original callback with the result.
    data->callback(text_boxes, data->user_data);

    // Clean up.
    g_free(data->path);
    g_free(data);
}

// This is the public function that starts the whole process.
void run_ocr_on_screenshot_async(const char *image_path, OcrResultFunc callback, gpointer user_data) {
    OcrTaskData *data = g_new0(OcrTaskData, 1);
    data->path = g_strdup(image_path);
    data->callback = callback;
    data->user_data = user_data;

    GTask *task = g_task_new(NULL, NULL, on_ocr_task_complete, data);
    g_task_set_task_data(task, data, NULL);
    g_task_run_in_thread(task, ocr_thread_func);
    g_object_unref(task);
}
// --- END: NEW ASYNCHRONOUS IMPLEMENTATION ---