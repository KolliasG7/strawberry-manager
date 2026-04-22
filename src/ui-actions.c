#include "skm-ui-private.h"

#include <math.h>

typedef gpointer (*SkmWorkerFunc)(SkmAppWindow *self, gpointer data);
typedef void (*SkmFinishFunc)(SkmAppWindow *self, gpointer result, gpointer data);

typedef struct {
  SkmAppWindow *self;
  SkmWorkerFunc worker;
  SkmFinishFunc finish;
  gpointer data;
  GDestroyNotify destroy_data;
} SkmTaskCtx;

typedef struct {
  GtkWidget *button;
  gint threshold;
} SkmFanApplyData;

typedef struct {
  GtkWidget *button;
} SkmButtonOnlyData;

typedef struct {
  GtkWidget *button;
  gchar *effect;
  gboolean thermal_mode;
  gint thermal_interval_ms;
} SkmLedApplyData;

typedef struct {
  GtkWidget *button;
  gboolean enabled;
} SkmGpuManualData;

typedef struct {
  GtkWidget *button;
  gint index;
} SkmGpuLevelData;

static gpointer skm_worker_snapshot(SkmAppWindow *self, gpointer data);
static void skm_finish_snapshot(SkmAppWindow *self, gpointer result, gpointer data);
static void skm_task_ctx_free(SkmTaskCtx *ctx);
static void skm_run_task(SkmAppWindow *self, SkmWorkerFunc worker, SkmFinishFunc finish, gpointer data, GDestroyNotify destroy_data);
static void skm_handle_operation_result(SkmAppWindow *self,
                                        SkmOperationResult *result,
                                        GtkWidget *button,
                                        SkmDirtyKind dirty_kind,
                                        gboolean update_reprobe_timestamp);
static gpointer skm_worker_apply_fan(SkmAppWindow *self, gpointer data);
static void skm_finish_apply_fan(SkmAppWindow *self, gpointer result, gpointer data);
static gpointer skm_worker_reset_fan(SkmAppWindow *self, gpointer data);
static void skm_finish_reset_fan(SkmAppWindow *self, gpointer result, gpointer data);
static gpointer skm_worker_apply_led(SkmAppWindow *self, gpointer data);
static void skm_finish_apply_led(SkmAppWindow *self, gpointer result, gpointer data);
static gpointer skm_worker_reset_led(SkmAppWindow *self, gpointer data);
static void skm_finish_reset_led(SkmAppWindow *self, gpointer result, gpointer data);
static gpointer skm_worker_set_gpu_manual(SkmAppWindow *self, gpointer data);
static void skm_finish_set_gpu_manual(SkmAppWindow *self, gpointer result, gpointer data);
static gpointer skm_worker_apply_gpu_level(SkmAppWindow *self, gpointer data);
static void skm_finish_apply_gpu_level(SkmAppWindow *self, gpointer result, gpointer data);
static gpointer skm_worker_reset_gpu(SkmAppWindow *self, gpointer data);
static void skm_finish_reset_gpu(SkmAppWindow *self, gpointer result, gpointer data);
static gpointer skm_worker_reprobe_hdmi(SkmAppWindow *self, gpointer data);
static void skm_finish_reprobe_hdmi(SkmAppWindow *self, gpointer result, gpointer data);
static gboolean skm_fan_debounce_cb(gpointer user_data);
static void skm_led_apply_data_free(SkmLedApplyData *data);

static gpointer
skm_worker_snapshot(SkmAppWindow *self, gpointer data)
{
  (void) data;
  return skm_service_read_snapshot(self->service);
}

static void
skm_finish_snapshot(SkmAppWindow *self, gpointer result, gpointer data)
{
  SkmSnapshot *snapshot = result;

  (void) data;

  self->refresh_in_flight = FALSE;
  skm_update_system(self, &snapshot->system);
  skm_update_fan(self, &snapshot->fan);
  skm_update_led(self, &snapshot->led);
  skm_update_gpu(self, &snapshot->gpu);
  skm_update_hdmi(self, &snapshot->hdmi);
  skm_snapshot_free(snapshot);
}

static void
skm_task_worker(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
  SkmTaskCtx *ctx = task_data;
  gpointer result = NULL;

  (void) source_object;
  (void) cancellable;

  result = ctx->worker(ctx->self, ctx->data);
  g_task_return_pointer(task, result, NULL);
}

static void
skm_task_done(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  SkmTaskCtx *ctx = user_data;
  gpointer result = NULL;

  (void) source_object;

  result = g_task_propagate_pointer(G_TASK(res), NULL);
  ctx->finish(ctx->self, result, ctx->data);
  skm_task_ctx_free(ctx);
}

