#include "skm-ui-private.h"

static void
skm_set_section_availability(GtkWidget *content, GtkWidget *unavailable, gboolean available, const gchar *message)
{
  gtk_widget_set_sensitive(content, available);
  gtk_widget_set_visible(unavailable, !available);
  gtk_label_set_text(GTK_LABEL(unavailable), message);
}

void
skm_update_led_control_sensitivity(SkmAppWindow *self)
{
  gboolean thermal_mode = gtk_switch_get_active(GTK_SWITCH(self->led_thermal_switch));

  gtk_widget_set_sensitive(self->led_effect_dropdown, !thermal_mode);
  gtk_widget_set_sensitive(self->led_interval_spin, thermal_mode);
}

static void
skm_set_dropdown_strings(GtkDropDown *dropdown, GPtrArray *values)
{
  GtkStringList *model = NULL;
  guint i = 0;

  model = gtk_string_list_new(NULL);
  if (values != NULL) {
    for (i = 0; i < values->len; i++) {
      gtk_string_list_append(model, g_ptr_array_index(values, i));
    }
  }

  gtk_drop_down_set_model(dropdown, G_LIST_MODEL(model));
  g_object_unref(model);
}

static void
skm_rebuild_gpu_levels(SkmAppWindow *self, GPtrArray *levels)
{
  GtkWidget *child = gtk_widget_get_first_child(self->gpu_levels_box);
  GtkCheckButton *first = NULL;
  guint i = 0;

  while (child != NULL) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);

    gtk_box_remove(GTK_BOX(self->gpu_levels_box), child);
    child = next;
  }

  if (levels == NULL || levels->len == 0) {
    gtk_box_append(GTK_BOX(self->gpu_levels_box), skm_make_label("No SCLK levels exposed.", "dim-label", 0.0f, FALSE));
    return;
  }

  self->gpu_syncing = TRUE;
  for (i = 0; i < levels->len; i++) {
    SkmGpuLevel *level = g_ptr_array_index(levels, i);
    GtkWidget *button = NULL;
    g_autofree gchar *text = NULL;

    text = level->has_mhz
      ? g_strdup_printf("%d: %d MHz", level->index, level->mhz)
      : g_strdup_printf("%d: %s", level->index, level->label);

    button = gtk_check_button_new_with_label(text);
    gtk_widget_add_css_class(button, "level-check");
    g_object_set_data(G_OBJECT(button), "skm-level-index", GINT_TO_POINTER(level->index));
    g_signal_connect(button, "toggled", G_CALLBACK(skm_on_gpu_level_toggled), self);

    if (first == NULL) {
      first = GTK_CHECK_BUTTON(button);
    } else {
      gtk_check_button_set_group(GTK_CHECK_BUTTON(button), first);
    }

    gtk_box_append(GTK_BOX(self->gpu_levels_box), button);
  }

  if (!self->gpu_selected_valid && levels->len > 0) {
    SkmGpuLevel *level = g_ptr_array_index(levels, 0);
    self->gpu_selected_valid = TRUE;
    self->gpu_selected_index = level->index;
  }

  child = gtk_widget_get_first_child(self->gpu_levels_box);
  while (child != NULL) {
    gint index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "skm-level-index"));

    if (GTK_IS_CHECK_BUTTON(child)) {
      gtk_check_button_set_active(GTK_CHECK_BUTTON(child), self->gpu_selected_valid && index == self->gpu_selected_index);
    }
    child = gtk_widget_get_next_sibling(child);
  }
  self->gpu_syncing = FALSE;
}

void
skm_update_system(SkmAppWindow *self, const SkmSystemInfoState *state)
{
  gtk_label_set_text(GTK_LABEL(self->system_kernel_value), state->kernel_version);
  gtk_label_set_text(GTK_LABEL(self->system_variant_value), state->hardware_variant);
  gtk_label_set_text(GTK_LABEL(self->system_governor_value), state->cpu_governor);
  gtk_label_set_text(GTK_LABEL(self->system_uptime_value), state->uptime);
}

void
skm_update_fan(SkmAppWindow *self, const SkmFanState *state)
{
  g_autofree gchar *temp_text = NULL;
  g_autofree gchar *rpm_text = NULL;

  skm_set_section_availability(self->fan_content, self->fan_unavailable_label, state->available, state->message);
  gtk_label_set_text(GTK_LABEL(self->fan_status_label), state->message);

  temp_text = state->has_temperature_c ? g_strdup_printf("%.1f C", state->temperature_c) : NULL;
  rpm_text = state->has_rpm ? g_strdup_printf("%d RPM", state->rpm) : NULL;
  gtk_label_set_text(GTK_LABEL(self->fan_temp_value), temp_text != NULL ? temp_text : "—");
  gtk_label_set_text(GTK_LABEL(self->fan_rpm_value), rpm_text != NULL ? rpm_text : "—");

  if (!state->available || self->fan_dirty) {
    return;
  }

  self->fan_syncing = TRUE;
  if (state->has_threshold_c) {
    g_autofree gchar *text = g_strdup_printf("%d C", state->threshold_c);

    gtk_range_set_value(GTK_RANGE(self->fan_threshold_scale), state->threshold_c);
    gtk_label_set_text(GTK_LABEL(self->fan_threshold_value), text);
  }

  self->fan_syncing = FALSE;
}

