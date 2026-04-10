#include "skm-service-private.h"

#include "skm-sysfs.h"

gchar *
skm_path_join(const gchar *left, const gchar *right)
{
  if (left == NULL) {
    return NULL;
  }

  return g_build_filename(left, right, NULL);
}

gchar *
skm_permission_hint(const gchar *subject)
{
  return g_strdup_printf(
    "Permission denied writing %s. Run as root or install Strawberry udev rules for video group access.",
    subject);
}

gchar *
skm_not_exposed_message(void)
{
  return g_strdup("Not exposed by this Strawberry PS4 kernel build.");
}

void
skm_service_refresh_paths(SkmService *self)
{
  g_clear_pointer(&self->fan_root, g_free);
  g_clear_pointer(&self->connector_root, g_free);

  self->fan_root = skm_find_hwmon_by_name(self->hwmon_root, "ps4_fan");
  self->connector_root = skm_find_drm_connector(self->drm_root, "card0-HDMI-A-1");
}

SkmService *
skm_service_new(const gchar *sys_root, const gchar *proc_root)
{
  SkmService *self = g_new0(SkmService, 1);

  self->sys_root = g_strdup(sys_root != NULL ? sys_root : "/sys");
  self->proc_root = g_strdup(proc_root != NULL ? proc_root : "/proc");
  self->hwmon_root = g_build_filename(self->sys_root, "class", "hwmon", NULL);
  self->led_root_base = g_build_filename(self->sys_root, "class", "leds", NULL);
  self->led_device_root = g_build_filename(self->sys_root, "bus", "platform", "devices", "ps4-led", NULL);
  self->drm_root = g_build_filename(self->sys_root, "class", "drm", NULL);
  self->gpu_root = g_build_filename(self->drm_root, "card0", "device", NULL);

  skm_service_refresh_paths(self);
  return self;
}

void
skm_service_free(SkmService *self)
{
  if (self == NULL) {
    return;
  }

  g_clear_pointer(&self->sys_root, g_free);
  g_clear_pointer(&self->proc_root, g_free);
  g_clear_pointer(&self->hwmon_root, g_free);
  g_clear_pointer(&self->led_root_base, g_free);
  g_clear_pointer(&self->led_device_root, g_free);
  g_clear_pointer(&self->drm_root, g_free);
  g_clear_pointer(&self->gpu_root, g_free);
  g_clear_pointer(&self->fan_root, g_free);
  g_clear_pointer(&self->connector_root, g_free);
  g_clear_pointer(&self->led_default_effect, g_free);
  g_free(self);
}

SkmOperationResult *
skm_operation_result_new(gboolean success, gboolean permission_denied, const gchar *message)
{
  SkmOperationResult *result = g_new0(SkmOperationResult, 1);

  result->success = success;
  result->permission_denied = permission_denied;
  result->message = g_strdup(message);
  return result;
}

void
skm_operation_result_free(SkmOperationResult *result)
{
  if (result == NULL) {
    return;
  }

  g_clear_pointer(&result->message, g_free);
  g_free(result);
}

void
skm_fan_state_clear(SkmFanState *state)
{
  g_clear_pointer(&state->message, g_free);
}

void
skm_led_state_clear(SkmLedState *state)
{
  g_clear_pointer(&state->message, g_free);
  g_clear_pointer(&state->active_effect, g_free);
  g_clear_pointer(&state->effect_options, g_ptr_array_unref);
}

void
skm_gpu_level_free(SkmGpuLevel *level)
{
  if (level == NULL) {
    return;
  }

  g_clear_pointer(&level->label, g_free);
  g_free(level);
}

void
skm_gpu_state_clear(SkmGpuState *state)
{
  g_clear_pointer(&state->message, g_free);
  g_clear_pointer(&state->variant, g_free);
  g_clear_pointer(&state->vendor_id, g_free);
  g_clear_pointer(&state->device_id, g_free);
  g_clear_pointer(&state->performance_level, g_free);
  g_clear_pointer(&state->warning, g_free);
  g_clear_pointer(&state->levels, g_ptr_array_unref);
}

void
skm_hdmi_state_clear(SkmHdmiState *state)
{
  g_clear_pointer(&state->message, g_free);
  g_clear_pointer(&state->connector_name, g_free);
  g_clear_pointer(&state->status, g_free);
}

void
skm_system_info_clear(SkmSystemInfoState *state)
{
  g_clear_pointer(&state->kernel_version, g_free);
  g_clear_pointer(&state->hardware_variant, g_free);
  g_clear_pointer(&state->cpu_governor, g_free);
  g_clear_pointer(&state->uptime, g_free);
}

void
skm_snapshot_free(SkmSnapshot *snapshot)
{
  if (snapshot == NULL) {
    return;
  }

  skm_system_info_clear(&snapshot->system);
  skm_fan_state_clear(&snapshot->fan);
  skm_led_state_clear(&snapshot->led);
  skm_gpu_state_clear(&snapshot->gpu);
  skm_hdmi_state_clear(&snapshot->hdmi);
  g_free(snapshot);
}

SkmSnapshot *
skm_service_read_snapshot(SkmService *self)
{
  SkmSnapshot *snapshot = g_new0(SkmSnapshot, 1);

  g_return_val_if_fail(self != NULL, snapshot);

  skm_service_refresh_paths(self);
  snapshot->system = skm_service_read_system_info(self);
  snapshot->fan = skm_service_read_fan(self);
  snapshot->led = skm_service_read_led(self);
  snapshot->gpu = skm_service_read_gpu(self);
  snapshot->hdmi = skm_service_read_hdmi(self);

  if (!self->led_defaults_valid && snapshot->led.available) {
    self->led_defaults_valid = TRUE;
    g_clear_pointer(&self->led_default_effect, g_free);
    self->led_default_effect = g_strdup(
      snapshot->led.active_effect != NULL ? snapshot->led.active_effect : "off");
    self->led_default_has_thermal = snapshot->led.thermal_mode_supported && snapshot->led.has_thermal_mode;
    self->led_default_thermal = snapshot->led.thermal_mode;
    self->led_default_has_interval = snapshot->led.has_thermal_interval_ms;
    self->led_default_interval_ms = snapshot->led.thermal_interval_ms;
  }

  return snapshot;
}
