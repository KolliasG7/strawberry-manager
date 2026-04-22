#include "skm-service-private.h"

#include "skm-sysfs.h"

static const gchar *const skm_led_effects[] = {
  "blue",
  "white",
  "orange",
  "orange_blue",
  "orange_white",
  "pulsate_orange",
  "orange_white_blue",
  "white_blue",
  "violet_blue",
  "pink",
  "pink_blue",
  NULL,
};

static gchar *
skm_led_effect_root(SkmService *self, const gchar *effect)
{
  g_autofree gchar *entry = NULL;

  g_return_val_if_fail(effect != NULL, NULL);
  entry = g_strdup_printf("ps4:%s:status", effect);
  return g_build_filename(self->led_root_base, entry, NULL);
}

gchar *
skm_led_effect_brightness_path(SkmService *self, const gchar *effect)
{
  g_autofree gchar *root = skm_led_effect_root(self, effect);

  return root != NULL ? g_build_filename(root, "brightness", NULL) : NULL;
}

gchar *
skm_led_mode_path(SkmService *self)
{
  return g_build_filename(self->led_device_root, "mode", NULL);
}

gchar *
skm_led_interval_path(SkmService *self)
{
  return g_build_filename(self->led_device_root, "thermal_interval_ms", NULL);
}

GPtrArray *
skm_collect_led_effect_options(SkmService *self)
{
  GPtrArray *options = g_ptr_array_new_with_free_func(g_free);
  guint i = 0;

  g_ptr_array_add(options, g_strdup("off"));

  for (i = 0; skm_led_effects[i] != NULL; i++) {
    g_autofree gchar *brightness_path = skm_led_effect_brightness_path(self, skm_led_effects[i]);

    if (skm_node_exists(brightness_path)) {
      g_ptr_array_add(options, g_strdup(skm_led_effects[i]));
    }
  }

  if (options->len == 1) {
    g_ptr_array_unref(options);
    return NULL;
  }

  return options;
}

gchar *
skm_detect_active_led_effect(SkmService *self, GPtrArray *options)
{
  guint i = 0;

  if (options == NULL) {
    return NULL;
  }

  for (i = 0; i < options->len; i++) {
    const gchar *effect = g_ptr_array_index(options, i);
    g_autofree gchar *brightness_path = NULL;
    gint brightness = 0;

    if (g_strcmp0(effect, "off") == 0) {
      continue;
    }

    brightness_path = skm_led_effect_brightness_path(self, effect);
    if (skm_sysfs_read_int(brightness_path, "LED brightness", &brightness, NULL) && brightness > 0) {
      return g_strdup(effect);
    }
  }

  return g_strdup("off");
}

gchar *
skm_thermal_led_effect_from_temp(SkmService *self)
{
  g_autofree gchar *temp_path = NULL;
  gint temp_mc = 0;

  if (self->fan_root == NULL) {
    return g_strdup("thermal");
  }

  temp_path = skm_path_join(self->fan_root, "temp1_input");
  if (!skm_sysfs_read_int(temp_path, "fan temperature", &temp_mc, NULL)) {
    return g_strdup("thermal");
  }

  if (temp_mc < 65000) {
    return g_strdup("blue");
  }

  if (temp_mc < 80000) {
    return g_strdup("white");
  }

  return g_strdup("orange");
}

void
skm_capture_led_defaults_if_needed(SkmService *self)
{
  SkmSnapshot *snapshot = NULL;

  if (self->led_defaults_valid) {
    return;
  }

  snapshot = skm_service_read_snapshot(self);
  if (snapshot->led.available) {
    self->led_defaults_valid = TRUE;
    g_clear_pointer(&self->led_default_effect, g_free);
    self->led_default_effect = g_strdup(
      snapshot->led.active_effect != NULL ? snapshot->led.active_effect : "off");
    self->led_default_has_thermal = snapshot->led.thermal_mode_supported && snapshot->led.has_thermal_mode;
    self->led_default_thermal = snapshot->led.thermal_mode;
    self->led_default_has_interval = snapshot->led.has_thermal_interval_ms;
    self->led_default_interval_ms = snapshot->led.thermal_interval_ms;
  }

  skm_snapshot_free(snapshot);
}

