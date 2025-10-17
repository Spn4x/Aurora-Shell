#ifndef MODULES_CALCULATOR_H
#define MODULES_CALCULATOR_H

#include <glib.h>

// This function will be called to get calculator results.
GList* get_calculator_results(const gchar *query);

#endif // MODULES_CALCULATOR_H