void
skm_update_led(SkmAppWindow *self, const SkmLedState *state)
{
  skm_set_section_availability(self->led_content, self->led_unavailable_label, state->available, state->message);
  gtk_label_set_text(GTK_LABEL(self->led_status_label), state->message);
  gtk_label_set_text(
    GTK_LABEL(self->led_mode_value),
    state->has_thermal_mode && state->thermal_mode ? "Thermal" : "Static");
  gtk_label_set_text(
    GTK_LABEL(self->led_active_value),
    state->has_active_effect && state->active_effect != NULL ? state->active_effect : "off");

  if (!state->available) {
    gtk_widget_set_visible(self->led_thermal_row, FALSE);
    gtk_widget_set_visible(self->led_interval_row, FALSE);
    return;
  }

  if (!self->led_dirty) {
    guint selected = 0;

    self->led_syncing = TRUE;
    skm_set_dropdown_strings(GTK_DROP_DOWN(self->led_effect_dropdown), state->effect_options);
    if (state->active_effect != NULL) {
      GListModel *list_model = gtk_drop_down_get_model(GTK_DROP_DOWN(self->led_effect_dropdown));
      GtkStringList *string_list = GTK_IS_STRING_LIST(list_model) ? GTK_STRING_LIST(list_model) : NULL;
      guint i = 0;

      for (i = 0; string_list != NULL && i < g_list_model_get_n_items(G_LIST_MODEL(string_list)); i++) {
        const gchar *value = gtk_string_list_get_string(string_list, i);

        if (g_strcmp0(value, state->active_effect) == 0) {
          selected = i;
          break;
        }
      }
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->led_effect_dropdown), selected);

    gtk_widget_set_visible(self->led_thermal_row, state->thermal_mode_supported);
    if (state->has_thermal_mode) {
      gtk_switch_set_active(GTK_SWITCH(self->led_thermal_switch), state->thermal_mode);
    }
    gtk_widget_set_visible(self->led_interval_row, state->has_thermal_interval_ms);
    if (state->has_thermal_interval_ms) {
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->led_interval_spin), state->thermal_interval_ms);
    }
    self->led_syncing = FALSE;
  }
  skm_update_led_control_sensitivity(self);
}

void
skm_update_gpu(SkmAppWindow *self, const SkmGpuState *state)
{
  g_autofree gchar *variant = NULL;
  g_autofree gchar *active = NULL;
  gboolean controls_enabled = state->available && state->supported_hardware;

  skm_set_section_availability(self->gpu_content, self->gpu_unavailable_label, state->available, state->message);
  variant = g_strdup_printf("%s (%s / %s)", state->variant, state->vendor_id, state->device_id);
  gtk_label_set_text(GTK_LABEL(self->gpu_variant_value), variant);
  gtk_label_set_text(GTK_LABEL(self->gpu_mode_value), state->performance_level != NULL ? state->performance_level : "Unavailable");

  if (state->has_active_level) {
    guint i = 0;

    for (i = 0; i < state->levels->len; i++) {
      SkmGpuLevel *level = g_ptr_array_index(state->levels, i);

      if (level->index == state->active_level) {
        active = level->has_mhz
          ? g_strdup_printf("%d: %d MHz", level->index, level->mhz)
          : g_strdup_printf("%d: %s", level->index, level->label);
        break;
      }
    }
  }
  gtk_label_set_text(GTK_LABEL(self->gpu_active_value), active != NULL ? active : "—");

  gtk_widget_set_visible(self->gpu_warning_label, state->warning != NULL && *state->warning != '\0');
  if (state->warning != NULL) {
    gtk_label_set_text(GTK_LABEL(self->gpu_warning_label), state->warning);
  }

  gtk_widget_set_sensitive(self->gpu_manual_switch, controls_enabled);
  gtk_widget_set_sensitive(self->gpu_levels_box, controls_enabled);
  gtk_widget_set_sensitive(self->gpu_apply_button, controls_enabled);
  gtk_widget_set_sensitive(self->gpu_auto_button, controls_enabled);
  gtk_widget_set_sensitive(self->gpu_reset_button, controls_enabled);

  if (!state->available) {
    skm_rebuild_gpu_levels(self, NULL);
    return;
  }

  self->gpu_syncing = TRUE;
  gtk_switch_set_active(
    GTK_SWITCH(self->gpu_manual_switch),
    g_strcmp0(state->performance_level, "manual") == 0);
  self->gpu_syncing = FALSE;

  if (!self->gpu_dirty && state->has_active_level) {
    self->gpu_selected_valid = TRUE;
    self->gpu_selected_index = state->active_level;
  }
  skm_rebuild_gpu_levels(self, state->levels);
}

void
skm_update_hdmi(SkmAppWindow *self, const SkmHdmiState *state)
{
  g_autofree gchar *timestamp = NULL;

  skm_set_section_availability(self->hdmi_content, self->hdmi_unavailable_label, state->available, state->message);
  gtk_label_set_text(GTK_LABEL(self->hdmi_connector_value), state->connector_name != NULL ? state->connector_name : "Unavailable");
  gtk_label_set_text(GTK_LABEL(self->hdmi_status_value), state->status != NULL ? state->status : "Unknown");

  if (self->last_reprobe_at != NULL) {
    timestamp = g_date_time_format(self->last_reprobe_at, "%Y-%m-%d %H:%M:%S");
  }
  gtk_label_set_text(GTK_LABEL(self->hdmi_last_reprobe_value), timestamp != NULL ? timestamp : "Never");
}
