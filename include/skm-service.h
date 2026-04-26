#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SkmService SkmService;

typedef struct {
  gboolean success;
  gboolean permission_denied;
  gchar *message;
} SkmOperationResult;

typedef struct {
  gboolean available;
  gboolean has_temperature_c;
  gboolean has_rpm;
  gboolean has_threshold_c;
  gchar *message;
  gdouble temperature_c;
  gint rpm;
  gint threshold_c;
} SkmFanState;

typedef struct {
  gboolean available;
  gboolean has_active_effect;
  gboolean thermal_mode_supported;
  gboolean has_thermal_mode;
  gboolean has_thermal_interval_ms;
  gchar *message;
  gchar *active_effect;
  GPtrArray *effect_options;
  gboolean thermal_mode;
  gint thermal_interval_ms;
} SkmLedState;

typedef struct {
  gint index;
  gchar *label;
  gint mhz;
  gboolean has_mhz;
  gboolean active;
} SkmGpuLevel;

typedef struct {
  gboolean available;
  gboolean supported_hardware;
  gboolean has_active_level;
  gchar *message;
  gchar *variant;
  gchar *vendor_id;
  gchar *device_id;
  gchar *performance_level;
  gchar *warning;
  GPtrArray *levels;
  gint active_level;
} SkmGpuState;

typedef struct {
  gboolean available;
  gchar *message;
  gchar *connector_name;
  gchar *status;
} SkmHdmiState;

typedef struct {
  gchar *kernel_version;
  gchar *hardware_variant;
  gchar *cpu_governor;
  gchar *uptime;
} SkmSystemInfoState;

typedef struct {
  SkmSystemInfoState system;
  SkmFanState fan;
  SkmLedState led;
  SkmGpuState gpu;
  SkmHdmiState hdmi;
} SkmSnapshot;

enum {
  SKM_FAN_THRESHOLD_MIN = -10,
  SKM_FAN_THRESHOLD_MAX = 85,
  SKM_FAN_THRESHOLD_DEFAULT = 79,
};

SkmService *skm_service_new(const gchar *sys_root, const gchar *proc_root);
void skm_service_free(SkmService *self);

SkmSnapshot *skm_service_read_snapshot(SkmService *self);
SkmOperationResult *skm_service_apply_fan(SkmService *self, gint threshold_c);
SkmOperationResult *skm_service_reset_fan_defaults(SkmService *self);
SkmOperationResult *skm_service_apply_led(SkmService *self, const gchar *effect, gboolean thermal_mode, gint thermal_interval_ms);
SkmOperationResult *skm_service_reset_led_defaults(SkmService *self);
SkmOperationResult *skm_service_set_gpu_manual(SkmService *self, gboolean enabled);
SkmOperationResult *skm_service_apply_gpu_level(SkmService *self, gint index);
SkmOperationResult *skm_service_reset_gpu_defaults(SkmService *self);
SkmOperationResult *skm_service_reprobe_display(SkmService *self);

void skm_operation_result_free(SkmOperationResult *result);
void skm_snapshot_free(SkmSnapshot *snapshot);
void skm_gpu_level_free(SkmGpuLevel *level);

G_END_DECLS
