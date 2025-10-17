#include <gtk/gtk.h>
#include <gio/gio.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include "sysinfo.h"

typedef struct {
    GtkWidget *main_button;
    GtkWidget *content_box;
    GtkWidget *cpu_glyph_label, *cpu_label;
    GtkWidget *ram_glyph_label, *ram_label;
    GtkWidget *temp_glyph_label, *temp_label;
    GtkWidget *battery_glyph_label, *battery_label;
    gulong last_total, last_idle;
    guint poll_timer_id;
    GDBusProxy *battery_proxy;
    guint upower_watcher_id;
    gchar *temp_file_path;
} SysInfoModule;

// --- Forward Declarations ---
static void update_battery_widget(SysInfoModule *module, gdouble percentage, guint state);
static void on_battery_properties_changed(GDBusProxy *proxy, GVariant *changed_properties, const gchar *const *invalidated, gpointer user_data);
static void on_battery_proxy_created(GObject *source_object, GAsyncResult *res, gpointer user_data);
static void on_upower_appeared(GDBusConnection *c, const gchar *n, const gchar *o, gpointer d);
static void on_upower_vanished(GDBusConnection *c, const gchar *n, gpointer d);
static void update_cpu_usage(SysInfoModule *module);
static void update_ram_usage(SysInfoModule *module);
static void find_and_update_temp(SysInfoModule *module);
static gboolean on_poll_timeout(gpointer user_data);
static void sysinfo_module_cleanup(gpointer data);

// =======================================================================
// THE MISSING FUNCTION IS RESTORED HERE
// =======================================================================
static gboolean on_poll_timeout(gpointer user_data) {
    SysInfoModule *module = (SysInfoModule *)user_data;
    update_cpu_usage(module);
    update_ram_usage(module);
    find_and_update_temp(module);
    return G_SOURCE_CONTINUE; // Keep the timer running
}

// (The rest of the file is now correct)

