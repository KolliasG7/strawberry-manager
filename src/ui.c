#include "skm-ui-private.h"

static gboolean
skm_on_close_request(GtkWindow *window, gpointer user_data)
{
  SkmAppWindow *self = user_data;

  (void) window;

  if (self->poll_source_id != 0) {
    g_source_remove(self->poll_source_id);
    self->poll_source_id = 0;
  }
  if (self->fan_debounce_source_id != 0) {
    g_source_remove(self->fan_debounce_source_id);
    self->fan_debounce_source_id = 0;
  }
  if (self->notice_source_id != 0) {
    g_source_remove(self->notice_source_id);
    self->notice_source_id = 0;
  }
  g_cancellable_cancel(self->cancellable);
  return FALSE;
}

static void
skm_app_window_free(gpointer data)
{
  SkmAppWindow *self = data;

  if (self == NULL) {
    return;
  }

  if (self->poll_source_id != 0) {
    g_source_remove(self->poll_source_id);
  }
  if (self->fan_debounce_source_id != 0) {
    g_source_remove(self->fan_debounce_source_id);
  }
  if (self->notice_source_id != 0) {
    g_source_remove(self->notice_source_id);
  }

  g_clear_object(&self->cancellable);
  g_clear_pointer(&self->last_reprobe_at, g_date_time_unref);
  skm_service_free(self->service);
  g_free(self);
}