static void
skm_task_ctx_free(SkmTaskCtx *ctx)
{
  if (ctx == NULL) {
    return;
  }

  if (ctx->destroy_data != NULL) {
    ctx->destroy_data(ctx->data);
  }
  g_free(ctx);
}

static void
skm_run_task(SkmAppWindow *self, SkmWorkerFunc worker, SkmFinishFunc finish, gpointer data, GDestroyNotify destroy_data)
{
  GTask *task = NULL;
  SkmTaskCtx *ctx = g_new0(SkmTaskCtx, 1);

  ctx->self = self;
  ctx->worker = worker;
  ctx->finish = finish;
  ctx->data = data;
  ctx->destroy_data = destroy_data;

  task = g_task_new(self->window, self->cancellable, skm_task_done, ctx);
  g_task_set_task_data(task, ctx, NULL);
  g_task_run_in_thread(task, skm_task_worker);
  g_object_unref(task);
}

static void
skm_handle_operation_result(SkmAppWindow *self,
                            SkmOperationResult *result,
                            GtkWidget *button,
                            SkmDirtyKind dirty_kind,
                            gboolean update_reprobe_timestamp)
{
  if (result->success) {
    if (dirty_kind == SKM_DIRTY_FAN) {
      self->fan_dirty = FALSE;
    } else if (dirty_kind == SKM_DIRTY_LED) {
      self->led_dirty = FALSE;
    } else if (dirty_kind == SKM_DIRTY_GPU) {
      self->gpu_dirty = FALSE;
    }

    if (update_reprobe_timestamp) {
      g_clear_pointer(&self->last_reprobe_at, g_date_time_unref);
      self->last_reprobe_at = g_date_time_new_now_local();
      skm_show_notice(self, "notice-success", "HDMI reprobe triggered.");
    } else {
      skm_show_notice(self, "notice-success", result->message);
    }
    skm_flash_button(button, TRUE);
    skm_refresh_dashboard(self);
  } else {
    skm_show_notice(self, "notice-error", result->message);
    skm_flash_button(button, FALSE);
  }

  skm_operation_result_free(result);
}

static gpointer
skm_worker_apply_fan(SkmAppWindow *self, gpointer data)
{
  SkmFanApplyData *params = data;

  return skm_service_apply_fan(self->service, params->threshold);
}

static void
skm_finish_apply_fan(SkmAppWindow *self, gpointer result, gpointer data)
{
  SkmFanApplyData *params = data;

  skm_handle_operation_result(self, result, params->button, SKM_DIRTY_FAN, FALSE);
}

static gpointer
skm_worker_reset_fan(SkmAppWindow *self, gpointer data)
{
  (void) data;
  return skm_service_reset_fan_defaults(self->service);
}

static void
skm_finish_reset_fan(SkmAppWindow *self, gpointer result, gpointer data)
{
  SkmButtonOnlyData *params = data;

  skm_handle_operation_result(self, result, params->button, SKM_DIRTY_FAN, FALSE);
}

static gpointer
skm_worker_apply_led(SkmAppWindow *self, gpointer data)
{
  SkmLedApplyData *params = data;

  return skm_service_apply_led(
    self->service,
    params->effect,
    params->thermal_mode,
    params->thermal_interval_ms);
}

static void
skm_finish_apply_led(SkmAppWindow *self, gpointer result, gpointer data)
{
  SkmLedApplyData *params = data;

  skm_handle_operation_result(self, result, params->button, SKM_DIRTY_LED, FALSE);
}

static gpointer
skm_worker_reset_led(SkmAppWindow *self, gpointer data)
{
  (void) data;
  return skm_service_reset_led_defaults(self->service);
}

static void
skm_finish_reset_led(SkmAppWindow *self, gpointer result, gpointer data)
{
  SkmButtonOnlyData *params = data;

  skm_handle_operation_result(self, result, params->button, SKM_DIRTY_LED, FALSE);
}

static gpointer
skm_worker_set_gpu_manual(SkmAppWindow *self, gpointer data)
{
  SkmGpuManualData *params = data;

  return skm_service_set_gpu_manual(self->service, params->enabled);
}

static void
skm_finish_set_gpu_manual(SkmAppWindow *self, gpointer result, gpointer data)
{
  SkmGpuManualData *params = data;

  skm_handle_operation_result(self, result, params->button, SKM_DIRTY_NONE, FALSE);
}

static gpointer
skm_worker_apply_gpu_level(SkmAppWindow *self, gpointer data)
{
  SkmGpuLevelData *params = data;

  return skm_service_apply_gpu_level(self->service, params->index);
}