static void update_battery_widget(SysInfoModule *module, gdouble percentage, guint state) { g_autofree gchar *label_text = g_strdup_printf("%.0f%%", percentage); gtk_label_set_text(GTK_LABEL(module->battery_label), label_text); const char *glyph; if (state == 1) { glyph = "󰂄"; } else { if (percentage > 95) glyph = "󰁹"; else if (percentage > 80) glyph = "󰂂"; else if (percentage > 60) glyph = "󰂁"; else if (percentage > 40) glyph = "󰁿"; else if (percentage > 20) glyph = "󰁽"; else glyph = "󰁺"; } gtk_label_set_text(GTK_LABEL(module->battery_glyph_label), glyph); }
static void on_battery_properties_changed(GDBusProxy *proxy, GVariant *changed_properties, const gchar *const *invalidated, gpointer user_data) { (void)changed_properties; (void)invalidated; SysInfoModule *module = (SysInfoModule *)user_data; gdouble percentage = 0.0; guint state = 0; g_autoptr(GVariant) percentage_var = g_dbus_proxy_get_cached_property(proxy, "Percentage"); if (percentage_var) percentage = g_variant_get_double(percentage_var); g_autoptr(GVariant) state_var = g_dbus_proxy_get_cached_property(proxy, "State"); if (state_var) state = g_variant_get_uint32(state_var); update_battery_widget(module, percentage, state); }
static void on_battery_proxy_created(GObject *source_object, GAsyncResult *res, gpointer user_data) { (void)source_object; SysInfoModule *module = (SysInfoModule *)user_data; g_autoptr(GError) error = NULL; module->battery_proxy = g_dbus_proxy_new_for_bus_finish(res, &error); if (module->battery_proxy) { g_signal_connect(module->battery_proxy, "g-properties-changed", G_CALLBACK(on_battery_properties_changed), module); on_battery_properties_changed(module->battery_proxy, NULL, NULL, module); } }
static void on_upower_appeared(GDBusConnection *c, const gchar *n, const gchar *o, gpointer d) { (void)c; (void)n; (void)o; g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.freedesktop.UPower", "/org/freedesktop/UPower/devices/DisplayDevice", "org.freedesktop.UPower.Device", NULL, (GAsyncReadyCallback)on_battery_proxy_created, d); }
static void on_upower_vanished(GDBusConnection *c, const gchar *n, gpointer d) { (void)c; (void)n; SysInfoModule *module = (SysInfoModule *)d; g_clear_object(&module->battery_proxy); gtk_label_set_text(GTK_LABEL(module->battery_label), "N/A"); gtk_label_set_text(GTK_LABEL(module->battery_glyph_label), "?"); }
static void update_cpu_usage(SysInfoModule *module) { FILE* fp = fopen("/proc/stat", "r"); if (!fp) return; gulong user, nice, system, idle, iowait, irq, softirq, steal; if (fscanf(fp, "cpu %lu %lu %lu %lu %lu %lu %lu %lu", &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) < 8) { fclose(fp); return; } fclose(fp); gulong current_idle = idle + iowait; gulong current_total = user + nice + system + current_idle + irq + softirq + steal; if (module->last_total > 0) { gulong total_diff = current_total - module->last_total; gulong idle_diff = current_idle - module->last_idle; double usage = 100.0 * (total_diff - idle_diff) / total_diff; g_autofree gchar *label = g_strdup_printf("%.0f%%", usage); gtk_label_set_text(GTK_LABEL(module->cpu_label), label); } module->last_total = current_total; module->last_idle = current_idle; }
static void update_ram_usage(SysInfoModule *module) { FILE* fp = fopen("/proc/meminfo", "r"); if (!fp) return; long mem_total = 0, mem_available = 0; char line[128]; while (fgets(line, sizeof(line), fp)) { if (sscanf(line, "MemTotal: %ld kB", &mem_total) == 1) {} if (sscanf(line, "MemAvailable: %ld kB", &mem_available) == 1) {} if (mem_total > 0 && mem_available > 0) break; } fclose(fp); if (mem_total > 0) { double usage = 100.0 * (mem_total - mem_available) / mem_total; g_autofree gchar *label = g_strdup_printf("%.0f%%", usage); gtk_label_set_text(GTK_LABEL(module->ram_label), label); } }
static void find_and_update_temp(SysInfoModule *module) { if (module->temp_file_path && g_file_test(module->temp_file_path, G_FILE_TEST_EXISTS)) { FILE* fp = fopen(module->temp_file_path, "r"); if (fp) { long temp_mc = 0; fscanf(fp, "%ld", &temp_mc); fclose(fp); g_autofree gchar *label = g_strdup_printf("%ld°C", temp_mc / 1000); gtk_label_set_text(GTK_LABEL(module->temp_label), label); return; } } g_free(module->temp_file_path); module->temp_file_path = NULL; const char *hwmon_dir_path = "/sys/class/hwmon"; DIR *hwmon_dir = opendir(hwmon_dir_path); if (!hwmon_dir) { gtk_label_set_text(GTK_LABEL(module->temp_label), "N/A"); return; } struct dirent *hwmon_entry; while ((hwmon_entry = readdir(hwmon_dir)) != NULL) { if (g_str_has_prefix(hwmon_entry->d_name, "hwmon")) { g_autofree gchar *mon_path = g_build_filename(hwmon_dir_path, hwmon_entry->d_name, NULL); DIR *mon_dir = opendir(mon_path); if (!mon_dir) continue; struct dirent *sub_entry; while ((sub_entry = readdir(mon_dir)) != NULL) { if (g_str_has_prefix(sub_entry->d_name, "temp") && g_str_has_suffix(sub_entry->d_name, "_input")) { g_autofree gchar *input_path = g_build_filename(mon_path, sub_entry->d_name, NULL); g_autofree gchar *label_path_builder = g_strdup(input_path); gchar *suffix_pos = strstr(label_path_builder, "_input"); if (suffix_pos) { *suffix_pos = '\0'; } g_autofree gchar *label_path = g_strconcat(label_path_builder, "_label", NULL); if (g_file_test(label_path, G_FILE_TEST_EXISTS)) { g_autofree gchar *label_content = NULL; g_file_get_contents(label_path, &label_content, NULL, NULL); if (label_content) { label_content = g_strstrip(label_content); if (g_strcmp0(label_content, "Tdie") == 0 || g_strcmp0(label_content, "Tctl") == 0 || g_strcmp0(label_content, "Package id 0") == 0) { module->temp_file_path = g_strdup(input_path); closedir(mon_dir); closedir(hwmon_dir); find_and_update_temp(module); return; } } } else { module->temp_file_path = g_strdup(input_path); closedir(mon_dir); closedir(hwmon_dir); find_and_update_temp(module); return; } } } closedir(mon_dir); } } closedir(hwmon_dir); gtk_label_set_text(GTK_LABEL(module->temp_label), "N/A"); }
static void sysinfo_module_cleanup(gpointer data) { SysInfoModule *module = (SysInfoModule *)data; if (module->poll_timer_id > 0) g_source_remove(module->poll_timer_id); if (module->upower_watcher_id > 0) g_bus_unwatch_name(module->upower_watcher_id); g_clear_object(&module->battery_proxy); g_free(module->temp_file_path); g_free(module); }
static void create_info_item(GtkWidget *parent_box, const char *glyph_str, const char* placeholder, GtkWidget **glyph_widget, GtkWidget **label_widget) { *glyph_widget = gtk_label_new(glyph_str); gtk_widget_add_css_class(*glyph_widget, "glyph-label"); *label_widget = gtk_label_new(placeholder); gtk_box_append(GTK_BOX(parent_box), *glyph_widget); gtk_box_append(GTK_BOX(parent_box), *label_widget); }

GtkWidget* create_sysinfo_module() {
    SysInfoModule *module = g_new0(SysInfoModule, 1);
    module->main_button = gtk_button_new();
    gtk_widget_add_css_class(module->main_button, "sysinfo-module");
    gtk_widget_add_css_class(module->main_button, "module");
    gtk_widget_add_css_class(module->main_button, "flat");
    module->content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(module->content_box, "content-box");
    gtk_button_set_child(GTK_BUTTON(module->main_button), module->content_box);
    create_info_item(module->content_box, "󰍛", "0%", &module->cpu_glyph_label, &module->cpu_label);
    create_info_item(module->content_box, "󰾆", "0%", &module->ram_glyph_label, &module->ram_label);
    create_info_item(module->content_box, "󰔏", "0°C", &module->temp_glyph_label, &module->temp_label);
    create_info_item(module->content_box, "󰁹", "0%", &module->battery_glyph_label, &module->battery_label);
    module->upower_watcher_id = g_bus_watch_name(G_BUS_TYPE_SYSTEM, "org.freedesktop.UPower", G_BUS_NAME_WATCHER_FLAGS_NONE, on_upower_appeared, on_upower_vanished, module, NULL);
    on_poll_timeout(module);
    module->poll_timer_id = g_timeout_add_seconds(2, on_poll_timeout, module);
    g_object_set_data_full(G_OBJECT(module->main_button), "module-state", module, sysinfo_module_cleanup);
    return module->main_button;
}