SkmLedState
skm_service_read_led(SkmService *self)
{
  SkmLedState state = { 0 };
  g_autofree gchar *mode_path = skm_led_mode_path(self);
  g_autofree gchar *interval_path = skm_led_interval_path(self);
  g_autofree gchar *mode_text = NULL;
  g_autoptr(GPtrArray) effects = NULL;

  effects = skm_collect_led_effect_options(self);
  if (effects == NULL) {
    state.available = FALSE;
    state.message = skm_not_exposed_message();
    return state;
  }

  state.available = TRUE;
  state.effect_options = g_steal_pointer(&effects);

  if (skm_node_exists(mode_path)) {
    state.thermal_mode_supported = TRUE;
    if (skm_sysfs_read_text(mode_path, "LED mode", &mode_text, NULL)) {
      state.has_thermal_mode = TRUE;
      state.thermal_mode = g_strcmp0(mode_text, "thermal") == 0;
    }
  }

  if (skm_node_exists(interval_path) &&
      skm_sysfs_read_int(interval_path, "LED thermal interval", &state.thermal_interval_ms, NULL)) {
    state.has_thermal_interval_ms = TRUE;
  }

  if (state.has_thermal_mode && state.thermal_mode) {
    state.has_active_effect = TRUE;
    state.active_effect = skm_thermal_led_effect_from_temp(self);
    state.message = g_strdup("Thermal LED mode active. Driver picks blue/white/orange from APU temp.");
  } else {
    state.active_effect = skm_detect_active_led_effect(self, state.effect_options);
    state.has_active_effect = state.active_effect != NULL;
    state.message = g_strdup("Static PS4 LED effects loaded.");
  }

  return state;
}

SkmOperationResult *
skm_service_apply_led(SkmService *self, const gchar *effect, gboolean thermal_mode, gint thermal_interval_ms)
{
  g_autofree gchar *mode_path = skm_led_mode_path(self);
  g_autofree gchar *interval_path = skm_led_interval_path(self);
  g_autoptr(GPtrArray) effects = NULL;
  g_autoptr(GError) error = NULL;
  gboolean effect_found = FALSE;
  guint i = 0;

  g_return_val_if_fail(self != NULL, skm_operation_result_new(FALSE, FALSE, "Service unavailable."));

  skm_service_refresh_paths(self);
  effects = skm_collect_led_effect_options(self);
  if (effects == NULL) {
    return skm_operation_result_new(FALSE, FALSE, "PS4 LED nodes not available.");
  }

  if (thermal_mode && thermal_interval_ms <= 0) {
    return skm_operation_result_new(FALSE, FALSE, "Thermal interval must be greater than 0 ms.");
  }

  for (i = 0; i < effects->len; i++) {
    const gchar *value = g_ptr_array_index(effects, i);

    if (g_strcmp0(value, effect) == 0) {
      effect_found = TRUE;
      break;
    }
  }

  if (!effect_found) {
    return skm_operation_result_new(FALSE, FALSE, "Selected LED effect is not available.");
  }

  if (thermal_mode) {
    if (skm_node_exists(interval_path) &&
        !skm_sysfs_write_int(interval_path, "LED thermal interval", thermal_interval_ms, &error)) {
      goto permission_or_error;
    }

    if (skm_node_exists(mode_path) &&
        !skm_sysfs_write_text(mode_path, "LED mode", "thermal", &error)) {
      goto permission_or_error;
    }
  } else {
    if (skm_node_exists(mode_path) &&
        !skm_sysfs_write_text(mode_path, "LED mode", "static", &error)) {
      goto permission_or_error;
    }

    for (i = 0; i < effects->len; i++) {
      const gchar *value = g_ptr_array_index(effects, i);
      g_autofree gchar *brightness_path = NULL;

      if (g_strcmp0(value, "off") == 0) {
        continue;
      }

      brightness_path = skm_led_effect_brightness_path(self, value);
      if (!skm_sysfs_write_int(brightness_path, "LED brightness", 0, &error)) {
        goto permission_or_error;
      }
    }

    if (g_strcmp0(effect, "off") != 0) {
      g_autofree gchar *brightness_path = skm_led_effect_brightness_path(self, effect);

      if (!skm_sysfs_write_int(brightness_path, "LED brightness", 255, &error)) {
        goto permission_or_error;
      }
    }
  }

  return skm_operation_result_new(
    TRUE,
    FALSE,
    thermal_mode ? "PS4 thermal LED mode applied." : "PS4 LED effect applied.");

permission_or_error:
  if (error != NULL && error->domain == SKM_SYSFS_ERROR && error->code == SKM_SYSFS_ERROR_PERMISSION) {
    g_autofree gchar *hint = skm_permission_hint("LED settings");
    return skm_operation_result_new(FALSE, TRUE, hint);
  }

  return skm_operation_result_new(FALSE, FALSE, error != NULL ? error->message : "Failed applying LED settings.");
}

SkmOperationResult *
skm_service_reset_led_defaults(SkmService *self)
{
  skm_capture_led_defaults_if_needed(self);

  if (!self->led_defaults_valid) {
    return skm_operation_result_new(FALSE, FALSE, "LED defaults unavailable on this PS4 kernel build.");
  }

  return skm_service_apply_led(
    self,
    self->led_default_effect != NULL ? self->led_default_effect : "off",
    self->led_default_has_thermal && self->led_default_thermal,
    self->led_default_has_interval ? self->led_default_interval_ms : 2000);
}
