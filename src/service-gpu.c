#include "skm-service-private.h"

#include "skm-sysfs.h"

#include <string.h>

static gint
skm_extract_mhz(const gchar *label, gboolean *has_mhz)
{
  g_autofree gchar *lower = NULL;
  gchar *mhz_ptr = NULL;
  gint start = 0;
  gint end = 0;

  *has_mhz = FALSE;
  if (label == NULL) {
    return 0;
  }

  lower = g_ascii_strdown(label, -1);
  mhz_ptr = g_strstr_len(lower, -1, "mhz");
  if (mhz_ptr == NULL) {
    return 0;
  }

  end = (gint) (mhz_ptr - lower);
  start = end - 1;
  while (start >= 0 && g_ascii_isdigit(lower[start])) {
    start--;
  }
  start++;

  if (start >= end) {
    return 0;
  }

  *has_mhz = TRUE;
  return (gint) g_ascii_strtoll(label + start, NULL, 10);
}

GPtrArray *
skm_parse_sclk_levels(const gchar *raw)
{
  GPtrArray *levels = g_ptr_array_new_with_free_func((GDestroyNotify) skm_gpu_level_free);
  gchar **lines = NULL;
  guint i = 0;

  if (raw == NULL) {
    return levels;
  }

  lines = g_strsplit(raw, "\n", -1);
  for (i = 0; lines[i] != NULL; i++) {
    g_autofree gchar *line = g_strdup(lines[i]);
    gchar *colon = NULL;
    gchar *label = NULL;
    SkmGpuLevel *level = NULL;

    g_strstrip(line);
    if (*line == '\0') {
      continue;
    }

    level = g_new0(SkmGpuLevel, 1);
    if (g_str_has_suffix(line, "*")) {
      level->active = TRUE;
      line[strlen(line) - 1] = '\0';
      g_strstrip(line);
    }

    colon = strchr(line, ':');
    if (colon == NULL) {
      g_free(level);
      continue;
    }

    *colon = '\0';
    colon++;
    g_strstrip(line);
    g_strstrip(colon);

    level->index = (gint) g_ascii_strtoll(line, NULL, 10);
    label = g_strdup(colon);
    level->label = label;
    level->mhz = skm_extract_mhz(level->label, &level->has_mhz);
    g_ptr_array_add(levels, level);
  }

  g_strfreev(lines);
  return levels;
}

SkmGpuState
skm_service_read_gpu(SkmService *self)
{
  SkmGpuState state = { 0 };
  g_autofree gchar *variant = NULL;
  g_autofree gchar *vendor = NULL;
  g_autofree gchar *device = NULL;
  gboolean supported = FALSE;
  g_autofree gchar *perf_path = NULL;
  g_autofree gchar *sclk_path = NULL;
  g_autofree gchar *perf_text = NULL;
  g_autofree gchar *sclk_text = NULL;
  guint i = 0;

  supported = skm_read_variant(self, &variant, &vendor, &device);
  state.supported_hardware = supported;
  state.variant = g_strdup(variant);
  state.vendor_id = g_strdup(vendor);
  state.device_id = g_strdup(device);

  if (!supported) {
    state.warning = g_strdup_printf(
      "Detected vendor %s, device %s. SCLK forcing only supported on CHIP_LIVERPOOL / CHIP_GLADIUS.",
      vendor,
      device);
  }

  perf_path = g_build_filename(self->gpu_root, "power_dpm_force_performance_level", NULL);
  sclk_path = g_build_filename(self->gpu_root, "pp_dpm_sclk", NULL);
  if (!skm_node_exists(perf_path) || !skm_node_exists(sclk_path)) {
    state.available = FALSE;
    state.message = skm_not_exposed_message();
    return state;
  }

  if (!skm_sysfs_read_text(perf_path, "GPU performance level", &perf_text, NULL) ||
      !skm_sysfs_read_text(sclk_path, "GPU sclk table", &sclk_text, NULL)) {
    state.available = FALSE;
    state.message = skm_not_exposed_message();
    return state;
  }

  state.available = TRUE;
  state.message = g_strdup("GPU SCLK levels loaded.");
  state.performance_level = g_strdup(perf_text);
  state.levels = skm_parse_sclk_levels(sclk_text);

  for (i = 0; i < state.levels->len; i++) {
    SkmGpuLevel *level = g_ptr_array_index(state.levels, i);

    if (level->active) {
      state.has_active_level = TRUE;
      state.active_level = level->index;
      break;
    }
  }

  return state;
}

