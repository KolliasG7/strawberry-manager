#pragma once

#include "skm-service.h"

#include <gtk/gtk.h>

typedef struct _SkmAppWindow SkmAppWindow;

typedef enum {
  SKM_DIRTY_NONE,
  SKM_DIRTY_FAN,
  SKM_DIRTY_LED,
  SKM_DIRTY_GPU,
} SkmDirtyKind;

struct _SkmAppWindow {
  GtkApplicationWindow *window;
  SkmService *service;
  GCancellable *cancellable;

  guint poll_source_id;
  guint fan_debounce_source_id;
  guint notice_source_id;

  gboolean refresh_in_flight;
  gboolean fan_syncing;
  gboolean led_syncing;
  gboolean gpu_syncing;
  gboolean fan_dirty;
  gboolean led_dirty;
  gboolean gpu_dirty;
  gboolean gpu_selected_valid;

  gint gpu_selected_index;
  GDateTime *last_reprobe_at;

  GtkWidget *notice_revealer;
  GtkWidget *notice_box;
  GtkWidget *notice_label;

  GtkWidget *system_kernel_value;
  GtkWidget *system_variant_value;
  GtkWidget *system_governor_value;
  GtkWidget *system_uptime_value;

  GtkWidget *fan_content;
  GtkWidget *fan_unavailable_label;
  GtkWidget *fan_temp_value;
  GtkWidget *fan_rpm_value;
  GtkWidget *fan_threshold_scale;
  GtkWidget *fan_threshold_value;
  GtkWidget *fan_status_label;
  GtkWidget *fan_apply_button;
  GtkWidget *fan_reset_button;

  GtkWidget *led_content;
  GtkWidget *led_unavailable_label;
  GtkWidget *led_mode_value;
  GtkWidget *led_active_value;
  GtkWidget *led_effect_dropdown;
  GtkWidget *led_thermal_row;
  GtkWidget *led_thermal_switch;
  GtkWidget *led_interval_row;
  GtkWidget *led_interval_spin;
  GtkWidget *led_status_label;
  GtkWidget *led_apply_button;
  GtkWidget *led_reset_button;

  GtkWidget *gpu_content;
  GtkWidget *gpu_unavailable_label;
  GtkWidget *gpu_warning_label;
  GtkWidget *gpu_variant_value;
  GtkWidget *gpu_mode_value;
  GtkWidget *gpu_active_value;
  GtkWidget *gpu_manual_switch;
  GtkWidget *gpu_levels_box;
  GtkWidget *gpu_apply_button;
  GtkWidget *gpu_auto_button;
  GtkWidget *gpu_reset_button;

  GtkWidget *hdmi_content;
  GtkWidget *hdmi_unavailable_label;
  GtkWidget *hdmi_connector_value;
  GtkWidget *hdmi_status_value;
  GtkWidget *hdmi_last_reprobe_value;
  GtkWidget *hdmi_reprobe_button;
};

GtkWidget *skm_make_label(const gchar *text, const gchar *css_class, gfloat xalign, gboolean wrap);
GtkWidget *skm_make_button(const gchar *text, const gchar *css_class);
GtkWidget *skm_attach_info_row(GtkGrid *grid, gint row, const gchar *title, GtkWidget **out_value);
GtkWidget *skm_make_metric_tile(const gchar *title, GtkWidget **out_value);
GtkWidget *skm_make_control_row(const gchar *title, GtkWidget *widget, GtkWidget *value_widget);
void skm_create_section_card(const gchar *title,
                             const gchar *subtitle,
                             GtkWidget **out_card,
                             GtkWidget **out_content,
                             GtkWidget **out_actions,
                             GtkWidget **out_unavailable);

void skm_flash_button(GtkWidget *button, gboolean success);
void skm_show_notice(SkmAppWindow *self, const gchar *kind, const gchar *message);

void skm_update_led_control_sensitivity(SkmAppWindow *self);
void skm_update_system(SkmAppWindow *self, const SkmSystemInfoState *state);
void skm_update_fan(SkmAppWindow *self, const SkmFanState *state);
void skm_update_led(SkmAppWindow *self, const SkmLedState *state);
void skm_update_gpu(SkmAppWindow *self, const SkmGpuState *state);
void skm_update_hdmi(SkmAppWindow *self, const SkmHdmiState *state);

void skm_refresh_dashboard(SkmAppWindow *self);
gboolean skm_poll_cb(gpointer user_data);

void skm_on_fan_threshold_changed(GtkRange *range, gpointer user_data);
void skm_on_fan_apply_clicked(GtkButton *button, gpointer user_data);
void skm_on_fan_reset_clicked(GtkButton *button, gpointer user_data);
void skm_on_led_effect_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
void skm_on_led_thermal_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
void skm_on_led_interval_changed(GtkSpinButton *spin, gpointer user_data);
void skm_on_led_apply_clicked(GtkButton *button, gpointer user_data);
void skm_on_led_reset_clicked(GtkButton *button, gpointer user_data);
void skm_on_gpu_level_toggled(GtkCheckButton *check, gpointer user_data);
void skm_on_gpu_manual_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
void skm_on_gpu_apply_clicked(GtkButton *button, gpointer user_data);
void skm_on_gpu_auto_clicked(GtkButton *button, gpointer user_data);
void skm_on_gpu_reset_clicked(GtkButton *button, gpointer user_data);
void skm_on_hdmi_reprobe_clicked(GtkButton *button, gpointer user_data);
