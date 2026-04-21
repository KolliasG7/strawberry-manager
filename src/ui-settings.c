#include "skm-ui-private.h"

static void
skm_update_remote_status(SkmAppWindow *self, gboolean error, const gchar *message)
{
  gtk_widget_remove_css_class(self->settings_remote_status_label, "remote-status-error");
  gtk_widget_add_css_class(self->settings_remote_status_label, "settings-status");
  if (error) {
    gtk_widget_add_css_class(self->settings_remote_status_label, "remote-status-error");
  }
  gtk_label_set_text(GTK_LABEL(self->settings_remote_status_label), message);
}

static void
skm_on_remote_notice(gpointer user_data, gboolean success, gboolean refresh, const gchar *message)
{
  SkmAppWindow *self = user_data;

  skm_show_notice(self, success ? "notice-success" : "notice-error", message);
  if (refresh) {
    skm_refresh_dashboard(self);
  }
}

void
skm_apply_theme(SkmAppWindow *self)
{
  if (self->settings.oled_black_mode) {
    gtk_widget_add_css_class(self->root, "oled-black");
    gtk_widget_add_css_class(GTK_WIDGET(self->window), "oled-black");
  } else {
    gtk_widget_remove_css_class(self->root, "oled-black");
    gtk_widget_remove_css_class(GTK_WIDGET(self->window), "oled-black");
  }
}

void
skm_sync_settings_controls(SkmAppWindow *self)
{
  self->settings_syncing = TRUE;
  skm_toggle_pill_set_active(self->settings_oled_switch, self->settings.oled_black_mode);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->settings_poll_spin), self->settings.poll_interval_ms);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->settings_fan_debounce_spin), self->settings.fan_debounce_ms);
  skm_toggle_pill_set_active(self->settings_remote_switch, self->settings.remote_enabled);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->settings_remote_port_spin), self->settings.remote_port);
  self->settings_syncing = FALSE;

  if (!self->settings.remote_enabled) {
    g_autofree gchar *message = g_strdup_printf(
      "Strawberry Manager API off. Enable to listen on port %d.",
      self->settings.remote_port);
    skm_update_remote_status(self, FALSE, message);
  }
}

void
skm_restart_poll_timer(SkmAppWindow *self)
{
  if (self->poll_source_id != 0) {
    g_source_remove(self->poll_source_id);
    self->poll_source_id = 0;
  }

  self->poll_source_id = g_timeout_add(self->settings.poll_interval_ms, skm_poll_cb, self);
}

void
skm_persist_settings(SkmAppWindow *self)
{
  g_autoptr(GError) error = NULL;

  if (!skm_settings_save(&self->settings, self->settings_path, &error)) {
    skm_show_notice(self, "notice-error", error->message);
    return;
  }

  if (self->remote_server != NULL) {
    skm_remote_server_sync_settings(self->remote_server, &self->settings, self->settings_path);
  }
}

void
skm_refresh_remote_server(SkmAppWindow *self, gboolean user_initiated)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *message = NULL;

  if (self->remote_server == NULL) {
    self->remote_server = skm_remote_server_new(skm_on_remote_notice, self);
  }
  skm_remote_server_sync_settings(self->remote_server, &self->settings, self->settings_path);

  if (!self->settings.remote_enabled) {
    skm_remote_server_stop(self->remote_server);
    message = g_strdup_printf("Strawberry Manager API off. Enable to listen on port %d.", self->settings.remote_port);
    skm_update_remote_status(self, FALSE, message);
    if (user_initiated) {
      skm_show_notice(self, "notice-success", "Strawberry Manager remote API disabled.");
    }
    return;
  }

  /* skm_remote_server_sync_settings() above has already applied either the
   * fresh plaintext (when the user just changed the password in the Settings
   * dialog) or the stored salt+hash (the usual startup path). Calling
   * set_password(self->settings.remote_password) unconditionally would pass
   * NULL after migration — because sync_settings wipes the in-memory plaintext
   * once it's been absorbed — and that would silently clear auth_token, salt,
   * and hash on every launch, dropping the listener back to loopback-only.
   * So only rotate the password here if the user actually supplied a new
   * plaintext this cycle. */
  if (self->settings.remote_password != NULL &&
      *self->settings.remote_password != '\0') {
    skm_remote_server_set_password(self->remote_server, self->settings.remote_password);
  }

  if (skm_remote_server_is_running(self->remote_server) &&
      skm_remote_server_get_port(self->remote_server) == self->settings.remote_port) {
    message = g_strdup_printf(
      "Listening on port %d for Strawberry Manager. Trusted LAN only.",
      self->settings.remote_port);
    skm_update_remote_status(self, FALSE, message);
    return;
  }

  if (!skm_remote_server_start(self->remote_server, self->settings.remote_port, &error)) {
    message = g_strdup_printf(
      "Could not open port %d: %s",
      self->settings.remote_port,
      error->message);
    skm_update_remote_status(self, TRUE, message);
    if (user_initiated) {
      skm_show_notice(self, "notice-error", message);
    }
    return;
  }

  message = g_strdup_printf(
    "Listening on port %d for Strawberry Manager remote access.",
    self->settings.remote_port);
  skm_update_remote_status(self, FALSE, message);
  if (user_initiated) {
    skm_show_notice(self, "notice-success", message);
  }
}

void
skm_on_settings_oled_changed(GtkToggleButton *button, gpointer user_data)
{
  SkmAppWindow *self = user_data;

  if (self->settings_syncing) {
    return;
  }

  self->settings.oled_black_mode = gtk_toggle_button_get_active(button);
  skm_apply_theme(self);
  skm_persist_settings(self);
}

void
skm_on_settings_poll_interval_changed(GtkSpinButton *spin, gpointer user_data)
{
  SkmAppWindow *self = user_data;

  if (self->settings_syncing) {
    return;
  }

  self->settings.poll_interval_ms = gtk_spin_button_get_value_as_int(spin);
  skm_restart_poll_timer(self);
  skm_persist_settings(self);
  skm_refresh_dashboard(self);
}

void
skm_on_settings_fan_debounce_changed(GtkSpinButton *spin, gpointer user_data)
{
  SkmAppWindow *self = user_data;

  if (self->settings_syncing) {
    return;
  }

  self->settings.fan_debounce_ms = gtk_spin_button_get_value_as_int(spin);
  skm_persist_settings(self);
}

void
skm_on_settings_remote_changed(GtkToggleButton *button, gpointer user_data)
{
  SkmAppWindow *self = user_data;

  if (self->settings_syncing) {
    return;
  }

  self->settings.remote_enabled = gtk_toggle_button_get_active(button);
  skm_refresh_remote_server(self, TRUE);
  skm_persist_settings(self);
}

void
skm_on_settings_remote_port_changed(GtkSpinButton *spin, gpointer user_data)
{
  SkmAppWindow *self = user_data;

  if (self->settings_syncing) {
    return;
  }

  self->settings.remote_port = gtk_spin_button_get_value_as_int(spin);
  skm_refresh_remote_server(self, self->settings.remote_enabled);
  skm_persist_settings(self);
}
