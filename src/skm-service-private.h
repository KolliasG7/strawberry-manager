#pragma once

#include "skm-service.h"

struct _SkmService {
  gchar *sys_root;
  gchar *proc_root;
  gchar *hwmon_root;
  gchar *led_root_base;
  gchar *led_device_root;
  gchar *drm_root;
  gchar *gpu_root;

  gchar *fan_root;
  gchar *connector_root;

  gboolean led_defaults_valid;
  gchar *led_default_effect;
  gboolean led_default_has_thermal;
  gboolean led_default_thermal;
  gboolean led_default_has_interval;
  gint led_default_interval_ms;
};

gchar *skm_path_join(const gchar *left, const gchar *right);
gchar *skm_permission_hint(const gchar *subject);
gchar *skm_not_exposed_message(void);
void skm_service_refresh_paths(SkmService *self);

SkmOperationResult *skm_operation_result_new(gboolean success, gboolean permission_denied, const gchar *message);

void skm_fan_state_clear(SkmFanState *state);
void skm_led_state_clear(SkmLedState *state);
void skm_gpu_state_clear(SkmGpuState *state);
void skm_hdmi_state_clear(SkmHdmiState *state);
void skm_system_info_clear(SkmSystemInfoState *state);

SkmSystemInfoState skm_service_read_system_info(SkmService *self);
SkmFanState skm_service_read_fan(SkmService *self);
SkmLedState skm_service_read_led(SkmService *self);
SkmGpuState skm_service_read_gpu(SkmService *self);
SkmHdmiState skm_service_read_hdmi(SkmService *self);

gchar *skm_read_text_or_unavailable(const gchar *path, const gchar *label);
gboolean skm_read_variant(SkmService *self, gchar **out_variant, gchar **out_vendor, gchar **out_device);

gchar *skm_led_effect_brightness_path(SkmService *self, const gchar *effect);
gchar *skm_led_mode_path(SkmService *self);
gchar *skm_led_interval_path(SkmService *self);
GPtrArray *skm_collect_led_effect_options(SkmService *self);
gchar *skm_detect_active_led_effect(SkmService *self, GPtrArray *options);
gchar *skm_thermal_led_effect_from_temp(SkmService *self);
void skm_capture_led_defaults_if_needed(SkmService *self);

GPtrArray *skm_parse_sclk_levels(const gchar *raw);
