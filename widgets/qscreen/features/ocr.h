#ifndef QSCREEN_OCR_H
#define QSCREEN_OCR_H

#include <gdk/gdk.h>
#include <glib.h>

// A struct to hold a single word found by OCR and its location
typedef struct {
    GdkRectangle geometry;
    char *text;
} QScreenTextBox;

// Define a callback function type that will receive the results
typedef void (*OcrResultFunc)(GList *text_boxes, gpointer user_data);

// The main function that runs Tesseract and returns a list of text boxes
GList* run_ocr_on_screenshot(const char *image_path);

// The new asynchronous version of the function
void run_ocr_on_screenshot_async(const char *image_path, OcrResultFunc callback, gpointer user_data);

// A helper to free the memory used by our text box list
void free_ocr_results(GList *text_boxes);

#endif // QSCREEN_OCR_H