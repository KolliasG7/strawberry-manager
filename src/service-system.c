#include "skm-service-private.h"

#include "skm-sysfs.h"

#include <sys/utsname.h>

static gchar *
skm_variant_from_device(const gchar *device_id)
{
  if (g_strcmp0(device_id, "0x9920") == 0) {
    return g_strdup("Liverpool");
  }

  if (g_strcmp0(device_id, "0x9923") == 0) {
    return g_strdup("Gladius");
  }

  return g_strdup("Unknown");
}

static gboolean
skm_supported_variant(const gchar *vendor_id, const gchar *device_id)
{
  return g_strcmp0(vendor_id, "0x1002") == 0 &&
         (g_strcmp0(device_id, "0x9920") == 0 || g_strcmp0(device_id, "0x9923") == 0);
}

static gchar *
skm_format_uptime(const gchar *proc_uptime_path)
{
  g_autofree gchar *text = NULL;
  gchar **parts = NULL;
  gdouble total = 0.0;
  gint days = 0;
  gint hours = 0;
  gint minutes = 0;
  gchar *formatted = NULL;

  if (!skm_sysfs_read_text(proc_uptime_path, "system uptime", &text, NULL)) {
    return g_strdup("Unavailable");
  }

  parts = g_strsplit(text, " ", 2);
  if (parts[0] == NULL) {
    g_strfreev(parts);
    return g_strdup("Unavailable");
  }

  total = g_ascii_strtod(parts[0], NULL);
  g_strfreev(parts);

  days = (gint) (total / 86400.0);
  total -= days * 86400.0;
  hours = (gint) (total / 3600.0);
  total -= hours * 3600.0;
  minutes = (gint) (total / 60.0);

  if (days > 0) {
    formatted = g_strdup_printf("%dd %dh %dm", days, hours, minutes);
  } else if (hours > 0) {
    formatted = g_strdup_printf("%dh %dm", hours, minutes);
  } else {
    formatted = g_strdup_printf("%dm", minutes);
  }

  return formatted;
}

gchar *
skm_read_text_or_unavailable(const gchar *path, const gchar *label)
{
  g_autofree gchar *text = NULL;

  if (!skm_sysfs_read_text(path, label, &text, NULL)) {
    return g_strdup("Unavailable");
  }

  return g_strdup(text);
}

gboolean
skm_read_variant(SkmService *self, gchar **out_variant, gchar **out_vendor, gchar **out_device)
{
  g_autofree gchar *vendor_path = NULL;
  g_autofree gchar *device_path = NULL;
  g_autofree gchar *vendor = NULL;
  g_autofree gchar *device = NULL;
  gboolean supported = FALSE;

  vendor_path = g_build_filename(self->gpu_root, "vendor", NULL);
  device_path = g_build_filename(self->gpu_root, "device", NULL);

  if (!skm_sysfs_read_text(vendor_path, "GPU vendor", &vendor, NULL)) {
    vendor = g_strdup("Unavailable");
  }

  if (!skm_sysfs_read_text(device_path, "GPU device", &device, NULL)) {
    device = g_strdup("Unavailable");
  }

  *out_variant = skm_variant_from_device(device);
  *out_vendor = g_strdup(vendor);
  *out_device = g_strdup(device);
  supported = skm_supported_variant(vendor, device);
  return supported;
}

SkmSystemInfoState
skm_service_read_system_info(SkmService *self)
{
  struct utsname uts;
  SkmSystemInfoState state = { 0 };
  g_autofree gchar *variant = NULL;
  g_autofree gchar *vendor = NULL;
  g_autofree gchar *device = NULL;
  g_autofree gchar *cpu_gov_path = NULL;
  g_autofree gchar *uptime_path = NULL;

  skm_read_variant(self, &variant, &vendor, &device);

  if (uname(&uts) == 0) {
    state.kernel_version = g_strdup(uts.release);
  } else {
    state.kernel_version = g_strdup("Unavailable");
  }

  state.hardware_variant = g_strdup(variant);
  cpu_gov_path = g_build_filename(self->sys_root, "devices", "system", "cpu", "cpu0", "cpufreq", "scaling_governor", NULL);
  uptime_path = g_build_filename(self->proc_root, "uptime", NULL);

  state.cpu_governor = skm_read_text_or_unavailable(cpu_gov_path, "CPU governor");
  state.uptime = skm_format_uptime(uptime_path);
  return state;
}

SkmHdmiState
skm_service_read_hdmi(SkmService *self)
{
  SkmHdmiState state = { 0 };
  g_autofree gchar *status_path = NULL;

  if (self->connector_root == NULL) {
    state.available = FALSE;
    state.message = skm_not_exposed_message();
    return state;
  }

  status_path = skm_path_join(self->connector_root, "status");
  if (!skm_node_exists(status_path)) {
    state.available = FALSE;
    state.message = skm_not_exposed_message();
    return state;
  }

  state.available = TRUE;
  state.message = g_strdup("Polling disabled in kernel. Use Reprobe after reconnecting monitor.");
  state.connector_name = g_path_get_basename(self->connector_root);
  state.status = skm_read_text_or_unavailable(status_path, "HDMI status");
  return state;
}

SkmOperationResult *
skm_service_reprobe_display(SkmService *self)
{
  g_autofree gchar *status_path = NULL;
  g_autofree gchar *status = NULL;
  g_autoptr(GError) error = NULL;

  g_return_val_if_fail(self != NULL, skm_operation_result_new(FALSE, FALSE, "Service unavailable."));

  skm_service_refresh_paths(self);
  if (self->connector_root == NULL) {
    return skm_operation_result_new(FALSE, FALSE, "HDMI connector status node not available.");
  }

  status_path = skm_path_join(self->connector_root, "status");
  if (!skm_sysfs_write_text(status_path, "HDMI status", "detect", &error)) {
    if (error != NULL && error->domain == SKM_SYSFS_ERROR && error->code == SKM_SYSFS_ERROR_PERMISSION) {
      g_autofree gchar *hint = skm_permission_hint("HDMI reprobe");
      return skm_operation_result_new(FALSE, TRUE, hint);
    }

    return skm_operation_result_new(FALSE, FALSE, error != NULL ? error->message : "Failed triggering HDMI reprobe.");
  }

  g_usleep(500000);
  status = skm_read_text_or_unavailable(status_path, "HDMI status");
  return skm_operation_result_new(TRUE, FALSE, status != NULL ? status : "HDMI reprobe triggered.");
}