static void
skm_build_ui(SkmAppWindow *self)
{
  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroller = gtk_scrolled_window_new();
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  GtkWidget *card = NULL;
  GtkWidget *actions = NULL;
  GtkWidget *unavailable = NULL;
  GtkWidget *hero = NULL;
  GtkWidget *title_box = NULL;
  GtkWidget *grid = NULL;
  GtkWidget *metrics = NULL;

  gtk_widget_add_css_class(root, "app-root");
  gtk_window_set_child(GTK_WINDOW(self->window), root);

  self->notice_revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(self->notice_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  gtk_box_append(GTK_BOX(root), self->notice_revealer);

  self->notice_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->notice_box, "notice-banner");
  gtk_widget_set_margin_top(self->notice_box, 12);
  gtk_widget_set_margin_start(self->notice_box, 16);
  gtk_widget_set_margin_end(self->notice_box, 16);
  gtk_widget_set_margin_bottom(self->notice_box, 12);
  self->notice_label = skm_make_label("", "notice-label", 0.0f, TRUE);
  gtk_box_append(GTK_BOX(self->notice_box), self->notice_label);
  gtk_revealer_set_child(GTK_REVEALER(self->notice_revealer), self->notice_box);

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroller, TRUE);
  gtk_box_append(GTK_BOX(root), scroller);

  gtk_widget_set_margin_top(content, 18);
  gtk_widget_set_margin_bottom(content, 18);
  gtk_widget_set_margin_start(content, 18);
  gtk_widget_set_margin_end(content, 18);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), content);

  card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_add_css_class(card, "glass-card");
  gtk_widget_add_css_class(card, "hero-card");
  hero = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_widget_add_css_class(hero, "aurora-header");
  gtk_box_append(GTK_BOX(hero), skm_make_label("🍓", "hero-logo", 0.0f, FALSE));
  title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand(title_box, TRUE);
  gtk_box_append(GTK_BOX(title_box), skm_make_label("Strawberry Kernel Manager", "hero-title", 0.0f, FALSE));
  gtk_box_append(GTK_BOX(title_box), skm_make_label(
    "Native C + GTK4 control surface for Strawberry PS4 kernels on Liverpool / Gladius hardware.",
    "hero-subtitle",
    0.0f,
    TRUE));
  gtk_box_append(GTK_BOX(hero), title_box);
  gtk_box_append(GTK_BOX(card), hero);

  grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 28);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
  skm_attach_info_row(GTK_GRID(grid), 0, "Kernel", &self->system_kernel_value);
  skm_attach_info_row(GTK_GRID(grid), 1, "Hardware", &self->system_variant_value);
  skm_attach_info_row(GTK_GRID(grid), 2, "CPU governor", &self->system_governor_value);
  skm_attach_info_row(GTK_GRID(grid), 3, "Uptime", &self->system_uptime_value);
  gtk_box_append(GTK_BOX(card), grid);
  gtk_box_append(GTK_BOX(content), card);

  skm_create_section_card(
    "Fan Control",
    "PS4 `ps4_fan` hwmon. Threshold writes `temp1_crit` after 500 ms debounce.",
    &card,
    &self->fan_content,
    &actions,
    &unavailable);
  self->fan_unavailable_label = unavailable;
  metrics = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_append(GTK_BOX(metrics), skm_make_metric_tile("🌡 Temperature", &self->fan_temp_value));
  gtk_box_append(GTK_BOX(metrics), skm_make_metric_tile("RPM", &self->fan_rpm_value));
  gtk_box_append(GTK_BOX(self->fan_content), metrics);
  self->fan_threshold_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, SKM_FAN_THRESHOLD_MIN, SKM_FAN_THRESHOLD_MAX, 1);
  gtk_scale_set_draw_value(GTK_SCALE(self->fan_threshold_scale), FALSE);
  g_signal_connect(self->fan_threshold_scale, "value-changed", G_CALLBACK(skm_on_fan_threshold_changed), self);
  self->fan_threshold_value = skm_make_label("79 C", "value-label", 1.0f, FALSE);
  gtk_box_append(GTK_BOX(self->fan_content), skm_make_control_row("Fan threshold", self->fan_threshold_scale, self->fan_threshold_value));
  self->fan_status_label = skm_make_label("", "dim-label", 0.0f, TRUE);
  gtk_box_append(GTK_BOX(self->fan_content), self->fan_status_label);
  self->fan_reset_button = skm_make_button("Reset to Defaults", "secondary-button");
  self->fan_apply_button = skm_make_button("Apply", "accent-button");
  g_signal_connect(self->fan_reset_button, "clicked", G_CALLBACK(skm_on_fan_reset_clicked), self);
  g_signal_connect(self->fan_apply_button, "clicked", G_CALLBACK(skm_on_fan_apply_clicked), self);
  gtk_box_append(GTK_BOX(actions), self->fan_reset_button);
  gtk_box_append(GTK_BOX(actions), self->fan_apply_button);
  gtk_widget_set_sensitive(self->fan_content, FALSE);
  gtk_box_append(GTK_BOX(content), card);

  skm_create_section_card(
    "Front Panel LED",
    "PS4 LED effects via `/sys/class/leds/ps4:*:status` and thermal mode via `/sys/bus/platform/devices/ps4-led`.",
    &card,
    &self->led_content,
    &actions,
    &unavailable);
  self->led_unavailable_label = unavailable;
  grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
  skm_attach_info_row(GTK_GRID(grid), 0, "Mode", &self->led_mode_value);
  skm_attach_info_row(GTK_GRID(grid), 1, "Active effect", &self->led_active_value);
  gtk_box_append(GTK_BOX(self->led_content), grid);
  self->led_effect_dropdown = gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)), NULL);
  g_signal_connect(self->led_effect_dropdown, "notify::selected", G_CALLBACK(skm_on_led_effect_changed), self);
  gtk_box_append(GTK_BOX(self->led_content), skm_make_control_row("Static effect", self->led_effect_dropdown, NULL));
  self->led_thermal_switch = gtk_switch_new();
  g_signal_connect(self->led_thermal_switch, "notify::active", G_CALLBACK(skm_on_led_thermal_changed), self);
  self->led_thermal_row = skm_make_control_row("Thermal mode", self->led_thermal_switch, NULL);
  gtk_widget_set_visible(self->led_thermal_row, FALSE);
  gtk_box_append(GTK_BOX(self->led_content), self->led_thermal_row);
  self->led_interval_spin = gtk_spin_button_new_with_range(100.0, 60000.0, 100.0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->led_interval_spin), 2000.0);
  g_signal_connect(self->led_interval_spin, "value-changed", G_CALLBACK(skm_on_led_interval_changed), self);
  self->led_interval_row = skm_make_control_row("Thermal interval (ms)", self->led_interval_spin, NULL);
  gtk_widget_set_visible(self->led_interval_row, FALSE);
  gtk_box_append(GTK_BOX(self->led_content), self->led_interval_row);
  self->led_status_label = skm_make_label("", "dim-label", 0.0f, TRUE);
  gtk_box_append(GTK_BOX(self->led_content), self->led_status_label);
  self->led_reset_button = skm_make_button("Reset to Defaults", "secondary-button");
  self->led_apply_button = skm_make_button("Apply", "accent-button");
  g_signal_connect(self->led_reset_button, "clicked", G_CALLBACK(skm_on_led_reset_clicked), self);
  g_signal_connect(self->led_apply_button, "clicked", G_CALLBACK(skm_on_led_apply_clicked), self);
  gtk_box_append(GTK_BOX(actions), self->led_reset_button);
  gtk_box_append(GTK_BOX(actions), self->led_apply_button);
  gtk_widget_set_sensitive(self->led_content, FALSE);
  gtk_box_append(GTK_BOX(content), card);
  skm_update_led_control_sensitivity(self);

  skm_create_section_card(
    "GPU Clock (SCLK)",
    "Forces amdgpu SCLK on PS4 Liverpool / Gladius. Writes confirmed after 500 ms poll.",
    &card,
    &self->gpu_content,
    &actions,
    &unavailable);
  self->gpu_unavailable_label = unavailable;
  self->gpu_warning_label = skm_make_label("", "warning-banner", 0.0f, TRUE);
  gtk_widget_set_visible(self->gpu_warning_label, FALSE);
  gtk_box_append(GTK_BOX(self->gpu_content), self->gpu_warning_label);
  grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
  skm_attach_info_row(GTK_GRID(grid), 0, "Detected GPU", &self->gpu_variant_value);
  skm_attach_info_row(GTK_GRID(grid), 1, "Perf level", &self->gpu_mode_value);
  skm_attach_info_row(GTK_GRID(grid), 2, "Active SCLK", &self->gpu_active_value);
  gtk_box_append(GTK_BOX(self->gpu_content), grid);
  self->gpu_manual_switch = gtk_switch_new();
  g_signal_connect(self->gpu_manual_switch, "notify::active", G_CALLBACK(skm_on_gpu_manual_changed), self);
  gtk_box_append(GTK_BOX(self->gpu_content), skm_make_control_row("Force Manual", self->gpu_manual_switch, NULL));
  self->gpu_levels_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class(self->gpu_levels_box, "level-list");
  gtk_box_append(GTK_BOX(self->gpu_content), self->gpu_levels_box);
  gtk_box_append(GTK_BOX(self->gpu_content), skm_make_label("Use Auto after testing. Unsupported hardware stays read-only.", "dim-label", 0.0f, TRUE));
  self->gpu_reset_button = skm_make_button("Reset to Defaults", "secondary-button");
  self->gpu_auto_button = skm_make_button("Auto", "secondary-button");
  self->gpu_apply_button = skm_make_button("Apply Level", "accent-button");
  g_signal_connect(self->gpu_reset_button, "clicked", G_CALLBACK(skm_on_gpu_reset_clicked), self);
  g_signal_connect(self->gpu_auto_button, "clicked", G_CALLBACK(skm_on_gpu_auto_clicked), self);
  g_signal_connect(self->gpu_apply_button, "clicked", G_CALLBACK(skm_on_gpu_apply_clicked), self);
  gtk_box_append(GTK_BOX(actions), self->gpu_reset_button);
  gtk_box_append(GTK_BOX(actions), self->gpu_auto_button);
  gtk_box_append(GTK_BOX(actions), self->gpu_apply_button);
  gtk_widget_set_sensitive(self->gpu_content, FALSE);
  gtk_box_append(GTK_BOX(content), card);

  skm_create_section_card(
    "Display / HDMI Hotplug",
    "Polling disabled in Strawberry kernel. Use Reprobe after reconnecting monitor.",
    &card,
    &self->hdmi_content,
    &actions,
    &unavailable);
  self->hdmi_unavailable_label = unavailable;
  grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
  skm_attach_info_row(GTK_GRID(grid), 0, "Connector", &self->hdmi_connector_value);
  skm_attach_info_row(GTK_GRID(grid), 1, "Status", &self->hdmi_status_value);
  skm_attach_info_row(GTK_GRID(grid), 2, "Last reprobe", &self->hdmi_last_reprobe_value);
  gtk_box_append(GTK_BOX(self->hdmi_content), grid);
  gtk_box_append(GTK_BOX(self->hdmi_content), skm_make_label(
    "Writing `detect` to connector status triggers EDID re-read and modeset.",
    "dim-label",
    0.0f,
    TRUE));
  self->hdmi_reprobe_button = skm_make_button("Reprobe", "accent-button");
  g_signal_connect(self->hdmi_reprobe_button, "clicked", G_CALLBACK(skm_on_hdmi_reprobe_clicked), self);
  gtk_box_append(GTK_BOX(actions), self->hdmi_reprobe_button);
  gtk_widget_set_sensitive(self->hdmi_content, FALSE);
  gtk_box_append(GTK_BOX(content), card);
}

GtkWindow *
skm_app_window_new(GtkApplication *application)
{
  SkmAppWindow *self = g_new0(SkmAppWindow, 1);

  self->window = GTK_APPLICATION_WINDOW(gtk_application_window_new(application));
  self->service = skm_service_new("/sys", "/proc");
  self->cancellable = g_cancellable_new();

  gtk_window_set_title(GTK_WINDOW(self->window), "🍓 Strawberry Kernel Manager");
  gtk_window_set_default_size(GTK_WINDOW(self->window), 1040, 760);
  gtk_widget_set_size_request(GTK_WIDGET(self->window), 900, 620);

  skm_build_ui(self);
  g_signal_connect(self->window, "close-request", G_CALLBACK(skm_on_close_request), self);
  g_object_set_data_full(G_OBJECT(self->window), "skm-app-window", self, skm_app_window_free);

  skm_refresh_dashboard(self);
  self->poll_source_id = g_timeout_add_seconds(2, skm_poll_cb, self);
  return GTK_WINDOW(self->window);
}
