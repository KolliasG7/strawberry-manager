#pragma once

#include <glib.h>

typedef struct {
  gboolean oled_black_mode;
  gint poll_interval_ms;
  gint fan_debounce_ms;
  gboolean remote_enabled;
  gint remote_port;
  gchar *remote_password; /* NULL = no auth; non-NULL = HMAC token required */
} SkmAppSettings;

enum {
  SKM_POLL_INTERVAL_MIN_MS = 250,
  SKM_POLL_INTERVAL_MAX_MS = 10000,
  SKM_POLL_INTERVAL_DEFAULT_MS = 2000,
  SKM_FAN_DEBOUNCE_MIN_MS = 100,
  SKM_FAN_DEBOUNCE_MAX_MS = 5000,
  SKM_FAN_DEBOUNCE_DEFAULT_MS = 500,
  SKM_REMOTE_PORT_DEFAULT = 8080,
  SKM_REMOTE_PORT_MIN = 1024,
  SKM_REMOTE_PORT_MAX = 65535,
};

void skm_settings_init_defaults(SkmAppSettings *settings);
gboolean skm_settings_load(SkmAppSettings *settings, gchar **out_path, GError **error);
gboolean skm_settings_save(const SkmAppSettings *settings, const gchar *path, GError **error);
