#include "skm-sysfs.h"

#include <errno.h>
#include <glib/gstdio.h>

GQuark
skm_sysfs_error_quark(void)
{
  return g_quark_from_static_string("skm-sysfs-error");
}

gboolean
skm_node_exists(const gchar *path)
{
  return path != NULL && g_file_test(path, G_FILE_TEST_EXISTS);
}

static gboolean
skm_make_missing_error(const gchar *path, const gchar *label, GError **error)
{
  g_set_error(
    error,
    SKM_SYSFS_ERROR,
    SKM_SYSFS_ERROR_NOT_FOUND,
    "%s not found: %s",
    label,
    path);
  return FALSE;
}

static gboolean
skm_make_permission_error(const gchar *path, const gchar *label, const gchar *verb, GError **error)
{
  g_set_error(
    error,
    SKM_SYSFS_ERROR,
    SKM_SYSFS_ERROR_PERMISSION,
    "Permission denied %s %s: %s",
    verb,
    label,
    path);
  return FALSE;
}

gboolean
skm_sysfs_read_text(const gchar *path, const gchar *label, gchar **out_text, GError **error)
{
  gchar *raw = NULL;
  gsize length = 0;

  g_return_val_if_fail(out_text != NULL, FALSE);
  *out_text = NULL;

  if (!skm_node_exists(path)) {
    return skm_make_missing_error(path, label, error);
  }

  if (!g_file_get_contents(path, &raw, &length, error)) {
    if (error != NULL && *error != NULL) {
      if ((*error)->domain == G_FILE_ERROR && (*error)->code == G_FILE_ERROR_ACCES) {
        g_clear_error(error);
        return skm_make_permission_error(path, label, "reading", error);
      }
      g_prefix_error(error, "Failed reading %s: ", label);
    }
    return FALSE;
  }

  g_strchomp(raw);
  *out_text = raw;
  return TRUE;
}

gboolean
skm_sysfs_read_int(const gchar *path, const gchar *label, gint *out_value, GError **error)
{
  gchar *text = NULL;
  gchar *endptr = NULL;
  gint64 parsed = 0;

  g_return_val_if_fail(out_value != NULL, FALSE);
  *out_value = 0;

  if (!skm_sysfs_read_text(path, label, &text, error)) {
    return FALSE;
  }

  errno = 0;
  parsed = g_ascii_strtoll(text, &endptr, 0);
  if (errno != 0 || endptr == text) {
    g_set_error(
      error,
      SKM_SYSFS_ERROR,
      SKM_SYSFS_ERROR_IO,
      "Failed parsing integer from %s",
      label);
    g_free(text);
    return FALSE;
  }

  *out_value = (gint) parsed;
  g_free(text);
  return TRUE;
}

gboolean
skm_sysfs_write_text(const gchar *path, const gchar *label, const gchar *value, GError **error)
{
  FILE *handle = NULL;

  if (!skm_node_exists(path)) {
    return skm_make_missing_error(path, label, error);
  }

  handle = g_fopen(path, "w");
  if (handle == NULL) {
    if (errno == EACCES || errno == EPERM) {
      return skm_make_permission_error(path, label, "writing", error);
    }

    g_set_error(
      error,
      SKM_SYSFS_ERROR,
      SKM_SYSFS_ERROR_IO,
      "Failed writing %s: %s",
      label,
      g_strerror(errno));
    return FALSE;
  }

  if (fputs(value, handle) == EOF) {
    if (errno == EACCES || errno == EPERM) {
      fclose(handle);
      return skm_make_permission_error(path, label, "writing", error);
    }

    g_set_error(
      error,
      SKM_SYSFS_ERROR,
      SKM_SYSFS_ERROR_IO,
      "Failed writing %s: %s",
      label,
      g_strerror(errno));
    fclose(handle);
    return FALSE;
  }

  fclose(handle);
  return TRUE;
}

gboolean
skm_sysfs_write_int(const gchar *path, const gchar *label, gint value, GError **error)
{
  gchar *text = g_strdup_printf("%d", value);
  gboolean ok = skm_sysfs_write_text(path, label, text, error);
  g_free(text);
  return ok;
}

gchar *
skm_find_hwmon_by_name(const gchar *base_path, const gchar *name)
{
  GDir *dir = NULL;
  const gchar *entry = NULL;

  if (!skm_node_exists(base_path)) {
    return NULL;
  }

  dir = g_dir_open(base_path, 0, NULL);
  if (dir == NULL) {
    return NULL;
  }

  while ((entry = g_dir_read_name(dir)) != NULL) {
    g_autofree gchar *candidate = NULL;
    g_autofree gchar *name_path = NULL;
    g_autofree gchar *read_name = NULL;

    if (!g_str_has_prefix(entry, "hwmon")) {
      continue;
    }

    candidate = g_build_filename(base_path, entry, NULL);
    name_path = g_build_filename(candidate, "name", NULL);
    if (!skm_sysfs_read_text(name_path, "hwmon name", &read_name, NULL)) {
      continue;
    }

    if (g_strcmp0(read_name, name) == 0) {
      g_dir_close(dir);
      return g_steal_pointer(&candidate);
    }
  }

  g_dir_close(dir);
  return NULL;
}

gchar *
skm_find_drm_connector(const gchar *base_path, const gchar *preferred_name)
{
  g_autofree gchar *preferred = NULL;
  GDir *dir = NULL;
  const gchar *entry = NULL;

  preferred = g_build_filename(base_path, preferred_name, "status", NULL);
  if (skm_node_exists(preferred)) {
    return g_build_filename(base_path, preferred_name, NULL);
  }

  if (!skm_node_exists(base_path)) {
    return NULL;
  }

  dir = g_dir_open(base_path, 0, NULL);
  if (dir == NULL) {
    return NULL;
  }

  while ((entry = g_dir_read_name(dir)) != NULL) {
    g_autofree gchar *status_path = NULL;

    if (!g_str_has_prefix(entry, "card")) {
      continue;
    }

    if (strchr(entry, '-') == NULL) {
      continue;
    }

    status_path = g_build_filename(base_path, entry, "status", NULL);
    if (skm_node_exists(status_path)) {
      g_dir_close(dir);
      return g_build_filename(base_path, entry, NULL);
    }
  }

  g_dir_close(dir);
  return NULL;
}