static void
skm_finish_apply_gpu_level(SkmAppWindow *self, gpointer result, gpointer data)
{
  SkmGpuLevelData *params = data;

  skm_handle_operation_result(self, result, params->button, SKM_DIRTY_GPU, FALSE);
}

static gpointer
skm_worker_reset_gpu(SkmAppWindow *self, gpointer data)
{
  (void) data;
  return skm_service_reset_gpu_defaults(self->service);
}

static void
skm_finish_reset_gpu(SkmAppWindow *self, gpointer result, gpointer data)
{
  SkmButtonOnlyData *params = data;

  skm_handle_operation_result(self, result, params->button, SKM_DIRTY_GPU, FALSE);
}

static gpointer
skm_worker_reprobe_hdmi(SkmAppWindow *self, gpointer data)
{
  (void) data;
  return skm_service_reprobe_display(self->service);
}

static void
skm_finish_reprobe_hdmi(SkmAppWindow *self, gpointer result, gpointer data)
{
  SkmButtonOnlyData *params = data;

  skm_handle_operation_result(self, result, params->button, SKM_DIRTY_NONE, TRUE);
}

void
skm_refresh_dashboard(SkmAppWindow *self)
{
  if (self->refresh_in_flight) {
    return;
  }

  self->refresh_in_flight = TRUE;
  skm_run_task(self, skm_worker_snapshot, skm_finish_snapshot, NULL, NULL);
}

gboolean
skm_poll_cb(gpointer user_data)
{
  skm_refresh_dashboard(user_data);
  return G_SOURCE_CONTINUE;
}

static gboolean
skm_fan_debounce_cb(gpointer user_data)
{
  SkmAppWindow *self = user_data;
  SkmFanApplyData *params = g_new0(SkmFanApplyData, 1);

  self->fan_debounce_source_id = 0;
  params->button = self->fan_apply_button;
  params->threshold = (gint) lround(gtk_range_get_value(GTK_RANGE(self->fan_threshold_scale)));
  skm_run_task(self, skm_worker_apply_fan, skm_finish_apply_fan, params, g_free);
  return G_SOURCE_REMOVE;
}

void
skm_on_fan_threshold_changed(GtkRange *range, gpointer user_data)
{
  SkmAppWindow *self = user_data;
  gint threshold = (gint) lround(gtk_range_get_value(range));
  g_autofree gchar *text = g_strdup_printf("%d C", threshold);

  gtk_label_set_text(GTK_LABEL(self->fan_threshold_value), text);
  if (self->fan_syncing) {
    return;
  }

  self->fan_dirty = TRUE;
  if (self->fan_debounce_source_id != 0) {
    g_source_remove(self->fan_debounce_source_id);
  }
  self->fan_debounce_source_id = g_timeout_add(self->settings.fan_debounce_ms, skm_fan_debounce_cb, self);
}

void
skm_on_fan_apply_clicked(GtkButton *button, gpointer user_data)
{
  SkmAppWindow *self = user_data;
  SkmFanApplyData *params = g_new0(SkmFanApplyData, 1);

  (void) button;

  params->button = self->fan_apply_button;
  params->threshold = (gint) lround(gtk_range_get_value(GTK_RANGE(self->fan_threshold_scale)));
  skm_run_task(self, skm_worker_apply_fan, skm_finish_apply_fan, params, g_free);
}

void
skm_on_fan_reset_clicked(GtkButton *button, gpointer user_data)
{
  SkmAppWindow *self = user_data;
  SkmButtonOnlyData *params = g_new0(SkmButtonOnlyData, 1);

  (void) button;

  params->button = self->fan_reset_button;
  skm_run_task(self, skm_worker_reset_fan, skm_finish_reset_fan, params, g_free);
}

void
skm_on_led_effect_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
  SkmAppWindow *self = user_data;

  (void) object;
  (void) pspec;

  if (self->led_syncing) {
    return;
  }
  self->led_dirty = TRUE;
}

void
skm_on_led_thermal_changed(GtkToggleButton *button, gpointer user_data)
{
  SkmAppWindow *self = user_data;
  (void) button;

  skm_update_led_control_sensitivity(self);

  if (self->led_syncing) {
    return;
  }
  self->led_dirty = TRUE;
}

void
skm_on_led_interval_changed(GtkSpinButton *spin, gpointer user_data)
{
  SkmAppWindow *self = user_data;

  (void) spin;

  if (self->led_syncing) {
    return;
  }
  self->led_dirty = TRUE;
}

