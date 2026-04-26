#include "skm-service-private.h"

#include "skm-sysfs.h"

SkmFanState
skm_service_read_fan(SkmService *self)
{
  SkmFanState state = { 0 };
  g_autofree gchar *temp_path = NULL;
  g_autofree gchar *rpm_path = NULL;
  g_autofree gchar *threshold_path = NULL;
  gint temp_raw = 0;
  gint threshold_raw = 0;

  if (self->fan_root == NULL) {
    state.available = FALSE;
    state.message = skm_not_exposed_message();
    return state;
  }

  temp_path = skm_path_join(self->fan_root, "temp1_input");
  rpm_path = skm_path_join(self->fan_root, "fan1_input");
  threshold_path = skm_path_join(self->fan_root, "temp1_crit");

  if (!skm_node_exists(temp_path) || !skm_node_exists(rpm_path) || !skm_node_exists(threshold_path)) {
    state.available = FALSE;
    state.message = skm_not_exposed_message();
    return state;
  }

  state.available = TRUE;
  state.message = g_strdup("PS4 fan hwmon loaded (`temp1_input`, `temp1_crit`, `fan1_input`).");

  if (skm_sysfs_read_int(temp_path, "fan temperature", &temp_raw, NULL)) {
    state.has_temperature_c = TRUE;
    state.temperature_c = temp_raw / 1000.0;
  }

  if (skm_sysfs_read_int(rpm_path, "fan RPM", &state.rpm, NULL)) {
    state.has_rpm = TRUE;
  }

  if (skm_sysfs_read_int(threshold_path, "fan threshold", &threshold_raw, NULL)) {
    state.has_threshold_c = TRUE;
    state.threshold_c = threshold_raw / 1000;
  }

  return state;
}

SkmOperationResult *
skm_service_apply_fan(SkmService *self, gint threshold_c)
{
  g_autofree gchar *threshold_path = NULL;
  g_autoptr(GError) error = NULL;

  g_return_val_if_fail(self != NULL, skm_operation_result_new(FALSE, FALSE, "Service unavailable."));

  skm_service_refresh_paths(self);
  if (self->fan_root == NULL) {
    return skm_operation_result_new(FALSE, FALSE, "PS4 fan hwmon not available.");
  }

  if (threshold_c < SKM_FAN_THRESHOLD_MIN || threshold_c > SKM_FAN_THRESHOLD_MAX) {
    return skm_operation_result_new(FALSE, FALSE, "Fan threshold must be -10-85 C.");
  }

  threshold_path = skm_path_join(self->fan_root, "temp1_crit");

  if (!skm_sysfs_write_int(threshold_path, "fan threshold", threshold_c * 1000, &error)) {
    if (error != NULL && error->domain == SKM_SYSFS_ERROR && error->code == SKM_SYSFS_ERROR_PERMISSION) {
      g_autofree gchar *hint = skm_permission_hint("fan settings");
      return skm_operation_result_new(FALSE, TRUE, hint);
    }

    return skm_operation_result_new(FALSE, FALSE, error != NULL ? error->message : "Failed applying fan settings.");
  }

  return skm_operation_result_new(TRUE, FALSE, "Fan settings applied.");
}

SkmOperationResult *
skm_service_reset_fan_defaults(SkmService *self)
{
  return skm_service_apply_fan(self, SKM_FAN_THRESHOLD_DEFAULT);
}
