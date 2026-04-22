#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  SKM_SYSFS_ERROR_NOT_FOUND,
  SKM_SYSFS_ERROR_PERMISSION,
  SKM_SYSFS_ERROR_IO,
  SKM_SYSFS_ERROR_VALIDATION,
} SkmSysfsError;

#define SKM_SYSFS_ERROR skm_sysfs_error_quark()

GQuark skm_sysfs_error_quark(void);

gboolean skm_node_exists(const gchar *path);
gboolean skm_sysfs_read_text(const gchar *path, const gchar *label, gchar **out_text, GError **error);
gboolean skm_sysfs_read_int(const gchar *path, const gchar *label, gint *out_value, GError **error);
gboolean skm_sysfs_write_text(const gchar *path, const gchar *label, const gchar *value, GError **error);
gboolean skm_sysfs_write_int(const gchar *path, const gchar *label, gint value, GError **error);

gchar *skm_find_hwmon_by_name(const gchar *base_path, const gchar *name);
gchar *skm_find_drm_connector(const gchar *base_path, const gchar *preferred_name);

G_END_DECLS