void
skm_on_led_apply_clicked(GtkButton *button, gpointer user_data)
{
  SkmAppWindow *self = user_data;
  SkmLedApplyData *params = g_new0(SkmLedApplyData, 1);
  guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->led_effect_dropdown));
  GListModel *list_model = gtk_drop_down_get_model(GTK_DROP_DOWN(self->led_effect_dropdown));
  GtkStringList *string_list = GTK_IS_STRING_LIST(list_model) ? GTK_STRING_LIST(list_model) : NULL;
  const gchar *effect = string_list != NULL
    ? gtk_string_list_get_string(string_list, selected)
    : "off";

  (void) button;

  params->button = self->led_apply_button;
  params->effect = g_strdup(effect != NULL ? effect : "off");
  params->thermal_mode = skm_toggle_pill_get_active(self->led_thermal_switch);
  params->thermal_interval_ms = gtk_widget_get_visible(self->led_interval_row)
    ? gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(self->led_interval_spin))
    : 2000;

  skm_run_task(self, skm_worker_apply_led, skm_finish_apply_led, params, (GDestroyNotify) skm_led_apply_data_free);
}

static void
skm_led_apply_data_free(SkmLedApplyData *data)
{
  if (data == NULL) {
    return;
  }

  g_clear_pointer(&data->effect, g_free);
  g_free(data);
}

void
skm_on_gpu_level_toggled(GtkCheckButton *check, gpointer user_data)
{
  SkmAppWindow *self = user_data;

  if (self->gpu_syncing || !gtk_check_button_get_active(check)) {
    return;
  }

  self->gpu_dirty = TRUE;
  self->gpu_selected_valid = TRUE;
  self->gpu_selected_index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(check), "skm-level-index"));
}

void
skm_on_led_reset_clicked(GtkButton *button, gpointer user_data)
{
  SkmAppWindow *self = user_data;
  SkmButtonOnlyData *params = g_new0(SkmButtonOnlyData, 1);

  (void) button;

  params->button = self->led_reset_button;
  skm_run_task(self, skm_worker_reset_led, skm_finish_reset_led, params, g_free);
}

void
skm_on_gpu_manual_changed(GtkToggleButton *button, gpointer user_data)
{
  SkmAppWindow *self = user_data;
  SkmGpuManualData *params = NULL;

  if (self->gpu_syncing) {
    return;
  }

  params = g_new0(SkmGpuManualData, 1);
  params->button = self->gpu_apply_button;
  params->enabled = gtk_toggle_button_get_active(button);
  skm_run_task(self, skm_worker_set_gpu_manual, skm_finish_set_gpu_manual, params, g_free);
}

void
skm_on_gpu_apply_clicked(GtkButton *button, gpointer user_data)
{
  SkmAppWindow *self = user_data;
  SkmGpuLevelData *params = NULL;

  (void) button;

  if (!self->gpu_selected_valid) {
    skm_show_notice(self, "notice-error", "Pick an SCLK level first.");
    skm_flash_button(self->gpu_apply_button, FALSE);
    return;
  }

  params = g_new0(SkmGpuLevelData, 1);
  params->button = self->gpu_apply_button;
  params->index = self->gpu_selected_index;
  skm_run_task(self, skm_worker_apply_gpu_level, skm_finish_apply_gpu_level, params, g_free);
}

void
skm_on_gpu_auto_clicked(GtkButton *button, gpointer user_data)
{
  SkmAppWindow *self = user_data;
  SkmGpuManualData *params = g_new0(SkmGpuManualData, 1);

  (void) button;

  params->button = self->gpu_auto_button;
  params->enabled = FALSE;
  skm_run_task(self, skm_worker_set_gpu_manual, skm_finish_set_gpu_manual, params, g_free);
}

void
skm_on_gpu_reset_clicked(GtkButton *button, gpointer user_data)
{
  SkmAppWindow *self = user_data;
  SkmButtonOnlyData *params = g_new0(SkmButtonOnlyData, 1);

  (void) button;

  params->button = self->gpu_reset_button;
  skm_run_task(self, skm_worker_reset_gpu, skm_finish_reset_gpu, params, g_free);
}

void
skm_on_hdmi_reprobe_clicked(GtkButton *button, gpointer user_data)
{
  SkmAppWindow *self = user_data;
  SkmButtonOnlyData *params = g_new0(SkmButtonOnlyData, 1);

  (void) button;

  params->button = self->hdmi_reprobe_button;
  skm_run_task(self, skm_worker_reprobe_hdmi, skm_finish_reprobe_hdmi, params, g_free);
}
