#pragma once

#include <glib.h>

typedef struct {
  gboolean oled_black_mode;
  gint poll_interval_ms;
  gint fan_debounce_ms;
  gboolean remote_enabled;
  gint remote_port;
  /* Legacy plaintext password slot. Kept only to migrate older settings.ini
   * files to the hashed form below, then cleared. Never persisted going
   * forward. */
  gchar *remote_password;
  /* Per-install random HMAC salt (hex). Generated on first run when a
   * password is set; used to derive tokens. NULL when no password set. */
  gchar *remote_hmac_salt;
  /* HMAC-SHA256(password, salt) hex digest. This is what the daemon compares
   * submitted passwords against — the plaintext password is never written
   * to disk. NULL when no password set. */
  gchar *remote_password_hash;
} SkmAppSettings;

enum {
  SKM_POLL_INTERVAL_MIN_MS = 250,
  SKM_POLL_INTERVAL_MAX_MS = 10000,
  SKM_POLL_INTERVAL_DEFAULT_MS = 2000,
  SKM_FAN_DEBOUNCE_MIN_MS = 100,
  SKM_FAN_DEBOUNCE_MAX_MS = 5000,
  SKM_FAN_DEBOUNCE_DEFAULT_MS = 500,
  SKM_REMOTE_PORT_DEFAULT = 8000,
  SKM_REMOTE_PORT_MIN = 1024,
  SKM_REMOTE_PORT_MAX = 65535,
};

void skm_settings_init_defaults(SkmAppSettings *settings);
gboolean skm_settings_load(SkmAppSettings *settings, gchar **out_path, GError **error);
gboolean skm_settings_save(const SkmAppSettings *settings, const gchar *path, GError **error);