SkmOperationResult *
skm_service_set_gpu_manual(SkmService *self, gboolean enabled)
{
  g_autofree gchar *perf_path = NULL;
  g_autoptr(GError) error = NULL;

  g_return_val_if_fail(self != NULL, skm_operation_result_new(FALSE, FALSE, "Service unavailable."));

  perf_path = g_build_filename(self->gpu_root, "power_dpm_force_performance_level", NULL);
  if (!skm_node_exists(perf_path)) {
    return skm_operation_result_new(FALSE, FALSE, "GPU control nodes not available.");
  }

  if (!skm_sysfs_write_text(perf_path, "GPU performance level", enabled ? "manual" : "auto", &error)) {
    if (error != NULL && error->domain == SKM_SYSFS_ERROR && error->code == SKM_SYSFS_ERROR_PERMISSION) {
      g_autofree gchar *hint = skm_permission_hint("GPU performance level");
      return skm_operation_result_new(FALSE, TRUE, hint);
    }

    return skm_operation_result_new(FALSE, FALSE, error != NULL ? error->message : "Failed updating GPU mode.");
  }

  return skm_operation_result_new(
    TRUE,
    FALSE,
    enabled ? "GPU manual mode enabled." : "GPU auto mode restored.");
}

SkmOperationResult *
skm_service_apply_gpu_level(SkmService *self, gint index)
{
  g_autofree gchar *perf_path = NULL;
  g_autofree gchar *sclk_path = NULL;
  g_autofree gchar *sclk_text = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) levels = NULL;
  gboolean found = FALSE;
  gboolean active = FALSE;
  guint i = 0;
  g_autofree gchar *variant = NULL;
  g_autofree gchar *vendor = NULL;
  g_autofree gchar *device = NULL;

  g_return_val_if_fail(self != NULL, skm_operation_result_new(FALSE, FALSE, "Service unavailable."));

  if (!skm_read_variant(self, &variant, &vendor, &device)) {
    return skm_operation_result_new(
      FALSE,
      FALSE,
      "SCLK forcing only supported on CHIP_LIVERPOOL / CHIP_GLADIUS.");
  }

  perf_path = g_build_filename(self->gpu_root, "power_dpm_force_performance_level", NULL);
  sclk_path = g_build_filename(self->gpu_root, "pp_dpm_sclk", NULL);
  if (!skm_sysfs_read_text(sclk_path, "GPU sclk table", &sclk_text, &error)) {
    return skm_operation_result_new(FALSE, FALSE, error->message);
  }

  levels = skm_parse_sclk_levels(sclk_text);
  for (i = 0; i < levels->len; i++) {
    SkmGpuLevel *level = g_ptr_array_index(levels, i);

    if (level->index == index) {
      found = TRUE;
      break;
    }
  }

  if (!found) {
    return skm_operation_result_new(FALSE, FALSE, "Selected SCLK level is not available.");
  }

  if (!skm_sysfs_write_text(perf_path, "GPU performance level", "manual", &error) ||
      !skm_sysfs_write_int(sclk_path, "GPU sclk table", index, &error)) {
    if (error != NULL && error->domain == SKM_SYSFS_ERROR && error->code == SKM_SYSFS_ERROR_PERMISSION) {
      g_autofree gchar *hint = skm_permission_hint("GPU SCLK");
      return skm_operation_result_new(FALSE, TRUE, hint);
    }

    return skm_operation_result_new(FALSE, FALSE, error != NULL ? error->message : "Failed forcing SCLK level.");
  }

  g_usleep(500000);
  g_clear_pointer(&sclk_text, g_free);
  if (!skm_sysfs_read_text(sclk_path, "GPU sclk table", &sclk_text, &error)) {
    return skm_operation_result_new(FALSE, FALSE, error->message);
  }

  g_clear_pointer(&levels, g_ptr_array_unref);
  levels = skm_parse_sclk_levels(sclk_text);
  for (i = 0; i < levels->len; i++) {
    SkmGpuLevel *level = g_ptr_array_index(levels, i);

    if (level->index == index && level->active) {
      active = TRUE;
      break;
    }
  }

  if (!active) {
    return skm_operation_result_new(FALSE, FALSE, "GPU level write did not stick after 500 ms confirmation poll.");
  }

  return skm_operation_result_new(TRUE, FALSE, "GPU SCLK level applied.");
}

SkmOperationResult *
skm_service_reset_gpu_defaults(SkmService *self)
{
  return skm_service_set_gpu_manual(self, FALSE);
}
