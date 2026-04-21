#include "skm-settings.h"

#include <glib/gstdio.h>

#include <errno.h>
#include <sys/stat.h>

static gchar *
skm_settings_build_path(void)
{
  return g_build_filename(g_get_user_config_dir(), "strawberry-kernel-manager", "settings.ini", NULL);
}

static void
skm_settings_clamp(SkmAppSettings *settings)
{
  settings->poll_interval_ms = CLAMP(
    settings->poll_interval_ms,
    SKM_POLL_INTERVAL_MIN_MS,
    SKM_POLL_INTERVAL_MAX_MS);
  settings->fan_debounce_ms = CLAMP(
    settings->fan_debounce_ms,
    SKM_FAN_DEBOUNCE_MIN_MS,
    SKM_FAN_DEBOUNCE_MAX_MS);
  settings->remote_port = CLAMP(
    settings->remote_port,
    SKM_REMOTE_PORT_MIN,
    SKM_REMOTE_PORT_MAX);
}

void
skm_settings_init_defaults(SkmAppSettings *settings)
{
  g_return_if_fail(settings != NULL);

  settings->oled_black_mode = FALSE;
  settings->poll_interval_ms = SKM_POLL_INTERVAL_DEFAULT_MS;
  settings->fan_debounce_ms = SKM_FAN_DEBOUNCE_DEFAULT_MS;
  settings->remote_enabled = FALSE;
  settings->remote_port = SKM_REMOTE_PORT_DEFAULT;
  g_clear_pointer(&settings->remote_password,      g_free);
  g_clear_pointer(&settings->remote_hmac_salt,     g_free);
  g_clear_pointer(&settings->remote_password_hash, g_free);
}

gboolean
skm_settings_load(SkmAppSettings *settings, gchar **out_path, GError **error)
{
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  g_autofree gchar *path = NULL;

  g_return_val_if_fail(settings != NULL, FALSE);

  skm_settings_init_defaults(settings);
  path = skm_settings_build_path();
  if (out_path != NULL) {
    *out_path = g_strdup(path);
  }

  if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
    return TRUE;
  }

  if (!g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, error)) {
    return FALSE;
  }

  if (g_key_file_has_key(key_file, "appearance", "oled_black_mode", NULL)) {
    settings->oled_black_mode = g_key_file_get_boolean(key_file, "appearance", "oled_black_mode", NULL);
  }
  if (g_key_file_has_key(key_file, "timing", "poll_interval_ms", NULL)) {
    settings->poll_interval_ms = g_key_file_get_integer(key_file, "timing", "poll_interval_ms", NULL);
  }
  if (g_key_file_has_key(key_file, "timing", "fan_debounce_ms", NULL)) {
    settings->fan_debounce_ms = g_key_file_get_integer(key_file, "timing", "fan_debounce_ms", NULL);
  }
  if (g_key_file_has_key(key_file, "experimental", "remote_enabled", NULL)) {
    settings->remote_enabled = g_key_file_get_boolean(key_file, "experimental", "remote_enabled", NULL);
  }
  if (g_key_file_has_key(key_file, "experimental", "remote_port", NULL)) {
    settings->remote_port = g_key_file_get_integer(key_file, "experimental", "remote_port", NULL);
  }
  /* Legacy plaintext password slot (pre-hash migration). Loaded only to
   * trigger one-time migration in remote.c; never re-persisted. */
  if (g_key_file_has_key(key_file, "experimental", "remote_password", NULL)) {
    settings->remote_password = g_key_file_get_string(key_file, "experimental", "remote_password", NULL);
    if (settings->remote_password != NULL && *settings->remote_password == '\0') {
      g_clear_pointer(&settings->remote_password, g_free);
    }
  }
  if (g_key_file_has_key(key_file, "experimental", "remote_hmac_salt", NULL)) {
    settings->remote_hmac_salt = g_key_file_get_string(key_file, "experimental", "remote_hmac_salt", NULL);
    if (settings->remote_hmac_salt != NULL && *settings->remote_hmac_salt == '\0') {
      g_clear_pointer(&settings->remote_hmac_salt, g_free);
    }
  }
  if (g_key_file_has_key(key_file, "experimental", "remote_password_hash", NULL)) {
    settings->remote_password_hash = g_key_file_get_string(key_file, "experimental", "remote_password_hash", NULL);
    if (settings->remote_password_hash != NULL && *settings->remote_password_hash == '\0') {
      g_clear_pointer(&settings->remote_password_hash, g_free);
    }
  }

  skm_settings_clamp(settings);
  return TRUE;
}

gboolean
skm_settings_save(const SkmAppSettings *settings, const gchar *path, GError **error)
{
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  g_autofree gchar *dir = NULL;
  g_autofree gchar *data = NULL;
  gsize length = 0;

  g_return_val_if_fail(settings != NULL, FALSE);
  g_return_val_if_fail(path != NULL, FALSE);

  g_key_file_set_boolean(key_file, "appearance", "oled_black_mode", settings->oled_black_mode);
  g_key_file_set_integer(key_file, "timing", "poll_interval_ms", settings->poll_interval_ms);
  g_key_file_set_integer(key_file, "timing", "fan_debounce_ms", settings->fan_debounce_ms);
  g_key_file_set_boolean(key_file, "experimental", "remote_enabled", settings->remote_enabled);
  g_key_file_set_integer(key_file, "experimental", "remote_port", settings->remote_port);
  /* Only persist the hash + salt — the plaintext password is intentionally
   * dropped so it can never be recovered from disk. */
  if (settings->remote_hmac_salt != NULL) {
    g_key_file_set_string(key_file, "experimental", "remote_hmac_salt", settings->remote_hmac_salt);
  }
  if (settings->remote_password_hash != NULL) {
    g_key_file_set_string(key_file, "experimental", "remote_password_hash", settings->remote_password_hash);
  }

  dir = g_path_get_dirname(path);
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    g_set_error(
      error,
      G_FILE_ERROR,
      g_file_error_from_errno(errno),
      "Failed creating settings directory `%s`.",
      dir);
    return FALSE;
  }

  data = g_key_file_to_data(key_file, &length, NULL);
  if (!g_file_set_contents(path, data, length, error)) {
    return FALSE;
  }

  /* settings.ini may contain an HMAC-SHA256 hash of the user's remote
   * password; restrict it to the owning user regardless of umask. */
  if (g_chmod(path, 0600) != 0 && errno != ENOENT) {
    /* non-fatal: log via GError only if the caller cares enough to pass it */
  }

  return TRUE;
}
