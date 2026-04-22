#define _DEFAULT_SOURCE  /* for DT_DIR/DT_UNKNOWN in dirent, RB_* in sys/reboot */

#include "skm-remote.h"

#include "skm-service-private.h"
#include "skm-sysfs.h"

#include <gio/gio.h>
#include <glib/gstdio.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>

/* Reboot magic constants (from <sys/reboot.h> / <linux/reboot.h>) */
#ifndef RB_AUTOBOOT
# define RB_AUTOBOOT   0x01234567
#endif
#ifndef RB_POWER_OFF
# define RB_POWER_OFF  0x4321fedc
#endif

#define SKM_REMOTE_MAX_CLIENTS 8
#define SKM_WS_MAGIC "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define SKM_REMOTE_PROCESS_LIMIT_DEFAULT 50
#define SKM_REMOTE_PROCESS_LIMIT_MAX 200
#define SKM_REMOTE_OPEN_TOKEN "open-access"

/* Login rate limiting: after this many failures in a row a peer is locked out
 * for SKM_LOGIN_LOCKOUT_SECONDS. Each failed attempt also sleeps briefly to
 * cap wall-clock brute force speed regardless of parallel workers. */
#define SKM_LOGIN_FAIL_LIMIT       10
#define SKM_LOGIN_LOCKOUT_SECONDS  300
#define SKM_LOGIN_FAIL_SLEEP_US    500000  /* 0.5s */

/* HMAC salt length in raw bytes (hex-encoded for on-disk storage). */
#define SKM_HMAC_SALT_BYTES 16

/* Post-response actions deferred until the reply is flushed + connection
 * closed — if we called reboot()/poweroff() inline, the client would see a
 * TCP RST and misreport the command as failed. */
typedef enum {
  SKM_POST_ACTION_NONE = 0,
  SKM_POST_ACTION_REBOOT,
  SKM_POST_ACTION_POWEROFF,
} SkmPostAction;

/* Telemetry push cadence (match legacy behaviour) + ping interval used to
 * detect half-open clients (iOS suspended apps, dropped Wi-Fi, etc.). */
#define SKM_WS_TELEMETRY_INTERVAL_US   2000000  /* 2s payload cadence */
#define SKM_WS_PING_INTERVAL_US       15000000  /* 15s keepalive ping */
#define SKM_WS_POLL_TIMEOUT_MS             200
#define SKM_WS_MAX_CONTROL_PAYLOAD      125       /* RFC 6455 §5.5 */

/* ── CPU delta tracking (protected by g_cpu_mutex) ─────────────────────── */
static GMutex  g_cpu_mutex;
static guint64 g_cpu_prev_total = 0;
static guint64 g_cpu_prev_idle  = 0;
static gdouble g_cpu_percent    = 0.0;

/* ── PTY terminal ────────────────────────────────────────────────────────
 * Each /ws/terminal connection gets its own bash + pty. No global state —
 * two simultaneous clients can no longer see each other's output, and a
 * disconnected shell doesn't persist to leak into the next client. */

typedef struct {
  guint64 rx_bytes;
  guint64 tx_bytes;
} SkmRemoteNetCounter;

typedef struct {
  gint64 locked_until_us; /* 0 = not locked */
  gint   fail_count;
} SkmLoginAttempt;

struct _SkmRemoteServer {
  GThreadedSocketService *service;
  gint port;
  SkmRemoteNoticeFunc notice_cb;
  gpointer notice_user_data;
  GMutex lock;
  GPtrArray *recent_clients;
  guint64 total_connections;
  gchar *last_client;
  gboolean auth_required;
  /* HMAC-SHA256(password, salt) hex digest. Compared against submitted tokens
   * in constant time. The plaintext password is never held in memory after
   * skm_remote_server_set_password() returns. */
  gchar *auth_token;
  GMutex login_lock;
  GHashTable *login_attempts; /* char* peer_ip -> SkmLoginAttempt* */
  gboolean cpu_prev_valid;
  guint64 cpu_prev_total;
  guint64 cpu_prev_idle;
  GArray *cpu_prev_totals;
  GArray *cpu_prev_idles;
  gint64 net_prev_usec;
  GHashTable *net_prev_counters;
  SkmAppSettings settings;
  gchar *settings_path;
};

typedef struct {
  SkmRemoteNoticeFunc notice_cb;
  gpointer notice_user_data;
  gboolean success;
  gboolean refresh;
  gchar *message;
} SkmRemoteNotice;

static void
skm_app_settings_clear(SkmAppSettings *settings)
{
  if (settings == NULL) {
    return;
  }

  g_clear_pointer(&settings->remote_password,      g_free);
  g_clear_pointer(&settings->remote_hmac_salt,     g_free);
  g_clear_pointer(&settings->remote_password_hash, g_free);
}

static void
skm_app_settings_clamp(SkmAppSettings *settings)
{
  if (settings == NULL) {
    return;
  }

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

static void
skm_app_settings_assign(SkmAppSettings *dest, const SkmAppSettings *src)
{
  if (dest == NULL || src == NULL) {
    return;
  }

  dest->oled_black_mode = src->oled_black_mode;
  dest->poll_interval_ms = src->poll_interval_ms;
  dest->fan_debounce_ms = src->fan_debounce_ms;
  dest->remote_enabled = src->remote_enabled;
  dest->remote_port = src->remote_port;
  g_clear_pointer(&dest->remote_password, g_free);
  dest->remote_password = g_strdup(src->remote_password);
  g_clear_pointer(&dest->remote_hmac_salt, g_free);
  dest->remote_hmac_salt = g_strdup(src->remote_hmac_salt);
  g_clear_pointer(&dest->remote_password_hash, g_free);
  dest->remote_password_hash = g_strdup(src->remote_password_hash);
  skm_app_settings_clamp(dest);
}

/* ── Auth helpers ──────────────────────────────────────────────────────── */

static gchar *
skm_hmac_sha256_hex(const gchar *password, const gchar *data)
{
  GHmac *hmac = g_hmac_new(G_CHECKSUM_SHA256,
                            (const guchar *) password, strlen(password));
  g_hmac_update(hmac, (const guchar *) data, -1);
  gchar *hex = g_strdup(g_hmac_get_string(hmac));
  g_hmac_unref(hmac);
  return hex;
}

/* Constant-time string comparison. Walks the longer of the two strings so
 * running time does not leak information about which byte differs. Returns
 * TRUE if both inputs are non-NULL and byte-wise equal. */
static gboolean
skm_token_equal(const gchar *a, const gchar *b)
{
  if (a == NULL || b == NULL) {
    return FALSE;
  }

  gsize la = strlen(a);
  gsize lb = strlen(b);
  gsize n  = la > lb ? la : lb;
  guint diff = (guint) (la ^ lb);

  for (gsize i = 0; i < n; i++) {
    guchar ca = (i < la) ? (guchar) a[i] : 0;
    guchar cb = (i < lb) ? (guchar) b[i] : 0;
    diff |= (guint) (ca ^ cb);
  }

  return diff == 0;
}

/* Generate SKM_HMAC_SALT_BYTES random bytes as a lower-case hex string. */
static gchar *
skm_generate_hmac_salt(void)
{
  guint8 raw[SKM_HMAC_SALT_BYTES];
  GString *hex = NULL;

  for (gsize i = 0; i < SKM_HMAC_SALT_BYTES; i++) {
    raw[i] = (guint8) g_random_int_range(0, 256);
  }
  hex = g_string_sized_new(SKM_HMAC_SALT_BYTES * 2);
  for (gsize i = 0; i < SKM_HMAC_SALT_BYTES; i++) {
    g_string_append_printf(hex, "%02x", raw[i]);
  }
  return g_string_free(hex, FALSE);
}

/* ── CPU% reader (/proc/stat delta) ────────────────────────────────────── */

/* Per-core CPU baseline kept by a WS telemetry subscriber. A single entry
 * covers one `cpuN` line from /proc/stat. The WS handler owns the array
 * and passes it to skm_build_telemetry_json, so each subscriber computes
 * its own delta and they don't fight over shared state. */
typedef struct {
  guint64 prev_total;
  guint64 prev_idle;
} SkmCpuCoreBaseline;

/* Per-interface network byte baseline, same ownership pattern as the CPU
 * core baselines above. `iface` is matched by name; a new interface coming
 * online (e.g. a Wi-Fi dongle plugged in mid-session) is just appended. */
typedef struct {
  gchar   iface[32];
  guint64 prev_rx;
  guint64 prev_tx;
  gint64  prev_ts_us;
} SkmNetBaseline;

/* Opaque bag of caller-owned deltas threaded through skm_build_telemetry_json.
 * Kept on the WS handler's stack; lifetime matches the WS connection. */
typedef struct {
  guint64  cpu_prev_total;
  guint64  cpu_prev_idle;
  GArray  *cores;  /* element-type: SkmCpuCoreBaseline, index = core # */
  GArray  *nets;   /* element-type: SkmNetBaseline */
} SkmTelemetryState;

/* Reads /proc/stat and computes CPU% against the caller's own previous
 * sample. Pass a pair of zero-initialised guint64s the first time — result
 * will be 0.0 on the first call and accurate thereafter. This variant is
 * the preferred one for per-client usage (e.g. each WS telemetry pump)
 * because two clients with different sample rates would fight over the
 * server-global baseline otherwise. */
static gdouble
skm_read_cpu_percent_local(guint64 *prev_total, guint64 *prev_idle)
{
  FILE *f = fopen("/proc/stat", "r");
  guint64 user = 0, nice = 0, system = 0, idle = 0, iowait = 0,
          irq = 0, softirq = 0, steal = 0;
  if (f == NULL) return 0.0;
  if (fscanf(f,
       "cpu %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
       " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
       " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT,
       &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) < 4) {
    fclose(f);
    return 0.0;
  }
  fclose(f);

  guint64 idle_total = idle + iowait;
  guint64 total = user + nice + system + idle_total + irq + softirq + steal;

  if (*prev_total == 0) {
    *prev_total = total;
    *prev_idle  = idle_total;
    return 0.0;
  }
  guint64 dtotal = total - *prev_total;
  guint64 didle  = idle_total - *prev_idle;
  *prev_total = total;
  *prev_idle  = idle_total;
  return dtotal > 0 ? (1.0 - (gdouble) didle / (gdouble) dtotal) * 100.0 : 0.0;
}

/* Reads every `cpuN` line from /proc/stat and fills @out with one gdouble
 * per core, computed against @cores[i]. @cores is grown as needed (matches
 * the kernel's ordering, which is stable). First call yields 0.0s; second
 * and later yield the real per-core %. */
static void
skm_read_cpu_per_core(GArray *cores, GArray *out)
{
  FILE *f = fopen("/proc/stat", "r");
  gchar line[256];
  guint index = 0;
  g_array_set_size(out, 0);
  if (f == NULL) return;
  while (fgets(line, sizeof(line), f)) {
    guint n = 0;
    /* Zero-init so optional fields (iowait/irq/softirq/steal) don't carry
     * garbage when fscanf returns 5–8 instead of 9. */
    guint64 user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (sscanf(line,
         "cpu%u %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
         " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
         " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT,
         &n, &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) < 5) {
      /* stop once we pass the cpuN block — subsequent lines (ctxt, btime…) */
      if (index > 0) break;
      else continue;
    }
    guint64 idle_total = idle + iowait;
    guint64 total = user + nice + system + idle_total + irq + softirq + steal;

    if (cores->len <= index) {
      SkmCpuCoreBaseline zero = { 0 };
      g_array_append_val(cores, zero);
    }
    SkmCpuCoreBaseline *b = &g_array_index(cores, SkmCpuCoreBaseline, index);
    gdouble pct = 0.0;
    if (b->prev_total != 0) {
      guint64 dtotal = total - b->prev_total;
      guint64 didle  = idle_total - b->prev_idle;
      pct = dtotal > 0 ? (1.0 - (gdouble) didle / (gdouble) dtotal) * 100.0 : 0.0;
    }
    b->prev_total = total;
    b->prev_idle  = idle_total;
    g_array_append_val(out, pct);
    index++;
  }
  fclose(f);
}

static gdouble
skm_read_cpu_percent(void)
{
  FILE *f = fopen("/proc/stat", "r");
  unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
  gdouble result;

  if (f == NULL)
    return 0.0;

  if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
             &user, &nice, &system, &idle,
             &iowait, &irq, &softirq, &steal) != 8) {
    fclose(f);
    return 0.0;
  }
  fclose(f);

  guint64 total      = (guint64)(user + nice + system + idle + iowait + irq + softirq + steal);
  guint64 idle_total = (guint64)(idle + iowait);

  g_mutex_lock(&g_cpu_mutex);
  if (g_cpu_prev_total == 0) {
    g_cpu_prev_total = total;
    g_cpu_prev_idle  = idle_total;
    g_mutex_unlock(&g_cpu_mutex);
    return 0.0;
  }
  guint64 dtotal = total - g_cpu_prev_total;
  guint64 didle  = idle_total - g_cpu_prev_idle;
  result = dtotal > 0 ? (1.0 - (gdouble) didle / (gdouble) dtotal) * 100.0 : 0.0;
  g_cpu_percent    = result;
  g_cpu_prev_total = total;
  g_cpu_prev_idle  = idle_total;
  g_mutex_unlock(&g_cpu_mutex);
  return result;
}

/* ── /proc/cpuinfo: get MHz and core count ─────────────────────────────── */

static void
skm_read_cpuinfo(gint *out_cores, gdouble *out_freq_mhz)
{
  FILE *f = fopen("/proc/cpuinfo", "r");
  char line[256];
  gint cores = 0;
  gdouble freq = 0.0;

  if (f == NULL) { *out_cores = 1; *out_freq_mhz = 0.0; return; }
  while (fgets(line, sizeof(line), f)) {
    if (g_str_has_prefix(line, "processor")) cores++;
    else if (g_str_has_prefix(line, "cpu MHz")) {
      char *p = strchr(line, ':');
      if (p) freq = g_ascii_strtod(p + 1, NULL);
    }
  }
  fclose(f);
  *out_cores    = MAX(cores, 1);
  *out_freq_mhz = freq;
}

/* ── /proc/loadavg ─────────────────────────────────────────────────────── */

static void
skm_read_loadavg(gdouble *l1, gdouble *l5, gdouble *l15)
{
  FILE *f = fopen("/proc/loadavg", "r");
  *l1 = *l5 = *l15 = 0.0;
  if (f == NULL) return;
  fscanf(f, "%lf %lf %lf", l1, l5, l15);
  fclose(f);
}

/* ── /proc/meminfo ─────────────────────────────────────────────────────── */

static void
skm_read_meminfo(gdouble *total_mb, gdouble *used_mb, gdouble *available_mb,
                 gdouble *cached_mb, gdouble *buffers_mb, gdouble *percent,
                 gdouble *swap_total_mb, gdouble *swap_used_mb)
{
  FILE *f = fopen("/proc/meminfo", "r");
  char line[128];
  guint64 mem_total = 0, mem_free = 0, mem_available = 0,
          buffers = 0, cached = 0, swap_total = 0, swap_free = 0;

  *total_mb = *used_mb = *available_mb = *cached_mb = *buffers_mb = *percent = 0.0;
  *swap_total_mb = *swap_used_mb = 0.0;

  if (f == NULL) return;
  while (fgets(line, sizeof(line), f)) {
    unsigned long long val;
    if (sscanf(line, "MemTotal: %llu kB",    &val) == 1) mem_total     = (guint64)val;
    else if (sscanf(line, "MemFree: %llu kB",      &val) == 1) mem_free      = (guint64)val;
    else if (sscanf(line, "MemAvailable: %llu kB", &val) == 1) mem_available = (guint64)val;
    else if (sscanf(line, "Buffers: %llu kB",      &val) == 1) buffers       = (guint64)val;
    else if (sscanf(line, "Cached: %llu kB",       &val) == 1) cached        = (guint64)val;
    else if (sscanf(line, "SwapTotal: %llu kB",    &val) == 1) swap_total    = (guint64)val;
    else if (sscanf(line, "SwapFree: %llu kB",     &val) == 1) swap_free     = (guint64)val;
  }
  fclose(f);

  *total_mb     = (gdouble) mem_total     / 1024.0;
  *available_mb = (gdouble) mem_available / 1024.0;
  *cached_mb    = (gdouble) cached        / 1024.0;
  *buffers_mb   = (gdouble) buffers       / 1024.0;
  *used_mb      = *total_mb - *available_mb;
  *percent      = *total_mb > 0 ? (*used_mb / *total_mb) * 100.0 : 0.0;
  *swap_total_mb = (gdouble) swap_total / 1024.0;
  *swap_used_mb  = (gdouble) (swap_total - swap_free) / 1024.0;
  (void) mem_free;
}

/* ── Process list from /proc ────────────────────────────────────────────── */

typedef struct {
  gint    pid;
  gint    threads;
  gchar   comm[64];
  gchar   user[64];
  gchar   status[24];
  gchar   cmdline[256];
  gchar   state;
  gulong  utime;
  gulong  stime;
  gulong  rss_kb;
  gdouble mem_percent;
  gdouble cpu_percent;
} SkmProcEntry;

static const gchar *
skm_proc_state_name(gchar state)
{
  switch (state) {
    case 'R': return "running";
    case 'S': return "sleeping";
    case 'D': return "disk_sleep";
    case 'T': return "stopped";
    case 't': return "tracing_stop";
    case 'Z': return "zombie";
    case 'X': return "dead";
    case 'I': return "idle";
    default:  return "unknown";
  }
}

static void
skm_proc_lookup_user(uid_t uid, gchar *buffer, gsize buffer_size)
{
  struct passwd pwd = { 0 };
  struct passwd *result = NULL;
  gchar scratch[1024];

  if (getpwuid_r(uid, &pwd, scratch, sizeof(scratch), &result) == 0 &&
      result != NULL &&
      result->pw_name != NULL) {
    g_snprintf(buffer, buffer_size, "%s", result->pw_name);
    return;
  }

  g_snprintf(buffer, buffer_size, "%u", (guint) uid);
}

static void
skm_proc_read_details(SkmProcEntry *entry)
{
  gchar proc_dir[64];
  gchar status_path[64];
  gchar cmdline_path[64];
  GStatBuf st;

  g_snprintf(proc_dir, sizeof(proc_dir), "/proc/%d", entry->pid);
  if (g_stat(proc_dir, &st) == 0) {
    skm_proc_lookup_user(st.st_uid, entry->user, sizeof(entry->user));
  }

  g_snprintf(status_path, sizeof(status_path), "/proc/%d/status", entry->pid);
  FILE *status_file = fopen(status_path, "r");
  if (status_file != NULL) {
    gchar line[256];

    while (fgets(line, sizeof(line), status_file) != NULL) {
      if (sscanf(line, "Threads: %d", &entry->threads) == 1) {
        continue;
      }
    }
    fclose(status_file);
  }

  g_snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%d/cmdline", entry->pid);
  FILE *cmdline_file = fopen(cmdline_path, "r");
  if (cmdline_file != NULL) {
    gsize bytes = fread(entry->cmdline, 1, sizeof(entry->cmdline) - 1, cmdline_file);

    fclose(cmdline_file);
    if (bytes > 0) {
      for (gsize i = 0; i < bytes; i++) {
        if (entry->cmdline[i] == '\0') {
          entry->cmdline[i] = ' ';
        }
      }
      entry->cmdline[bytes] = '\0';
      g_strstrip(entry->cmdline);
    }
  }

  if (entry->cmdline[0] == '\0') {
    g_snprintf(entry->cmdline, sizeof(entry->cmdline), "%s", entry->comm);
  }
}

static int
skm_proc_cmp_cpu(const void *a, const void *b)
{
  const SkmProcEntry *pa = a, *pb = b;
  if (pa->cpu_percent > pb->cpu_percent) return -1;
  if (pa->cpu_percent < pb->cpu_percent) return  1;
  return 0;
}

static int
skm_proc_cmp_mem(const void *a, const void *b)
{
  const SkmProcEntry *pa = a, *pb = b;
  if (pa->rss_kb > pb->rss_kb) return -1;
  if (pa->rss_kb < pb->rss_kb) return  1;
  return 0;
}

static int
skm_proc_cmp_pid(const void *a, const void *b)
{
  const SkmProcEntry *pa = a, *pb = b;
  if (pa->pid < pb->pid) return -1;
  if (pa->pid > pb->pid) return  1;
  return 0;
}

static int
skm_proc_cmp_name(const void *a, const void *b)
{
  const SkmProcEntry *pa = a, *pb = b;
  return g_ascii_strcasecmp(pa->comm, pb->comm);
}

static GArray *
skm_read_processes(gint limit, const gchar *sort_by)
{
  GArray *arr = g_array_new(FALSE, TRUE, sizeof(SkmProcEntry));
  DIR *proc = opendir("/proc");
  struct dirent *ent;
  gdouble total_ram_kb = 0.0;
  struct sysinfo sys_info = { 0 };

  if (proc == NULL) return arr;
  if (sysinfo(&sys_info) == 0 && sys_info.totalram > 0) {
    total_ram_kb = ((gdouble) sys_info.totalram * (gdouble) sys_info.mem_unit) / 1024.0;
  }

  /* First sample: read (utime+stime) per PID as of T0. We collect into a
   * GHashTable keyed by PID so T1 can look up each PID's earlier tick
   * count in O(1). Total-ticks snapshots /proc/stat at both samples so the
   * per-process fraction has the right denominator. */
  GHashTable *t0_ticks = g_hash_table_new(g_direct_hash, g_direct_equal);
  guint64 t0_total = 0, t1_total = 0;
  {
    FILE *sf = fopen("/proc/stat", "r");
    if (sf != NULL) {
      /* Zero-init so a short fscanf (older kernels lack iowait/irq/softirq/
       * steal) can't contribute indeterminate bits to the total. */
      guint64 u = 0, n = 0, s = 0, id = 0, iw = 0, ir = 0, sir = 0, st = 0;
      if (fscanf(sf,
           "cpu %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
           " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
           " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT,
           &u, &n, &s, &id, &iw, &ir, &sir, &st) >= 4) {
        t0_total = u + n + s + id + iw + ir + sir + st;
      }
      fclose(sf);
    }
  }
  while ((ent = readdir(proc)) != NULL) {
    if (ent->d_type != DT_DIR && ent->d_type != DT_UNKNOWN) continue;
    if (!g_ascii_isdigit(ent->d_name[0])) continue;
    gint pid = atoi(ent->d_name);
    if (pid <= 0) continue;
    gchar stat_path[64];
    g_snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
    FILE *sf = fopen(stat_path, "r");
    if (sf == NULL) continue;
    gulong u0 = 0, s0 = 0;
    if (fscanf(sf, "%*d (%*[^)]) %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
               &u0, &s0) >= 2) {
      g_hash_table_insert(t0_ticks,
                          GINT_TO_POINTER(pid),
                          GSIZE_TO_POINTER((gsize)(u0 + s0)));
    }
    fclose(sf);
  }
  closedir(proc);

  /* Sleep ~100ms to give busy processes a detectable tick delta. This is
   * long enough to see 1 tick at HZ=100, short enough that the iOS app
   * doesn't notice — well under one telemetry-frame interval. */
  g_usleep(100 * 1000);

  /* Second sample — this is what actually populates the returned array. */
  proc = opendir("/proc");
  if (proc == NULL) {
    g_hash_table_destroy(t0_ticks);
    return arr;
  }
  {
    FILE *sf = fopen("/proc/stat", "r");
    if (sf != NULL) {
      /* Zero-init mirrors the T0 block above — same fscanf-short hazard. */
      guint64 u = 0, n = 0, s = 0, id = 0, iw = 0, ir = 0, sir = 0, st = 0;
      if (fscanf(sf,
           "cpu %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
           " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
           " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT,
           &u, &n, &s, &id, &iw, &ir, &sir, &st) >= 4) {
        t1_total = u + n + s + id + iw + ir + sir + st;
      }
      fclose(sf);
    }
  }
  guint64 dtotal = t1_total > t0_total ? t1_total - t0_total : 0;

  while ((ent = readdir(proc)) != NULL) {
    if (ent->d_type != DT_DIR && ent->d_type != DT_UNKNOWN) continue;
    if (!g_ascii_isdigit(ent->d_name[0])) continue;

    gint pid = atoi(ent->d_name);
    if (pid <= 0) continue;

    gchar stat_path[64];
    g_snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
    FILE *sf = fopen(stat_path, "r");
    if (sf == NULL) continue;

    SkmProcEntry e = { 0 };
    e.pid = pid;
    char raw_comm[64] = "";
    char state_c = 'U';
    gulong utime = 0, stime = 0;
    long rss = 0;

    /* /proc/PID/stat: pid (comm) state ppid ... utime(14) stime(15) ... rss(24) */
    if (fscanf(sf, "%*d (%63[^)]) %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %*u %*u %ld",
               raw_comm, &state_c, &utime, &stime, &rss) >= 4) {
      g_snprintf(e.comm, sizeof(e.comm), "%s", raw_comm);
      e.state = state_c;
      g_snprintf(e.status, sizeof(e.status), "%s", skm_proc_state_name(state_c));
      e.utime = utime;
      e.stime = stime;
      /* rss is in pages */
      e.rss_kb = (gulong)rss * (gulong)(sysconf(_SC_PAGESIZE) / 1024);
      e.mem_percent = total_ram_kb > 0.0 ? MIN(((gdouble) e.rss_kb / total_ram_kb) * 100.0, 100.0) : 0.0;
      /* Real CPU%: paired delta against the T0 snapshot. Processes that
       * didn't exist at T0 (fork'd during our 100ms sleep) simply look
       * like they ran for the full 100ms window, which is close enough. */
      gpointer prev_any = g_hash_table_lookup(t0_ticks, GINT_TO_POINTER(pid));
      gulong prev_ticks = (gulong) GPOINTER_TO_SIZE(prev_any);
      gulong cur_ticks  = (gulong)(utime + stime);
      gulong dticks     = cur_ticks > prev_ticks ? cur_ticks - prev_ticks : 0;
      e.cpu_percent = dtotal > 0
        ? MIN(((gdouble) dticks / (gdouble) dtotal) * 100.0, 100.0)
        : 0.0;
      skm_proc_read_details(&e);
      g_array_append_val(arr, e);
    }
    fclose(sf);
  }
  closedir(proc);
  g_hash_table_destroy(t0_ticks);

  if (g_strcmp0(sort_by, "mem") == 0) {
    qsort(arr->data, arr->len, sizeof(SkmProcEntry), skm_proc_cmp_mem);
  } else if (g_strcmp0(sort_by, "pid") == 0) {
    qsort(arr->data, arr->len, sizeof(SkmProcEntry), skm_proc_cmp_pid);
  } else if (g_strcmp0(sort_by, "name") == 0) {
    qsort(arr->data, arr->len, sizeof(SkmProcEntry), skm_proc_cmp_name);
  } else {
    qsort(arr->data, arr->len, sizeof(SkmProcEntry), skm_proc_cmp_cpu);
  }

  if (arr->len > (guint) MIN(limit, SKM_REMOTE_PROCESS_LIMIT_MAX)) {
    g_array_set_size(arr, MIN(limit, SKM_REMOTE_PROCESS_LIMIT_MAX));
  }
  return arr;
}

/* ── /proc/net/dev: per-interface byte counters ─────────────────────────── */

typedef struct {
  gchar   iface[32];
  guint64 rx_bytes;
  guint64 tx_bytes;
} SkmNetIface;

static GArray *
skm_read_net_dev(void)
{
  GArray *arr = g_array_new(FALSE, TRUE, sizeof(SkmNetIface));
  FILE *f = fopen("/proc/net/dev", "r");
  char line[256];
  if (f == NULL) return arr;
  /* skip 2 header lines */
  fgets(line, sizeof(line), f);
  fgets(line, sizeof(line), f);
  while (fgets(line, sizeof(line), f)) {
    SkmNetIface n = { 0 };
    unsigned long long rx, tx;
    char *colon = strchr(line, ':');
    if (!colon) continue;
    *colon = '\0';
    g_snprintf(n.iface, sizeof(n.iface), "%s", g_strstrip(line));
    if (sscanf(colon + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &rx, &tx) == 2) {
      n.rx_bytes = (guint64)rx;
      n.tx_bytes = (guint64)tx;
      g_array_append_val(arr, n);
    }
  }
  fclose(f);
  return arr;
}

/* Look up (or lazily append) the per-interface baseline so the caller can
 * compute bytes/s against its own previous sample. Matching is by name —
 * interfaces don't get renumbered at runtime, and the kernel reuses names. */
static SkmNetBaseline *
skm_net_baseline_get(GArray *nets, const gchar *iface)
{
  for (guint i = 0; i < nets->len; i++) {
    SkmNetBaseline *b = &g_array_index(nets, SkmNetBaseline, i);
    if (g_strcmp0(b->iface, iface) == 0) return b;
  }
  SkmNetBaseline fresh = { 0 };
  g_snprintf(fresh.iface, sizeof(fresh.iface), "%s", iface);
  g_array_append_val(nets, fresh);
  return &g_array_index(nets, SkmNetBaseline, nets->len - 1);
}

/* ── Tunnel probe (/proc scan for cloudflared / tailscaled) ─────────────── */

/* Scan /proc/<pid>/comm for any process whose basename matches @names.
 * Returns TRUE as soon as one is found; FALSE if none of the N names are
 * live. This is a cheap substitute for `pgrep` that avoids spawning
 * anything and works on the stripped userspace a PS4 Linux distro
 * usually ships. */
static gboolean
skm_tunnel_any_running(const gchar * const *names)
{
  DIR *proc = opendir("/proc");
  struct dirent *ent;
  gboolean found = FALSE;
  if (proc == NULL) return FALSE;
  while ((ent = readdir(proc)) != NULL && !found) {
    if (!g_ascii_isdigit(ent->d_name[0])) continue;
    gchar comm_path[64];
    g_snprintf(comm_path, sizeof(comm_path), "/proc/%s/comm", ent->d_name);
    FILE *cf = fopen(comm_path, "r");
    if (cf == NULL) continue;
    gchar comm[64] = "";
    if (fgets(comm, sizeof(comm), cf) != NULL) {
      g_strchomp(comm);
      for (gsize i = 0; names[i] != NULL; i++) {
        if (g_strcmp0(comm, names[i]) == 0) { found = TRUE; break; }
      }
    }
    fclose(cf);
  }
  closedir(proc);
  return found;
}

/* ── PTY terminal helpers ───────────────────────────────────────────────── */

/* Spawn a fresh login bash attached to a new pty for this single connection.
 * On success *out_master_fd is non-blocking and *out_pid is a live child.
 * On failure both are -1 and errno reflects the first syscall that failed. */
static gboolean
skm_term_spawn(int *out_master_fd, pid_t *out_pid)
{
  *out_master_fd = -1;
  *out_pid = -1;

  int master_fd = -1, slave_fd = -1;
  if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) < 0) return FALSE;

  /* set non-blocking on master so poll() alone drives I/O */
  int fl = fcntl(master_fd, F_GETFL, 0);
  if (fl >= 0) fcntl(master_fd, F_SETFL, fl | O_NONBLOCK);

  pid_t pid = fork();
  if (pid == 0) {
    struct passwd *pw = getpwuid(geteuid());
    const gchar *user_name = (pw != NULL && pw->pw_name != NULL && *pw->pw_name != '\0')
      ? pw->pw_name : "user";
    const gchar *home_dir = (pw != NULL && pw->pw_dir != NULL && *pw->pw_dir != '\0')
      ? pw->pw_dir : "/";
    g_autofree gchar *home_env = g_strdup_printf("HOME=%s", home_dir);
    g_autofree gchar *user_env = g_strdup_printf("USER=%s", user_name);
    g_autofree gchar *logname_env = g_strdup_printf("LOGNAME=%s", user_name);
    g_autofree gchar *ps1_env = g_strdup_printf(
      "PS1=\\[\\033[1;31m\\]%s\\[\\033[0m\\]@"
      "\\[\\033[1;36m\\]PlayStation4\\[\\033[0m\\]:"
      "\\[\\033[1;34m\\]\\w\\[\\033[0m\\]\\$ ",
      user_name);

    close(master_fd);
    setsid();
    ioctl(slave_fd, TIOCSCTTY, 0);
    dup2(slave_fd, STDIN_FILENO);
    dup2(slave_fd, STDOUT_FILENO);
    dup2(slave_fd, STDERR_FILENO);
    if (slave_fd > STDERR_FILENO) close(slave_fd);
    const char *env[] = {
      "TERM=xterm-256color",
      home_env, user_env, logname_env, "SHELL=/bin/bash",
      "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
      "LANG=en_US.UTF-8",
      ps1_env,
      NULL
    };
    execle("/bin/bash", "/bin/bash", "--login", NULL, env);
    _exit(1);
  }

  close(slave_fd);
  if (pid < 0) { close(master_fd); return FALSE; }

  *out_master_fd = master_fd;
  *out_pid = pid;
  return TRUE;
}

/* Tear down the per-connection shell at disconnect. Sends SIGHUP, waits up
 * to ~500 ms for a clean exit, then SIGKILL + reap. Safe to call with
 * master_fd == -1 / pid == -1 (no-op). */
static void
skm_term_teardown(int master_fd, pid_t pid)
{
  if (master_fd >= 0) close(master_fd);
  if (pid <= 0) return;

  kill(pid, SIGHUP);
  for (int i = 0; i < 10; i++) {
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid || r < 0) return;
    g_usleep(50 * 1000);
  }
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
}


static gboolean skm_remote_server_run(GThreadedSocketService *service,
                                      GSocketConnection *connection,
                                      GObject *source_object,
                                      gpointer user_data);

static gboolean
skm_remote_dispatch_notice_cb(gpointer user_data)
{
  SkmRemoteNotice *notice = user_data;

  notice->notice_cb(notice->notice_user_data, notice->success, notice->refresh, notice->message);
  g_free(notice->message);
  g_free(notice);
  return G_SOURCE_REMOVE;
}

static void
skm_remote_emit_notice(SkmRemoteServer *server, gboolean success, gboolean refresh, const gchar *message)
{
  SkmRemoteNotice *notice = NULL;

  if (server == NULL || server->notice_cb == NULL || message == NULL) {
    return;
  }

  notice = g_new0(SkmRemoteNotice, 1);
  notice->notice_cb = server->notice_cb;
  notice->notice_user_data = server->notice_user_data;
  notice->success = success;
  notice->refresh = refresh;
  notice->message = g_strdup(message);
  g_main_context_invoke(NULL, skm_remote_dispatch_notice_cb, notice);
}


static gchar *
skm_json_escape(const gchar *text)
{
  GString *escaped = NULL;
  const gchar *cursor = NULL;

  if (text == NULL) {
    return g_strdup("");
  }

  escaped = g_string_new("");
  for (cursor = text; *cursor != '\0'; cursor++) {
    switch (*cursor) {
      case '\\':
        g_string_append(escaped, "\\\\");
        break;
      case '"':
        g_string_append(escaped, "\\\"");
        break;
      case '\n':
        g_string_append(escaped, "\\n");
        break;
      case '\r':
        g_string_append(escaped, "\\r");
        break;
      case '\t':
        g_string_append(escaped, "\\t");
        break;
      default:
        g_string_append_c(escaped, *cursor);
        break;
    }
  }

  return g_string_free(escaped, FALSE);
}


static gint
skm_form_get_int(GHashTable *form, const gchar *key, gint fallback)
{
  const gchar *value = NULL;
  gchar *end = NULL;
  glong parsed = 0;

  if (form == NULL) {
    return fallback;
  }

  value = g_hash_table_lookup(form, key);
  if (value == NULL || *value == '\0') {
    return fallback;
  }

  parsed = g_ascii_strtoll(value, &end, 10);
  if (end == value || *end != '\0') {
    return fallback;
  }

  return (gint) parsed;
}

static gchar *
skm_terminal_sanitize_text(const guint8 *payload, gsize length)
{
  g_autofree gchar *valid = NULL;
  GString *text = NULL;
  const gchar *cursor = NULL;

  valid = g_utf8_make_valid((const gchar *) payload, (gssize) length);
  text = g_string_sized_new(strlen(valid));
  cursor = valid;

  while (*cursor != '\0') {
    if ((guchar) cursor[0] == 0x1b && cursor[1] != '\0' && cursor[1] == ']') {
      cursor += 2;
      while (*cursor != '\0') {
        if ((guchar) cursor[0] == 0x07) {
          cursor++;
          break;
        }
        if ((guchar) cursor[0] == 0x1b && cursor[1] == '\\') {
          cursor += 2;
          break;
        }
        cursor++;
      }
      continue;
    }

    if ((guchar) cursor[0] == 0x1b &&
        cursor[1] != '\0' && cursor[1] == '[' &&
        cursor[2] != '\0' && cursor[2] == '?') {
      const gchar *end = cursor + 3;

      while (g_ascii_isdigit(*end) || *end == ';') {
        end++;
      }
      if (*end == 'h' || *end == 'l' || *end == 'r') {
        cursor = end + 1;
        continue;
      }
    }

    g_string_append_c(text, *cursor);
    cursor++;
  }

  return g_string_free(text, FALSE);
}

static GHashTable *
skm_parse_pairs(const gchar *text, const gchar *separator)
{
  GHashTable *pairs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  gchar **parts = NULL;
  guint i = 0;

  if (text == NULL || *text == '\0') {
    return pairs;
  }

  parts = g_strsplit(text, separator, -1);
  for (i = 0; parts[i] != NULL; i++) {
    gchar *value_sep = strchr(parts[i], '=');
    g_autofree gchar *key = NULL;
    g_autofree gchar *value = NULL;

    if (value_sep != NULL) {
      *value_sep = '\0';
      key = g_uri_unescape_string(parts[i], NULL);
      value = g_uri_unescape_string(value_sep + 1, NULL);
    } else {
      key = g_uri_unescape_string(parts[i], NULL);
      value = g_strdup("");
    }

    if (key != NULL) {
      g_hash_table_replace(pairs, g_strdup(key), g_strdup(value != NULL ? value : ""));
    }
  }

  g_strfreev(parts);
  return pairs;
}

static GHashTable *
skm_parse_form(const gchar *body)
{
  return skm_parse_pairs(body, "&");
}

static gchar *
skm_read_body(GInputStream *input, gsize length)
{
  gchar *body = NULL;
  gsize bytes_read = 0;

  if (length == 0) {
    return g_strdup("");
  }

  body = g_malloc0(length + 1);
  if (!g_input_stream_read_all(input, body, length, &bytes_read, NULL, NULL) || bytes_read != length) {
    g_free(body);
    return g_strdup("");
  }

  return body;
}

static GHashTable *
skm_read_headers(GDataInputStream *data_input, gsize *out_content_length)
{
  GHashTable *headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  if (out_content_length != NULL) {
    *out_content_length = 0;
  }

  for (;;) {
    g_autofree gchar *header_line = g_data_input_stream_read_line(data_input, NULL, NULL, NULL);
    gchar *separator = NULL;
    g_autofree gchar *name = NULL;
    const gchar *value = NULL;

    if (header_line == NULL || *header_line == '\0') {
      break;
    }

    separator = strchr(header_line, ':');
    if (separator == NULL) {
      continue;
    }

    *separator = '\0';
    name = g_ascii_strdown(header_line, -1);
    value = g_strstrip(separator + 1);
    g_hash_table_replace(headers, g_strdup(name), g_strdup(value));

    if (out_content_length != NULL && g_strcmp0(name, "content-length") == 0) {
      *out_content_length = (gsize) g_ascii_strtoull(value, NULL, 10);
    }
  }

  return headers;
}

static gchar *
skm_json_extract_string(const gchar *body, const gchar *key)
{
  g_autofree gchar *pattern = NULL;
  g_autoptr(GRegex) regex = NULL;
  g_autoptr(GMatchInfo) match = NULL;

  if (body == NULL || key == NULL) {
    return NULL;
  }

  pattern = g_strdup_printf("\"%s\"\\s*:\\s*\"([^\"]*)\"", key);
  regex = g_regex_new(pattern, G_REGEX_OPTIMIZE, 0, NULL);
  if (regex == NULL || !g_regex_match(regex, body, 0, &match)) {
    return NULL;
  }

  return g_match_info_fetch(match, 1);
}

static gboolean
skm_json_extract_int(const gchar *body, const gchar *key, gint *out_value)
{
  g_autofree gchar *pattern = NULL;
  g_autoptr(GRegex) regex = NULL;
  g_autoptr(GMatchInfo) match = NULL;
  g_autofree gchar *value = NULL;
  gchar *end = NULL;
  glong parsed = 0;

  if (body == NULL || key == NULL || out_value == NULL) {
    return FALSE;
  }

  pattern = g_strdup_printf("\"%s\"\\s*:\\s*(-?[0-9]+)", key);
  regex = g_regex_new(pattern, G_REGEX_OPTIMIZE, 0, NULL);
  if (regex == NULL || !g_regex_match(regex, body, 0, &match)) {
    return FALSE;
  }

  value = g_match_info_fetch(match, 1);
  if (value == NULL) {
    return FALSE;
  }

  parsed = g_ascii_strtoll(value, &end, 10);
  if (end == value || *end != '\0') {
    return FALSE;
  }

  *out_value = (gint) parsed;
  return TRUE;
}

static gboolean
skm_json_extract_bool(const gchar *body, const gchar *key, gboolean *out_value)
{
  g_autofree gchar *pattern = NULL;
  g_autoptr(GRegex) regex = NULL;
  g_autoptr(GMatchInfo) match = NULL;
  g_autofree gchar *value = NULL;

  if (body == NULL || key == NULL || out_value == NULL) {
    return FALSE;
  }

  pattern = g_strdup_printf("\"%s\"\\s*:\\s*(true|false|1|0)", key);
  regex = g_regex_new(pattern, G_REGEX_OPTIMIZE | G_REGEX_CASELESS, 0, NULL);
  if (regex == NULL || !g_regex_match(regex, body, 0, &match)) {
    return FALSE;
  }

  value = g_match_info_fetch(match, 1);
  if (value == NULL) {
    return FALSE;
  }

  if (g_ascii_strcasecmp(value, "true") == 0 || g_strcmp0(value, "1") == 0) {
    *out_value = TRUE;
    return TRUE;
  }
  if (g_ascii_strcasecmp(value, "false") == 0 || g_strcmp0(value, "0") == 0) {
    *out_value = FALSE;
    return TRUE;
  }

  return FALSE;
}

static gboolean
skm_write_response(GOutputStream *output,
                   gint status,
                   const gchar *content_type,
                   const gchar *body)
{
  g_autofree gchar *header = NULL;
  gsize ignored = 0;
  const gchar *reason = "OK";
  gsize body_length = body != NULL ? strlen(body) : 0;

  if (status == 400) {
    reason = "Bad Request";
  } else if (status == 401) {
    reason = "Unauthorized";
  } else if (status == 404) {
    reason = "Not Found";
  } else if (status == 405) {
    reason = "Method Not Allowed";
  } else if (status == 422) {
    reason = "Unprocessable Entity";
  } else if (status == 426) {
    reason = "Upgrade Required";
  } else if (status == 429) {
    reason = "Too Many Requests";
  } else if (status == 500) {
    reason = "Internal Server Error";
  } else if (status == 501) {
    reason = "Not Implemented";
  }

  header = g_strdup_printf(
    "HTTP/1.1 %d %s\r\n"
    "Content-Type: %s\r\n"
    "Content-Length: %" G_GSIZE_FORMAT "\r\n"
    "Connection: close\r\n"
    "\r\n",
    status,
    reason,
    content_type != NULL ? content_type : "text/plain; charset=utf-8",
    body_length);

  if (!g_output_stream_write_all(output, header, strlen(header), &ignored, NULL, NULL)) {
    return FALSE;
  }

  if (body_length > 0 &&
      !g_output_stream_write_all(output, body, body_length, &ignored, NULL, NULL)) {
    return FALSE;
  }

  g_output_stream_flush(output, NULL, NULL);
  return TRUE;
}

static gchar *
skm_remote_identify_peer(GSocketConnection *connection)
{
  g_autoptr(GSocketAddress) address = NULL;

  address = g_socket_connection_get_remote_address(connection, NULL);
  if (address != NULL && G_IS_INET_SOCKET_ADDRESS(address)) {
    GInetAddress *inet = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(address));
    g_autofree gchar *ip = g_inet_address_to_string(inet);

    return g_strdup_printf(
      "%s:%u",
      ip != NULL ? ip : "unknown",
      g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(address)));
  }

  return g_strdup("unknown");
}

/* Extract just the bare IP from an "ip:port" peer string for rate-limiting
 * purposes — port would make each login attempt unique and defeat the limit.
 * Returns newly-allocated string; caller frees. */
static gchar *
skm_remote_peer_ip(const gchar *peer)
{
  if (peer == NULL) {
    return g_strdup("unknown");
  }

  /* IPv6 peers come formatted as "[::1]:12345"; strip brackets + port. */
  if (peer[0] == '[') {
    const gchar *end = strchr(peer, ']');
    if (end != NULL) {
      return g_strndup(peer + 1, (gsize) (end - peer - 1));
    }
  }

  const gchar *colon = strrchr(peer, ':');
  if (colon != NULL) {
    return g_strndup(peer, (gsize) (colon - peer));
  }

  return g_strdup(peer);
}

/* ── Login rate limit (protected by server->login_lock) ───────────── */

static gboolean
skm_login_is_locked(SkmRemoteServer *server, const gchar *peer_ip)
{
  gboolean locked = FALSE;
  if (server == NULL || peer_ip == NULL || server->login_attempts == NULL) {
    return FALSE;
  }

  g_mutex_lock(&server->login_lock);
  SkmLoginAttempt *a = g_hash_table_lookup(server->login_attempts, peer_ip);
  if (a != NULL && a->locked_until_us > 0 &&
      g_get_monotonic_time() < a->locked_until_us) {
    locked = TRUE;
  }
  g_mutex_unlock(&server->login_lock);
  return locked;
}

static void
skm_login_record_failure(SkmRemoteServer *server, const gchar *peer_ip)
{
  if (server == NULL || peer_ip == NULL || server->login_attempts == NULL) {
    return;
  }

  g_mutex_lock(&server->login_lock);
  SkmLoginAttempt *a = g_hash_table_lookup(server->login_attempts, peer_ip);
  if (a == NULL) {
    a = g_new0(SkmLoginAttempt, 1);
    g_hash_table_insert(server->login_attempts, g_strdup(peer_ip), a);
  }
  a->fail_count++;
  if (a->fail_count >= SKM_LOGIN_FAIL_LIMIT) {
    a->locked_until_us = g_get_monotonic_time() +
      (gint64) SKM_LOGIN_LOCKOUT_SECONDS * G_USEC_PER_SEC;
  }
  g_mutex_unlock(&server->login_lock);
}

static void
skm_login_record_success(SkmRemoteServer *server, const gchar *peer_ip)
{
  if (server == NULL || peer_ip == NULL || server->login_attempts == NULL) {
    return;
  }

  g_mutex_lock(&server->login_lock);
  g_hash_table_remove(server->login_attempts, peer_ip);
  g_mutex_unlock(&server->login_lock);
}

static gchar *
skm_remote_shorten_agent(const gchar *user_agent)
{
  if (user_agent == NULL || *user_agent == '\0') {
    return g_strdup("unknown client");
  }

  if (g_strrstr(user_agent, "Braska") != NULL) {
    return g_strdup("Strawberry Manager");
  }
  if (g_strrstr(user_agent, "Android") != NULL) {
    return g_strdup("Android");
  }
  if (g_strrstr(user_agent, "iPhone") != NULL || g_strrstr(user_agent, "iPad") != NULL) {
    return g_strdup("iPhone/iPad");
  }
  if (g_strrstr(user_agent, "curl") != NULL) {
    return g_strdup("curl");
  }
  if (strlen(user_agent) > 52) {
    return g_strdup_printf("%.52s...", user_agent);
  }

  return g_strdup(user_agent);
}

static void
skm_remote_record_client(SkmRemoteServer *server,
                         const gchar *peer,
                         const gchar *method,
                         const gchar *path,
                         const gchar *user_agent)
{
  g_autoptr(GDateTime) now = NULL;
  g_autofree gchar *timestamp = NULL;
  g_autofree gchar *agent = NULL;
  g_autofree gchar *entry = NULL;

  if (server == NULL) {
    return;
  }

  now = g_date_time_new_now_local();
  timestamp = g_date_time_format(now, "%H:%M:%S");
  agent = skm_remote_shorten_agent(user_agent);
  entry = g_strdup_printf(
    "%s  %s  %s %s  [%s]",
    timestamp,
    peer != NULL ? peer : "unknown",
    method != NULL ? method : "GET",
    path != NULL ? path : "/",
    agent);

  g_mutex_lock(&server->lock);
  server->total_connections++;
  g_clear_pointer(&server->last_client, g_free);
  server->last_client = g_strdup(entry);
  g_ptr_array_add(server->recent_clients, g_strdup(entry));
  while (server->recent_clients->len > SKM_REMOTE_MAX_CLIENTS) {
    g_ptr_array_remove_index(server->recent_clients, 0);
  }
  g_mutex_unlock(&server->lock);

  g_print("[remote] %s\n", entry);
}

static GPtrArray *
skm_remote_copy_recent_clients(SkmRemoteServer *server, guint64 *out_total_connections, gchar **out_last_client)
{
  GPtrArray *copy = g_ptr_array_new_with_free_func(g_free);
  guint i = 0;

  g_mutex_lock(&server->lock);
  if (out_total_connections != NULL) {
    *out_total_connections = server->total_connections;
  }
  if (out_last_client != NULL) {
    *out_last_client = g_strdup(server->last_client);
  }
  for (i = 0; i < server->recent_clients->len; i++) {
    g_ptr_array_add(copy, g_strdup(g_ptr_array_index(server->recent_clients, i)));
  }
  g_mutex_unlock(&server->lock);

  return copy;
}


static void
skm_normalize_path(gchar *path)
{
  if (path == NULL || *path == '\0') {
    return;
  }

  while (g_str_has_prefix(path, "//")) {
    memmove(path, path + 1, strlen(path));
  }
}

static gboolean
skm_is_websocket_request(GHashTable *headers)
{
  const gchar *upgrade = NULL;
  const gchar *connection = NULL;
  g_autofree gchar *upgrade_lower = NULL;
  g_autofree gchar *connection_lower = NULL;

  if (headers == NULL) {
    return FALSE;
  }

  upgrade = g_hash_table_lookup(headers, "upgrade");
  connection = g_hash_table_lookup(headers, "connection");
  upgrade_lower = g_ascii_strdown(upgrade != NULL ? upgrade : "", -1);
  connection_lower = g_ascii_strdown(connection != NULL ? connection : "", -1);
  return g_strcmp0(upgrade_lower, "websocket") == 0 &&
         g_strrstr(connection_lower, "upgrade") != NULL;
}

static gchar *
skm_build_json_detail(gint status, const gchar *detail)
{
  g_autofree gchar *escaped = skm_json_escape(detail != NULL ? detail : "Unknown error.");

  return g_strdup_printf("{\"status\":%d,\"detail\":\"%s\"}", status, escaped);
}

static gchar *
skm_read_uptime_text(const gchar *proc_uptime_path, gint *out_seconds)
{
  g_autofree gchar *text = NULL;
  gchar **parts = NULL;
  gdouble total = 0.0;
  gint days = 0;
  gint hours = 0;
  gint minutes = 0;

  if (out_seconds != NULL) {
    *out_seconds = 0;
  }

  if (!skm_sysfs_read_text(proc_uptime_path, "system uptime", &text, NULL)) {
    return g_strdup("Unavailable");
  }

  parts = g_strsplit(text, " ", 2);
  if (parts[0] == NULL) {
    g_strfreev(parts);
    return g_strdup("Unavailable");
  }

  total = g_ascii_strtod(parts[0], NULL);
  if (out_seconds != NULL) {
    *out_seconds = (gint) total;
  }
  g_strfreev(parts);

  days = (gint) (total / 86400.0);
  total -= days * 86400.0;
  hours = (gint) (total / 3600.0);
  total -= hours * 3600.0;
  minutes = (gint) (total / 60.0);

  if (days > 0) {
    return g_strdup_printf("%dd %dh %dm", days, hours, minutes);
  }
  if (hours > 0) {
    return g_strdup_printf("%dh %dm", hours, minutes);
  }
  return g_strdup_printf("%dm", minutes);
}

static gchar *
skm_build_health_json(SkmRemoteServer *server)
{
  g_autofree gchar *last_client = NULL;
  g_autofree gchar *escaped_last = NULL;
  guint64 total_connections = 0;
  gchar *json = NULL;

  skm_remote_copy_recent_clients(server, &total_connections, &last_client);
  escaped_last = skm_json_escape(last_client != NULL ? last_client : "none");
  json = g_strdup_printf(
    "{"
    "\"status\":\"ok\","
    "\"service\":\"strawberry-kernel-manager\","
    "\"auth_required\":%s,"
    "\"compat\":[\"braska\"],"
    "\"remote\":{"
      "\"port\":%d,"
      "\"total_connections\":%" G_GUINT64_FORMAT ","
      "\"last_client\":\"%s\""
    "}"
    "}",
    server->auth_required ? "true" : "false",
    server->port,
    total_connections,
    escaped_last);
  return json;
}

static gchar *
skm_build_settings_json(SkmRemoteServer *server)
{
  g_autofree gchar *path = NULL;
  gboolean exists = FALSE;

  if (server == NULL) {
    return g_strdup("{\"status\":500,\"detail\":\"Settings unavailable.\"}");
  }

  path = skm_json_escape(server->settings_path != NULL ? server->settings_path : "");
  exists = server->settings_path != NULL && g_file_test(server->settings_path, G_FILE_TEST_EXISTS);
  return g_strdup_printf(
    "{"
    "\"path\":\"%s\","
    "\"exists\":%s,"
    "\"oled_black_mode\":%s,"
    "\"poll_interval_ms\":%d,"
    "\"fan_debounce_ms\":%d,"
    "\"remote_enabled\":%s,"
    "\"remote_port\":%d,"
    "\"remote_password\":%s,"
    "\"auth_required\":%s"
    "}",
    path,
    exists ? "true" : "false",
    server->settings.oled_black_mode ? "true" : "false",
    server->settings.poll_interval_ms,
    server->settings.fan_debounce_ms,
    server->settings.remote_enabled ? "true" : "false",
    server->settings.remote_port,
    server->settings.remote_password_hash != NULL ? "\"***\"" : "null",
    server->auth_required ? "true" : "false");
}

static gchar *
skm_build_led_profiles_json(const SkmLedState *state)
{
  GString *json = g_string_new("{\"profiles\":[");
  guint i = 0;

  for (i = 0; state->effect_options != NULL && i < state->effect_options->len; i++) {
    const gchar *value = g_ptr_array_index(state->effect_options, i);
    g_autofree gchar *escaped = skm_json_escape(value);

    if (i > 0) {
      g_string_append(json, ",");
    }
    g_string_append_printf(json, "\"%s\"", escaped);
  }
  g_string_append(json, "]}");
  return g_string_free(json, FALSE);
}

static gchar *
skm_build_led_active_json(const SkmLedState *state)
{
  g_autofree gchar *escaped = skm_json_escape(
    state->has_active_effect && state->active_effect != NULL ? state->active_effect : "off");

  return g_strdup_printf("{\"active\":\"%s\"}", escaped);
}

static gchar *
skm_build_fan_threshold_json(gint threshold, gboolean confirmed)
{
  return confirmed
    ? g_strdup_printf("{\"threshold_confirmed\":%d}", threshold)
    : g_strdup_printf("{\"threshold\":%d}", threshold);
}

static gchar *
skm_build_tunnel_status_json(void)
{
  /* No integration with cloudflared/tailscaled control sockets yet — we
   * just report "running" when the relevant daemon process is live, so the
   * iOS Connect screen's Tunnel mode stops showing a permanent "stopped"
   * regardless of reality. @url is still null because we have no way to
   * recover the assigned hostname without parsing cloudflared's logs or
   * talking to tailscaled's LocalAPI. */
  static const gchar * const names[] = { "cloudflared", "tailscaled", NULL };
  const gchar *state = skm_tunnel_any_running(names) ? "running" : "stopped";
  return g_strdup_printf("{\"state\":\"%s\",\"url\":null}", state);
}

/* Tail the systemd journal for the strawberry-manager unit and return a
 * JSON envelope:
 *
 *   { "unit": "strawberry-manager", "lines": [ "...", "..." ],
 *     "count": N, "priority": <0-7>|null }
 *
 * Parameters:
 *   lines    — number of most-recent lines to return (1..2000). The caller
 *              is expected to have clamped but we re-clamp defensively so
 *              a bug in the dispatch layer can't explode into an OOM.
 *   priority — NULL or a single digit 0..7 (journald syslog priority).
 *              Already validated by the caller; rejected here too as a
 *              defence in depth.
 *   out_status — on failure, set to the HTTP status code (400/500) the
 *                caller should return alongside the body.
 *
 * Spawns `journalctl -u strawberry-manager -n N --no-pager
 * --output=short-iso [-p P]` with an explicit argv (no shell), so there
 * is no injection surface even if upstream validation ever regresses.
 *
 * Returns a heap-allocated JSON string on success, or a
 * skm_build_json_detail() error body if journalctl is unavailable, times
 * out, or exits non-zero. */
static gchar *
skm_build_logs_json(gint lines, const gchar *priority, gint *out_status)
{
  if (lines < 1) lines = 1;
  if (lines > 2000) lines = 2000;

  const gchar *prio = NULL;
  if (priority != NULL && priority[0] != '\0') {
    if (priority[1] == '\0' && priority[0] >= '0' && priority[0] <= '7') {
      prio = priority;
    } else {
      if (out_status != NULL) *out_status = 400;
      return skm_build_json_detail(400,
        "priority must be a single digit 0-7 (journald syslog priority).");
    }
  }

  g_autofree gchar *lines_str = g_strdup_printf("%d", lines);

  /* Explicit argv, no shell. Every slot is either a literal or one of the
   * two values we just validated, so g_spawn_sync() cannot be tricked into
   * running anything else via argument smuggling. */
  const gchar *argv[12] = { 0 };
  gsize i = 0;
  argv[i++] = "journalctl";
  argv[i++] = "-u";
  argv[i++] = "strawberry-manager";
  argv[i++] = "-n";
  argv[i++] = lines_str;
  argv[i++] = "--no-pager";
  argv[i++] = "--output=short-iso";
  if (prio != NULL) {
    argv[i++] = "-p";
    argv[i++] = prio;
  }
  argv[i] = NULL;

  g_autofree gchar *stdout_buf = NULL;
  g_autofree gchar *stderr_buf = NULL;
  gint exit_status = 0;
  g_autoptr(GError) spawn_error = NULL;
  gboolean ok = g_spawn_sync(
    NULL,                            /* working directory: inherit */
    (gchar **) argv,
    NULL,                            /* env: inherit */
    G_SPAWN_SEARCH_PATH,
    NULL, NULL,                      /* no setup fn */
    &stdout_buf, &stderr_buf,
    &exit_status,
    &spawn_error);

  if (!ok) {
    if (out_status != NULL) *out_status = 500;
    return skm_build_json_detail(500,
      spawn_error != NULL && spawn_error->message != NULL
        ? spawn_error->message
        : "Failed to spawn journalctl.");
  }
  if (!g_spawn_check_wait_status(exit_status, NULL)) {
    if (out_status != NULL) *out_status = 500;
    g_autofree gchar *trimmed = (stderr_buf != NULL && *stderr_buf != '\0')
      ? g_strdup(g_strchomp(stderr_buf))
      : g_strdup("journalctl exited non-zero.");
    return skm_build_json_detail(500, trimmed);
  }

  GString *json = g_string_new("{\"unit\":\"strawberry-manager\",\"lines\":[");
  gsize count = 0;
  if (stdout_buf != NULL) {
    gchar **arr = g_strsplit(stdout_buf, "\n", -1);
    for (gchar **p = arr; *p != NULL; p++) {
      /* journalctl's output includes a trailing empty line from the
       * final newline; skip any empties so the JSON array is tight. */
      if (**p == '\0') continue;
      g_autofree gchar *esc = skm_json_escape(*p);
      if (count > 0) g_string_append_c(json, ',');
      g_string_append_printf(json, "\"%s\"", esc);
      count++;
    }
    g_strfreev(arr);
  }
  g_string_append_printf(json,
    "],\"count\":%" G_GSIZE_FORMAT ",\"priority\":%s%s%s}",
    count,
    prio != NULL ? "\"" : "null",
    prio != NULL ? prio : "",
    prio != NULL ? "\"" : "");
  return g_string_free(json, FALSE);
}

static gchar *
skm_build_processes_json(gint limit, const gchar *sort_by)
{
  GArray *arr = skm_read_processes(limit > 0 ? limit : SKM_REMOTE_PROCESS_LIMIT_DEFAULT, sort_by);
  GString *json = g_string_new("{\"processes\":[");

  for (guint i = 0; i < arr->len; i++) {
    SkmProcEntry *p = &g_array_index(arr, SkmProcEntry, i);
    g_autofree gchar *escaped_comm = skm_json_escape(p->comm);
    g_autofree gchar *escaped_user = skm_json_escape(p->user);
    g_autofree gchar *escaped_status = skm_json_escape(p->status);
    g_autofree gchar *escaped_cmdline = skm_json_escape(p->cmdline);
    gdouble memory_mb = (gdouble) p->rss_kb / 1024.0;
    if (i > 0) g_string_append_c(json, ',');
    g_string_append_printf(json,
      "{\"pid\":%d,"
      "\"name\":\"%s\","
      "\"command\":\"%s\","
      "\"user\":\"%s\","
      "\"username\":\"%s\","
      "\"state\":\"%c\","
      "\"status\":\"%s\","
      "\"threads\":%d,"
      "\"cmdline\":\"%s\","
      "\"cpu\":%.1f,"
      "\"cpu_pct\":%.1f,"
      "\"cpuPercent\":%.1f,"
      "\"cpu_percent\":%.1f,"
      "\"memory\":%.1f,"
      "\"mem_rss_mb\":%.1f,"
      "\"memoryMb\":%.1f,"
      "\"memory_mb\":%.1f,"
      "\"mem_pct\":%.1f,"
      "\"memory_percent\":%.1f}",
      p->pid,
      escaped_comm,
      escaped_comm,
      escaped_user,
      escaped_user,
      p->state,
      escaped_status,
      p->threads,
      escaped_cmdline,
      p->cpu_percent,
      p->cpu_percent,
      p->cpu_percent,
      p->cpu_percent,
      memory_mb,
      memory_mb,
      memory_mb,
      memory_mb,
      p->mem_percent,
      p->mem_percent);
  }
  g_string_append_printf(json, "],\"count\":%u}", arr->len);
  g_array_free(arr, TRUE);
  return g_string_free(json, FALSE);
}

/* Allowlist of filesystem roots the /api/files endpoints may access.
 * Every user-supplied path is resolved with realpath() and rejected unless it
 * lands under one of these prefixes. This blocks the classic `..` traversal
 * and absolute-path tricks without requiring chroot. */
static const gchar *
skm_files_allowed_roots[] = {
  "/home",
  "/mnt",
  "/media",
  "/tmp",
  "/var/tmp",
  "/data",
  "/storage",
  NULL,
};

/* Resolve a caller-supplied path to its canonical form and confirm it lives
 * under the allowlist above. Returns newly-allocated canonical path on
 * success, or NULL on rejection (sets out_reason to a static, user-safe
 * diagnostic). */
static gchar *
skm_files_validate_path(const gchar *input, const gchar **out_reason)
{
  if (out_reason != NULL) *out_reason = NULL;

  if (input == NULL || *input == '\0') {
    if (out_reason != NULL) *out_reason = "Path required.";
    return NULL;
  }

  /* Reject relative paths outright — we don't trust the daemon's cwd. */
  if (input[0] != '/') {
    if (out_reason != NULL) *out_reason = "Absolute path required.";
    return NULL;
  }

  /* realpath() follows symlinks and requires every component to exist, so a
   * caller can't smuggle in an allowlisted prefix that points at something
   * outside (e.g. /tmp/escape -> /etc). g_canonicalize_filename was purely
   * lexical and would have let that through. */
  char *resolved = realpath(input, NULL);
  g_autofree gchar *canonical = resolved;
  if (canonical == NULL) {
    if (out_reason != NULL) *out_reason = "Path not accessible.";
    return NULL;
  }

  for (gsize i = 0; skm_files_allowed_roots[i] != NULL; i++) {
    const gchar *root = skm_files_allowed_roots[i];
    gsize rlen = strlen(root);
    if (g_strcmp0(canonical, root) == 0) {
      return g_steal_pointer(&canonical);
    }
    if (g_str_has_prefix(canonical, root) && canonical[rlen] == '/') {
      return g_steal_pointer(&canonical);
    }
  }

  if (out_reason != NULL) *out_reason = "Path is outside the allowed roots.";
  return NULL;
}

/* Validates an upload destination where the target file itself may not
 * exist yet — realpath() would fail. Instead canonicalize the parent
 * directory (which must exist and must be inside the allowlist) and
 * append the basename. Rejects "." / ".." components up front so a caller
 * can't traverse out of the parent via the joined path. */
static gchar *
skm_files_validate_upload_target(const gchar *input, const gchar **out_reason)
{
  if (out_reason != NULL) *out_reason = NULL;
  if (input == NULL || *input == '\0') {
    if (out_reason != NULL) *out_reason = "Path required.";
    return NULL;
  }
  if (input[0] != '/') {
    if (out_reason != NULL) *out_reason = "Absolute path required.";
    return NULL;
  }
  /* Any "/.." or "/./" or terminal ".." is rejected before we split — this
   * keeps the basename join below trivially safe. */
  if (strstr(input, "/../") != NULL || strstr(input, "/./") != NULL ||
      g_str_has_suffix(input, "/..") || g_str_has_suffix(input, "/.")) {
    if (out_reason != NULL) *out_reason = "Path must not contain . or .. components.";
    return NULL;
  }
  g_autofree gchar *parent = g_path_get_dirname(input);
  g_autofree gchar *base   = g_path_get_basename(input);
  if (base == NULL || *base == '\0' ||
      g_strcmp0(base, ".") == 0 || g_strcmp0(base, "..") == 0 ||
      strchr(base, '/') != NULL) {
    if (out_reason != NULL) *out_reason = "Invalid filename.";
    return NULL;
  }
  char *resolved_parent = realpath(parent, NULL);
  g_autofree gchar *canon_parent = resolved_parent;
  if (canon_parent == NULL) {
    if (out_reason != NULL) *out_reason = "Parent directory does not exist.";
    return NULL;
  }
  gboolean ok = FALSE;
  for (gsize i = 0; skm_files_allowed_roots[i] != NULL; i++) {
    const gchar *root = skm_files_allowed_roots[i];
    gsize rlen = strlen(root);
    if (g_strcmp0(canon_parent, root) == 0) { ok = TRUE; break; }
    if (g_str_has_prefix(canon_parent, root) && canon_parent[rlen] == '/') { ok = TRUE; break; }
  }
  if (!ok) {
    if (out_reason != NULL) *out_reason = "Path is outside the allowed roots.";
    return NULL;
  }
  return g_build_filename(canon_parent, base, NULL);
}

/* Shared auth check mirroring the inline block at the top of
 * skm_handle_api_request — lets the file streaming pre-dispatch do the
 * same check without routing through the buffered-body code path. */
static gboolean
skm_remote_auth_check(SkmRemoteServer *server, GHashTable *headers)
{
  if (!server->auth_required) return TRUE;
  if (headers == NULL) return FALSE;
  const gchar *auth_header = g_hash_table_lookup(headers, "authorization");
  const gchar *bearer = NULL;
  if (auth_header != NULL && g_str_has_prefix(auth_header, "Bearer "))
    bearer = auth_header + 7;
  else
    bearer = g_hash_table_lookup(headers, "token");
  return bearer != NULL && skm_token_equal(bearer, server->auth_token);
}

/* Streams a file's contents to @output as a 200 response. Avoids loading
 * the whole thing into memory so the daemon can serve multi-MB PS4 game
 * saves or log dumps without blowing up its address space. */
static gboolean
skm_stream_file_response(GOutputStream *output, const gchar *safe_path)
{
  int fd = open(safe_path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return FALSE;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return FALSE;
  }
  g_autofree gchar *base = g_path_get_basename(safe_path);
  /* Linux filenames can contain ", \, CR, LF and other bytes that would
   * break the Content-Disposition header value. Sanitize in place — any
   * non-printable ASCII or structural character becomes '_'. Clients that
   * care about the exact filename can reconstruct it from the path query
   * param; this header is only a download convenience. */
  for (gchar *p = base; *p != '\0'; p++) {
    guchar c = (guchar) *p;
    if (c < 0x20 || c == 0x7f || c == '"' || c == '\\') *p = '_';
  }
  g_autofree gchar *header = g_strdup_printf(
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/octet-stream\r\n"
    "Content-Length: %" G_GOFFSET_FORMAT "\r\n"
    "Content-Disposition: attachment; filename=\"%s\"\r\n"
    "Connection: close\r\n\r\n",
    (goffset) st.st_size, base);
  gsize ignored = 0;
  if (!g_output_stream_write_all(output, header, strlen(header), &ignored, NULL, NULL)) {
    close(fd);
    return FALSE;
  }
  /* 64 KB chunks is a reasonable balance between syscall count and memory
   * footprint — the PS4 has plenty of RAM so we're optimizing for not
   * starving the daemon's other connections. */
  guchar buf[64 * 1024];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      close(fd);
      return FALSE;
    }
    if (n == 0) break;
    if (!g_output_stream_write_all(output, buf, (gsize) n, &ignored, NULL, NULL)) {
      close(fd);
      return FALSE;
    }
  }
  close(fd);
  return TRUE;
}

/* Streams up to @content_length bytes from @input into @dest_path. Writes
 * to a temp sibling first and rename()s on success so a partial transfer
 * never replaces a good file — if the client hangs up mid-upload, the
 * pre-existing file (if any) survives. */
static gboolean
skm_stream_upload_to_file(GInputStream *input,
                          const gchar *dest_path,
                          gsize content_length,
                          gsize *out_written)
{
  if (out_written != NULL) *out_written = 0;
  g_autofree gchar *tmp_path = g_strdup_printf("%s.skm-upload.%u", dest_path, g_random_int());
  int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return FALSE;
  gsize remaining = content_length;
  guchar buf[64 * 1024];
  while (remaining > 0) {
    gsize want = remaining > sizeof(buf) ? sizeof(buf) : remaining;
    gsize got = 0;
    if (!g_input_stream_read_all(input, buf, want, &got, NULL, NULL) || got == 0) {
      close(fd);
      unlink(tmp_path);
      return FALSE;
    }
    gsize off = 0;
    while (off < got) {
      ssize_t n = write(fd, buf + off, got - off);
      if (n < 0) {
        if (errno == EINTR) continue;
        close(fd);
        unlink(tmp_path);
        return FALSE;
      }
      off += (gsize) n;
    }
    remaining -= got;
    if (out_written != NULL) *out_written += got;
  }
  if (close(fd) != 0) {
    unlink(tmp_path);
    return FALSE;
  }
  if (rename(tmp_path, dest_path) != 0) {
    unlink(tmp_path);
    return FALSE;
  }
  return TRUE;
}

static gchar *
skm_build_files_json(const gchar *path)
{
  const gchar *reason = NULL;
  g_autofree gchar *safe_path = skm_files_validate_path(
    (path != NULL && *path != '\0') ? path : "/home",
    &reason);
  if (safe_path == NULL) {
    g_autofree gchar *escaped_reason = skm_json_escape(reason ? reason : "Path not allowed.");
    return g_strdup_printf(
      "{\"error\":\"%s\",\"items\":[]}",
      escaped_reason);
  }

  GDir *dir = g_dir_open(safe_path, 0, NULL);
  GString *json = g_string_new(NULL);
  g_autofree gchar *escaped_path = skm_json_escape(safe_path);

  g_string_printf(json, "{\"path\":\"%s\",\"items\":[", escaped_path);

  if (dir != NULL) {
    const gchar *name;
    gboolean first = TRUE;
    while ((name = g_dir_read_name(dir)) != NULL) {
      g_autofree gchar *full = g_build_filename(safe_path, name, NULL);
      GStatBuf st;
      if (g_stat(full, &st) != 0) continue;
      gboolean is_dir = S_ISDIR(st.st_mode);
      g_autofree gchar *escaped_name = skm_json_escape(name);
      g_autofree gchar *escaped_full = skm_json_escape(full);
      if (!first) g_string_append_c(json, ',');
      first = FALSE;
      g_string_append_printf(json,
        "{\"name\":\"%s\",\"path\":\"%s\","
        "\"is_dir\":%s,\"size\":%lld}",
        escaped_name, escaped_full,
        is_dir ? "true" : "false",
        (long long) st.st_size);
    }
    g_dir_close(dir);
  }
  g_string_append(json, "]}");
  return g_string_free(json, FALSE);
}

/* Build the /api/health telemetry payload.
 *
 * @state is caller-owned and keeps the baselines CPU-total, per-core, and
 * per-interface need so each WS subscriber sees its own deltas. The first
 * call for a given state yields zeros for the deltas (no prior sample),
 * subsequent calls yield real numbers. If @state is NULL we degrade to the
 * process-global CPU baseline and report 0 for per-core/net deltas — that
 * path is fine for one-shot REST calls where state ownership is awkward. */
static gchar *
skm_build_telemetry_json(SkmService *service, SkmTelemetryState *state)
{
  g_autofree gchar *uptime_path = NULL;
  g_autofree gchar *uptime_text = NULL;
  SkmSnapshot *snapshot = NULL;
  GString *json = NULL;
  gint uptime_seconds = 0;
  gdouble cpu_pct, cores, freq_mhz, l1, l5, l15;
  gdouble mem_total, mem_used, mem_avail, mem_cached, mem_buffers, mem_pct;
  gdouble swap_total, swap_used;

  uptime_path = g_build_filename(service->proc_root, "uptime", NULL);
  uptime_text = skm_read_uptime_text(uptime_path, &uptime_seconds);
  snapshot = skm_service_read_snapshot(service);

  if (state != NULL) {
    cpu_pct = skm_read_cpu_percent_local(&state->cpu_prev_total, &state->cpu_prev_idle);
  } else {
    cpu_pct = skm_read_cpu_percent();
  }
  /* Previously this line did a pointer-cast type-pun — writing a gint
   * through a gdouble's storage — which works today by accident but UB
   * formally and breaks under -flto. Store into a real gint. */
  gint core_count = 1;
  skm_read_cpuinfo(&core_count, &freq_mhz);
  cores = core_count;
  skm_read_loadavg(&l1, &l5, &l15);
  skm_read_meminfo(&mem_total, &mem_used, &mem_avail, &mem_cached, &mem_buffers, &mem_pct,
                   &swap_total, &swap_used);

  /* Per-core CPU% against the caller's per-core baselines. When state is
   * absent we still walk /proc/stat so @core_count (from /proc/cpuinfo) and
   * the array length agree, but every entry will be 0.0 — acceptable for
   * the stateless REST path. */
  g_autoptr(GArray) per_core = g_array_new(FALSE, TRUE, sizeof(gdouble));
  if (state != NULL) {
    skm_read_cpu_per_core(state->cores, per_core);
  }

  json = g_string_new("{");
  g_string_append_printf(json, "\"ts\":%.3f,", g_get_real_time() / 1000000.0);

  /* cpu block */
  g_string_append_printf(json,
    "\"cpu\":{\"percent\":%.1f,\"per_core\":[",
    cpu_pct);
  {
    guint emitted = 0;
    for (guint i = 0; i < per_core->len; i++) {
      if (emitted > 0) g_string_append_c(json, ',');
      g_string_append_printf(json, "%.1f", g_array_index(per_core, gdouble, i));
      emitted++;
    }
    /* Fallback: no per-core state (REST path, or /proc/stat unavailable) —
     * pad with @core_count copies of the aggregate so the iOS client's
     * `per_core` row count still matches what it'd expect from the PS4's
     * Jaguar APU. */
    if (emitted == 0) {
      for (gint i = 0; i < core_count; i++) {
        if (i > 0) g_string_append_c(json, ',');
        g_string_append_printf(json, "%.1f", cpu_pct);
      }
    }
  }
  g_string_append_printf(json,
    "],\"core_count\":%d,"
    "\"freq_mhz\":%.0f,\"freq_max_mhz\":%.0f,"
    "\"load_1\":%.2f,\"load_5\":%.2f,\"load_15\":%.2f},",
    (gint) cores,
    freq_mhz, freq_mhz,
    l1, l5, l15);

  /* ram block */
  g_string_append_printf(json,
    "\"ram\":{\"total_mb\":%.1f,\"used_mb\":%.1f,\"available_mb\":%.1f,"
    "\"cached_mb\":%.1f,\"buffers_mb\":%.1f,\"percent\":%.1f},",
    mem_total, mem_used, mem_avail, mem_cached, mem_buffers, mem_pct);

  /* swap block */
  g_string_append_printf(json,
    "\"swap\":{\"total_mb\":%.1f,\"used_mb\":%.1f,\"percent\":%.1f},",
    swap_total, swap_used,
    swap_total > 0 ? (swap_used / swap_total) * 100.0 : 0.0);

  /* fan block */
  if (snapshot->fan.available) {
    g_string_append_printf(json,
      "\"fan\":{\"rpm\":%d,\"threshold_c\":%d,\"apu_temp_c\":%.1f},",
      snapshot->fan.has_rpm ? snapshot->fan.rpm : 0,
      snapshot->fan.has_threshold_c ? snapshot->fan.threshold_c : SKM_FAN_THRESHOLD_DEFAULT,
      snapshot->fan.has_temperature_c ? snapshot->fan.temperature_c : 0.0);
  }

  /* net block — real bytes/s against caller's per-interface baseline. */
  {
    GArray *ifaces = skm_read_net_dev();
    gint64 now_us = g_get_monotonic_time();
    g_string_append(json, "\"net\":[");
    for (guint i = 0; i < ifaces->len; i++) {
      SkmNetIface *n = &g_array_index(ifaces, SkmNetIface, i);
      g_autofree gchar *esc = skm_json_escape(n->iface);
      gdouble rx_ps = 0.0, tx_ps = 0.0;
      if (state != NULL) {
        SkmNetBaseline *b = skm_net_baseline_get(state->nets, n->iface);
        if (b->prev_ts_us > 0 && now_us > b->prev_ts_us) {
          gdouble dt = (now_us - b->prev_ts_us) / 1000000.0;
          if (n->rx_bytes >= b->prev_rx) rx_ps = (n->rx_bytes - b->prev_rx) / dt;
          if (n->tx_bytes >= b->prev_tx) tx_ps = (n->tx_bytes - b->prev_tx) / dt;
        }
        b->prev_rx    = n->rx_bytes;
        b->prev_tx    = n->tx_bytes;
        b->prev_ts_us = now_us;
      }
      if (i > 0) g_string_append_c(json, ',');
      g_string_append_printf(json,
        "{\"iface\":\"%s\",\"bytes_sent_s\":%.0f,\"bytes_recv_s\":%.0f,"
        "\"rx_total\":%" G_GUINT64_FORMAT ",\"tx_total\":%" G_GUINT64_FORMAT ","
        "\"packets_sent\":0,\"packets_recv\":0,\"errin\":0,\"errout\":0}",
        esc, tx_ps, rx_ps, n->rx_bytes, n->tx_bytes);
    }
    g_string_append(json, "],");
    g_array_free(ifaces, TRUE);
  }

  g_string_append(json, "\"disk\":[],");
  g_string_append_printf(json, "\"uptime_s\":%d,", uptime_seconds);
  {
    g_autofree gchar *tun = skm_build_tunnel_status_json();
    g_string_append_printf(json, "\"tunnel\":%s", tun);
  }
  g_string_append(json, "}");
  skm_snapshot_free(snapshot);
  (void) uptime_text;
  return g_string_free(json, FALSE);
}

static gboolean
skm_ws_write_frame(GOutputStream *output, guint8 opcode, const guint8 *payload, gsize length)
{
  guint8 header[10] = { 0 };
  gsize header_len = 0;
  gsize ignored = 0;

  header[0] = 0x80 | (opcode & 0x0f);
  if (length < 126) {
    header[1] = (guint8) length;
    header_len = 2;
  } else if (length <= 0xffff) {
    header[1] = 126;
    header[2] = (guint8) ((length >> 8) & 0xff);
    header[3] = (guint8) (length & 0xff);
    header_len = 4;
  } else {
    header[1] = 127;
    header[2] = (guint8) ((length >> 56) & 0xff);
    header[3] = (guint8) ((length >> 48) & 0xff);
    header[4] = (guint8) ((length >> 40) & 0xff);
    header[5] = (guint8) ((length >> 32) & 0xff);
    header[6] = (guint8) ((length >> 24) & 0xff);
    header[7] = (guint8) ((length >> 16) & 0xff);
    header[8] = (guint8) ((length >> 8) & 0xff);
    header[9] = (guint8) (length & 0xff);
    header_len = 10;
  }

  if (!g_output_stream_write_all(output, header, header_len, &ignored, NULL, NULL)) {
    return FALSE;
  }
  if (length > 0 &&
      !g_output_stream_write_all(output, payload, length, &ignored, NULL, NULL)) {
    return FALSE;
  }

  return g_output_stream_flush(output, NULL, NULL);
}

static gboolean
skm_ws_write_text(GOutputStream *output, const gchar *text)
{
  return skm_ws_write_frame(output, 0x1, (const guint8 *) text, strlen(text));
}

static gboolean
skm_ws_send_handshake(GOutputStream *output, const gchar *key)
{
  GChecksum *checksum = NULL;
  guint8 digest[20] = { 0 };
  gsize digest_len = sizeof(digest);
  g_autofree gchar *source = NULL;
  g_autofree gchar *accept = NULL;
  g_autofree gchar *response = NULL;
  gsize ignored = 0;

  if (key == NULL || *key == '\0') {
    return FALSE;
  }

  source = g_strdup_printf("%s%s", key, SKM_WS_MAGIC);
  checksum = g_checksum_new(G_CHECKSUM_SHA1);
  g_checksum_update(checksum, (const guchar *) source, strlen(source));
  g_checksum_get_digest(checksum, digest, &digest_len);
  g_checksum_free(checksum);

  accept = g_base64_encode(digest, digest_len);
  response = g_strdup_printf(
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Accept: %s\r\n"
    "\r\n",
    accept);

  if (!g_output_stream_write_all(output, response, strlen(response), &ignored, NULL, NULL)) {
    return FALSE;
  }

  return g_output_stream_flush(output, NULL, NULL);
}

static SkmOperationResult *
skm_handle_settings_action(SkmRemoteServer *server, GHashTable *values)
{
  SkmAppSettings updated = { 0 };
  g_autoptr(GError) error = NULL;
  g_autoptr(GString) message = NULL;
  const gchar *password = NULL;
  gboolean created_file = FALSE;
  gboolean password_changed = FALSE;
  gboolean port_changed = FALSE;

  if (server == NULL || server->settings_path == NULL) {
    return skm_operation_result_new(FALSE, FALSE, "Settings path unavailable.");
  }

  created_file = !g_file_test(server->settings_path, G_FILE_TEST_EXISTS);
  skm_app_settings_assign(&updated, &server->settings);
  updated.oled_black_mode = skm_form_get_int(
    values,
    "oled_black_mode",
    updated.oled_black_mode ? 1 : 0) != 0;
  updated.poll_interval_ms = skm_form_get_int(values, "poll_interval_ms", updated.poll_interval_ms);
  updated.fan_debounce_ms = skm_form_get_int(values, "fan_debounce_ms", updated.fan_debounce_ms);
  updated.remote_enabled = skm_form_get_int(
    values,
    "remote_enabled",
    updated.remote_enabled ? 1 : 0) != 0;
  updated.remote_port = skm_form_get_int(values, "remote_port", updated.remote_port);
  password = g_hash_table_lookup(values, "remote_password");
  /* `password` is plaintext (pre-hash). Re-derive salt+hash for the persisted
   * form and drop plaintext before save. NULL = field omitted (keep auth as-is);
   * ""  = clear auth; non-empty = rotate. */
  g_clear_pointer(&updated.remote_password, g_free);
  if (password != NULL) {
    updated.remote_password = g_strdup(password);
  }
  skm_app_settings_clamp(&updated);

  if (password != NULL) {
    gboolean had_auth = server->settings.remote_password_hash != NULL;
    password_changed = (*password != '\0') ? TRUE : had_auth;
    g_clear_pointer(&updated.remote_hmac_salt, g_free);
    g_clear_pointer(&updated.remote_password_hash, g_free);
    if (*password != '\0') {
      updated.remote_hmac_salt = skm_generate_hmac_salt();
      updated.remote_password_hash = skm_hmac_sha256_hex(password, updated.remote_hmac_salt);
    }
  }
  port_changed = updated.remote_port != server->settings.remote_port;

  if (!skm_settings_save(&updated, server->settings_path, &error)) {
    if (updated.remote_password != NULL) {
      memset(updated.remote_password, 0, strlen(updated.remote_password));
    }
    skm_app_settings_clear(&updated);
    return skm_operation_result_new(FALSE, FALSE, error->message);
  }

  skm_remote_server_sync_settings(server, &updated, server->settings_path);
  /* Plaintext has done its job; wipe it from both copies. */
  if (updated.remote_password != NULL) {
    memset(updated.remote_password, 0, strlen(updated.remote_password));
  }
  g_clear_pointer(&updated.remote_password, g_free);
  g_clear_pointer(&server->settings.remote_password, g_free);

  message = g_string_new(created_file ? "settings.ini created." : "settings.ini saved.");
  if (password_changed) {
    g_string_append(message, " Auth updated now.");
  }
  if (port_changed) {
    g_string_append(message, " New port applies on next start.");
  }

  skm_app_settings_clear(&updated);
  return skm_operation_result_new(TRUE, FALSE, message->str);
}

static SkmOperationResult *
skm_handle_action(const gchar *action, GHashTable *values)
{
  SkmService *service = NULL;
  SkmOperationResult *result = NULL;

  if (action == NULL || *action == '\0') {
    return skm_operation_result_new(FALSE, FALSE, "Missing action.");
  }

  service = skm_service_new("/sys", "/proc");
  if (g_strcmp0(action, "fan-apply") == 0) {
    result = skm_service_apply_fan(service, skm_form_get_int(values, "threshold", SKM_FAN_THRESHOLD_DEFAULT));
  } else if (g_strcmp0(action, "fan-reset") == 0) {
    result = skm_service_reset_fan_defaults(service);
  } else if (g_strcmp0(action, "led-apply") == 0) {
    const gchar *effect = g_hash_table_lookup(values, "effect");
    gboolean thermal_mode = skm_form_get_int(values, "thermal_mode", 0) != 0;

    result = skm_service_apply_led(
      service,
      effect != NULL ? effect : "off",
      thermal_mode,
      skm_form_get_int(values, "interval_ms", 2000));
  } else if (g_strcmp0(action, "led-reset") == 0) {
    result = skm_service_reset_led_defaults(service);
  } else if (g_strcmp0(action, "gpu-manual") == 0) {
    result = skm_service_set_gpu_manual(service, TRUE);
  } else if (g_strcmp0(action, "gpu-auto") == 0) {
    result = skm_service_set_gpu_manual(service, FALSE);
  } else if (g_strcmp0(action, "gpu-apply") == 0) {
    SkmOperationResult *manual = NULL;
    gint level_index = skm_form_get_int(values, "level_index", -1);

    if (level_index < 0) {
      result = skm_operation_result_new(FALSE, FALSE, "Pick an SCLK level first.");
    } else {
      manual = skm_service_set_gpu_manual(service, TRUE);
      if (!manual->success) {
        result = skm_operation_result_new(manual->success, manual->permission_denied, manual->message);
      } else {
        result = skm_service_apply_gpu_level(service, level_index);
      }
      skm_operation_result_free(manual);
    }
  } else if (g_strcmp0(action, "gpu-reset") == 0) {
    result = skm_service_reset_gpu_defaults(service);
  } else if (g_strcmp0(action, "hdmi-reprobe") == 0) {
    result = skm_service_reprobe_display(service);
  } else {
    result = skm_operation_result_new(FALSE, FALSE, "Unknown action.");
  }

  skm_service_free(service);
  return result;
}

static GHashTable *
skm_values_from_json_or_form(const gchar *content_type, const gchar *body)
{
  GHashTable *values = NULL;
  g_autofree gchar *profile = NULL;
  g_autofree gchar *password = NULL;
  g_autofree gchar *remote_password = NULL;
  g_autofree gchar *signal_name = NULL;
  gint int_value = 0;
  gboolean bool_value = FALSE;

  if (content_type != NULL && g_strrstr(content_type, "application/json") != NULL) {
    values = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (skm_json_extract_bool(body, "oled_black_mode", &bool_value)) {
      g_hash_table_replace(values, g_strdup("oled_black_mode"), g_strdup(bool_value ? "1" : "0"));
    }
    if (skm_json_extract_int(body, "threshold", &int_value)) {
      g_hash_table_replace(values, g_strdup("threshold"), g_strdup_printf("%d", int_value));
    }
    if (skm_json_extract_int(body, "poll_interval_ms", &int_value)) {
      g_hash_table_replace(values, g_strdup("poll_interval_ms"), g_strdup_printf("%d", int_value));
    }
    if (skm_json_extract_int(body, "fan_debounce_ms", &int_value)) {
      g_hash_table_replace(values, g_strdup("fan_debounce_ms"), g_strdup_printf("%d", int_value));
    }
    if (skm_json_extract_bool(body, "remote_enabled", &bool_value)) {
      g_hash_table_replace(values, g_strdup("remote_enabled"), g_strdup(bool_value ? "1" : "0"));
    }
    if (skm_json_extract_int(body, "remote_port", &int_value)) {
      g_hash_table_replace(values, g_strdup("remote_port"), g_strdup_printf("%d", int_value));
    }
    if (skm_json_extract_int(body, "cols", &int_value)) {
      g_hash_table_replace(values, g_strdup("cols"), g_strdup_printf("%d", int_value));
    }
    if (skm_json_extract_int(body, "rows", &int_value)) {
      g_hash_table_replace(values, g_strdup("rows"), g_strdup_printf("%d", int_value));
    }
    if (skm_json_extract_int(body, "pid", &int_value)) {
      g_hash_table_replace(values, g_strdup("pid"), g_strdup_printf("%d", int_value));
    }
    profile = skm_json_extract_string(body, "profile");
    if (profile != NULL) {
      g_hash_table_replace(values, g_strdup("effect"), g_strdup(profile));
      g_hash_table_replace(values, g_strdup("profile"), g_strdup(profile));
    }
    password = skm_json_extract_string(body, "password");
    if (password != NULL) {
      g_hash_table_replace(values, g_strdup("password"), g_strdup(password));
    }
    remote_password = skm_json_extract_string(body, "remote_password");
    if (remote_password != NULL) {
      g_hash_table_replace(values, g_strdup("remote_password"), g_strdup(remote_password));
    }
    signal_name = skm_json_extract_string(body, "signal");
    if (signal_name != NULL) {
      g_hash_table_replace(values, g_strdup("signal"), g_strdup(signal_name));
    }
    return values;
  }

  return skm_parse_form(body);
}

/* Read, discard, and classify one incoming WS frame. Returns the opcode on
 * success, or -1 if the peer went away / sent an over-sized frame. We only
 * care about control frames (close/ping/pong) on this endpoint; text/binary
 * frames from the client are drained and ignored. */
static gint
skm_ws_drain_inbound_frame(GInputStream *input)
{
  guint8 hdr[2];
  gsize got = 0;
  if (!g_input_stream_read_all(input, hdr, 2, &got, NULL, NULL) || got != 2) {
    return -1;
  }
  guint8 opcode = hdr[0] & 0x0f;
  gboolean masked = (hdr[1] & 0x80) != 0;
  guint64 payload_len = hdr[1] & 0x7f;
  if (payload_len == 126) {
    guint8 ext[2];
    if (!g_input_stream_read_all(input, ext, 2, &got, NULL, NULL) || got != 2) return -1;
    payload_len = ((guint64) ext[0] << 8) | ext[1];
  } else if (payload_len == 127) {
    guint8 ext[8];
    if (!g_input_stream_read_all(input, ext, 8, &got, NULL, NULL) || got != 8) return -1;
    payload_len = 0;
    for (int i = 0; i < 8; i++) payload_len = (payload_len << 8) | ext[i];
  }
  if (masked) {
    guint8 mask[4];
    if (!g_input_stream_read_all(input, mask, 4, &got, NULL, NULL) || got != 4) return -1;
  }
  if (payload_len > 65536) return -1; /* reject oversized frames */
  if (payload_len > 0) {
    g_autofree guint8 *buf = g_malloc(payload_len);
    if (!g_input_stream_read_all(input, buf, payload_len, &got, NULL, NULL) || got != payload_len)
      return -1;
  }
  return (gint) opcode;
}

static gboolean
skm_handle_ws_telemetry(GSocketConnection *connection, GHashTable *headers)
{
  GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
  GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
  const gchar *key = g_hash_table_lookup(headers, "sec-websocket-key");
  SkmService *service = NULL;
  /* Per-connection telemetry state — CPU total, per-core, and per-interface
   * net baselines all live here. Previously every WS client shared the same
   * g_cpu_prev_total/idle globals, and per-core/net bytes were hardcoded to
   * 0; this struct gives each subscriber its own delta window. */
  SkmTelemetryState tstate = {
    .cpu_prev_total = 0,
    .cpu_prev_idle  = 0,
    .cores = g_array_new(FALSE, TRUE, sizeof(SkmCpuCoreBaseline)),
    .nets  = g_array_new(FALSE, TRUE, sizeof(SkmNetBaseline)),
  };
  gint64 now_us = 0, last_send_us = 0, last_ping_us = 0;
  gint fd = -1;

  if (!skm_ws_send_handshake(output, key)) {
    g_array_free(tstate.cores, TRUE);
    g_array_free(tstate.nets, TRUE);
    return FALSE;
  }

  service = skm_service_new("/sys", "/proc");
  fd = g_socket_get_fd(g_socket_connection_get_socket(connection));

  for (;;) {
    now_us = g_get_monotonic_time();

    /* Cadence: push a telemetry JSON every SKM_WS_TELEMETRY_INTERVAL_US.
     * skm_build_telemetry_json uses the caller-supplied state struct so
     * each WS subscriber gets its own CPU / per-core / net deltas, instead
     * of sharing the process-global baseline that made multi-client
     * percentages oscillate. */
    if (now_us - last_send_us >= SKM_WS_TELEMETRY_INTERVAL_US) {
      g_autofree gchar *payload = skm_build_telemetry_json(service, &tstate);
      if (!skm_ws_write_text(output, payload)) {
        goto closed;
      }
      last_send_us = now_us;
    }

    /* Keepalive ping so a silently-dropped socket (e.g. iOS app suspended
     * mid-frame, NAT box forgot the mapping) surfaces as a write error
     * within ~15s instead of leaving a worker thread stuck forever. */
    if (now_us - last_ping_us >= SKM_WS_PING_INTERVAL_US) {
      if (!skm_ws_write_frame(output, 0x9, NULL, 0)) {
        goto closed;
      }
      last_ping_us = now_us;
    }

    /* Poll briefly so we can (a) notice close frames promptly, (b) honour
     * pings from the client, (c) wake up when it's time for the next push. */
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int r = poll(&pfd, 1, SKM_WS_POLL_TIMEOUT_MS);
    if (r < 0) {
      if (errno == EINTR) continue;
      goto closed;
    }
    if (r == 0) continue;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) goto closed;
    if (!(pfd.revents & POLLIN)) continue;

    gint opcode = skm_ws_drain_inbound_frame(input);
    if (opcode < 0) goto closed;
    if (opcode == 0x8) {
      /* Close — echo it back so the client sees a clean shutdown, then
       * return directly. Falling through to `closed:` would send a second
       * close frame, which RFC 6455 §5.5.1 forbids ("a peer does not expect
       * to receive any more data frames" after a close control frame). */
      skm_ws_write_frame(output, 0x8, NULL, 0);
      skm_service_free(service);
      g_array_free(tstate.cores, TRUE);
      g_array_free(tstate.nets, TRUE);
      return TRUE;
    }
    if (opcode == 0x9) {
      /* Ping — RFC 6455 requires a pong with the same payload; we send
       * empty which is acceptable for liveness and avoids echoing attacker
       * data in the less-than-1KB buffer. */
      skm_ws_write_frame(output, 0xA, NULL, 0);
    }
    /* pong (0xA) or any unsupported opcode — ignore. */
  }

closed:
  /* Best-effort close frame — ignore write failure since the peer may be
   * gone already. */
  skm_ws_write_frame(output, 0x8, NULL, 0);
  skm_service_free(service);
  g_array_free(tstate.cores, TRUE);
  g_array_free(tstate.nets, TRUE);
  return TRUE;
}

static gboolean
skm_handle_ws_terminal(GSocketConnection *connection, GHashTable *headers)
{
  GInputStream  *input  = g_io_stream_get_input_stream (G_IO_STREAM(connection));
  GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
  const gchar   *key    = g_hash_table_lookup(headers, "sec-websocket-key");

  if (!skm_ws_send_handshake(output, key))
    return FALSE;

  int master_fd = -1;
  pid_t term_pid = -1;
  if (!skm_term_spawn(&master_fd, &term_pid)) {
    skm_ws_write_text(output, "Failed to spawn shell.\r\n");
    skm_ws_write_frame(output, 0x8, NULL, 0);
    return FALSE;
  }

  /* Run until WS disconnects or pty dies.
   * We do a simple poll loop: read pty → send WS frame,
   * read WS frame → write pty. The shell is this connection's alone and
   * will be reaped via skm_term_teardown() on any exit path below. */
  struct pollfd pfd[2];
  guint8 buf[4096];

  for (;;) {
    pfd[0].fd     = master_fd;
    pfd[0].events = POLLIN;
    pfd[1].fd     = g_socket_get_fd(g_socket_connection_get_socket(connection));
    pfd[1].events = POLLIN;

    int r = poll(pfd, 2, 200);
    if (r < 0) break;

    /* PTY → WS */
    if (pfd[0].revents & POLLIN) {
      ssize_t n = read(master_fd, buf, sizeof(buf));
      if (n <= 0) break;
      g_autofree gchar *text = skm_terminal_sanitize_text(buf, (gsize) n);
      if (!skm_ws_write_text(output, text))
        break;
    }

    /* WS → PTY: read one WS frame */
    if (pfd[1].revents & POLLIN) {
      guint8 hdr[2];
      gsize ignored;
      if (!g_input_stream_read_all(input, hdr, 2, &ignored, NULL, NULL) || ignored != 2)
        break;

      guint8 opcode = hdr[0] & 0x0f;
      gboolean masked = (hdr[1] & 0x80) != 0;
      guint64 payload_len = hdr[1] & 0x7f;

      if (payload_len == 126) {
        guint8 ext[2];
        if (!g_input_stream_read_all(input, ext, 2, &ignored, NULL, NULL)) break;
        payload_len = ((guint64)ext[0] << 8) | ext[1];
      } else if (payload_len == 127) {
        guint8 ext[8];
        if (!g_input_stream_read_all(input, ext, 8, &ignored, NULL, NULL)) break;
        payload_len = 0;
        for (int i = 0; i < 8; i++) payload_len = (payload_len << 8) | ext[i];
      }

      guint8 mask[4] = { 0 };
      if (masked) {
        if (!g_input_stream_read_all(input, mask, 4, &ignored, NULL, NULL)) break;
      }

      if (payload_len > 65536) break; /* sanity */
      guint8 *payload = g_new(guint8, payload_len + 1);
      if (!g_input_stream_read_all(input, payload, payload_len, &ignored, NULL, NULL) || ignored != payload_len) {
        g_free(payload);
        break;
      }
      if (masked) {
        for (guint64 i = 0; i < payload_len; i++)
          payload[i] ^= mask[i % 4];
      }

      if (opcode == 0x8) { /* close */
        g_free(payload);
        break;
      }

      /* resize JSON: {"type":"resize","cols":N,"rows":N} */
      if (opcode == 0x1 && payload_len > 2 && payload[0] == '{') {
        payload[payload_len] = '\0';
        gint cols = 80, rows = 24;
        if (skm_json_extract_int((const gchar *) payload, "cols", &cols) &&
            skm_json_extract_int((const gchar *) payload, "rows", &rows)) {
          struct winsize ws2 = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
          ioctl(master_fd, TIOCSWINSZ, &ws2);
          kill(term_pid, SIGWINCH);
          g_free(payload);
          continue;
        }
      }

      if (payload_len > 0) {
        ssize_t w = write(master_fd, payload, payload_len);
        (void) w; /* best-effort — next poll iteration will observe EIO/EOF */
      }
      g_free(payload);
    }
  }

  /* Politely close the WS then tear down this connection's shell. */
  skm_ws_write_frame(output, 0x8, NULL, 0);
  skm_term_teardown(master_fd, term_pid);
  return TRUE;
}

static gchar *
skm_handle_api_request(SkmRemoteServer *server,
                       const gchar *method,
                       const gchar *path,
                       GHashTable *query,
                       GHashTable *values,
                       GHashTable *headers,
                       const gchar *peer_ip,
                       gint *out_status,
                       gboolean *out_notice_success,
                       gchar **out_notice_message,
                       SkmPostAction *out_post_action)
{
  SkmSnapshot *snapshot = NULL;
  SkmService *service = NULL;
  SkmOperationResult *result = NULL;
  gchar *response = NULL;
  const gchar *action = NULL;

  *out_status = 200;
  if (out_post_action != NULL) *out_post_action = SKM_POST_ACTION_NONE;

  /* ── /auth/login ────────────────────────────────────────────────────── */
  if (g_strcmp0(path, "/auth/login") == 0 && g_strcmp0(method, "POST") == 0) {
    if (!server->auth_required) {
      /* No password configured — return stable token for reconnects. */
      return g_strdup_printf("{\"token\":\"%s\",\"auth_required\":false}",
                             SKM_REMOTE_OPEN_TOKEN);
    }
    if (skm_login_is_locked(server, peer_ip)) {
      *out_status = 429;
      return skm_build_json_detail(429, "Too many failed attempts. Try again later.");
    }
    const gchar *submitted = g_hash_table_lookup(values, "password");
    if (submitted == NULL || *submitted == '\0') {
      *out_status = 401;
      return skm_build_json_detail(401, "Password required.");
    }
    g_autofree gchar *submitted_token = (server->settings.remote_hmac_salt != NULL)
      ? skm_hmac_sha256_hex(submitted, server->settings.remote_hmac_salt)
      : NULL;
    if (submitted_token == NULL || !skm_token_equal(submitted_token, server->auth_token)) {
      skm_login_record_failure(server, peer_ip);
      g_usleep(SKM_LOGIN_FAIL_SLEEP_US);
      *out_status = 401;
      return skm_build_json_detail(401, "Wrong password.");
    }
    skm_login_record_success(server, peer_ip);
    return g_strdup_printf("{\"token\":\"%s\",\"auth_required\":true}",
                           server->auth_token);
  }

  /* ── /auth/verify ────────────────────────────────────────────────────── */
  if (g_strcmp0(path, "/auth/verify") == 0 && g_strcmp0(method, "GET") == 0) {
    /* auth guard below will reject if token bad; if we reach here it's valid */
    return g_strdup_printf("{\"ok\":true,\"auth_required\":%s}",
                           server->auth_required ? "true" : "false");
  }

  /* ── Auth guard (all routes below require valid token if password set) ── */
  if (server->auth_required) {
    const gchar *auth_header = NULL;
    const gchar *bearer = NULL;

    if (headers != NULL)
      auth_header = g_hash_table_lookup(headers, "authorization");

    if (auth_header != NULL && g_str_has_prefix(auth_header, "Bearer "))
      bearer = auth_header + 7;
    else if (headers != NULL)
      bearer = g_hash_table_lookup(headers, "token"); /* WS query param fallback */

    if (bearer == NULL || !skm_token_equal(bearer, server->auth_token)) {
      *out_status = 401;
      return skm_build_json_detail(401, "Invalid or missing token.");
    }
  }

  /* ── /auth/change-password ───────────────────────────────────────────── */
  /* Requires a valid Bearer token (enforced by the guard above) AND
   * knowledge of the current password. The two checks together mean a
   * leaked token alone can't rotate the password — the attacker would
   * also need the plaintext — and a stale password-guess attempt can't
   * rotate without first logging in. */
  if (g_strcmp0(path, "/auth/change-password") == 0 && g_strcmp0(method, "POST") == 0) {
    if (!server->auth_required) {
      *out_status = 400;
      return skm_build_json_detail(400,
        "No password is currently set. Configure remote_password via settings first.");
    }
    if (server->settings_path == NULL) {
      *out_status = 500;
      return skm_build_json_detail(500, "Settings path unavailable; cannot persist rotation.");
    }

    const gchar *current = g_hash_table_lookup(values, "current_password");
    const gchar *next    = g_hash_table_lookup(values, "new_password");
    if (current == NULL || *current == '\0' || next == NULL || *next == '\0') {
      *out_status = 400;
      return skm_build_json_detail(400,
        "Both current_password and new_password are required and must be non-empty.");
    }
    if (strlen(next) < 4) {
      *out_status = 400;
      return skm_build_json_detail(400, "New password must be at least 4 characters.");
    }
    if (g_strcmp0(current, next) == 0) {
      *out_status = 400;
      return skm_build_json_detail(400, "New password must differ from the current one.");
    }

    /* Verify current password matches stored hash, with the same
     * rate-limit + jitter-sleep as /auth/login so this endpoint can't
     * be used to bypass the login throttle. */
    if (skm_login_is_locked(server, peer_ip)) {
      *out_status = 429;
      return skm_build_json_detail(429, "Too many failed attempts. Try again later.");
    }
    g_autofree gchar *current_hash = (server->settings.remote_hmac_salt != NULL)
      ? skm_hmac_sha256_hex(current, server->settings.remote_hmac_salt)
      : NULL;
    if (current_hash == NULL || !skm_token_equal(current_hash, server->auth_token)) {
      skm_login_record_failure(server, peer_ip);
      g_usleep(SKM_LOGIN_FAIL_SLEEP_US);
      *out_status = 401;
      return skm_build_json_detail(401, "Current password is incorrect.");
    }
    skm_login_record_success(server, peer_ip);

    /* Rotate: new salt + hash, persist, then publish the new token.
     * set_password() mutates server->settings in place; we save first
     * using a staged copy so a disk failure doesn't leave the daemon
     * with an unpersisted hash that no settings.ini row matches. */
    g_autofree gchar *new_salt = skm_generate_hmac_salt();
    g_autofree gchar *new_hash = skm_hmac_sha256_hex(next, new_salt);
    if (new_salt == NULL || new_hash == NULL) {
      *out_status = 500;
      return skm_build_json_detail(500, "Failed to derive new password hash.");
    }

    SkmAppSettings updated = { 0 };
    skm_app_settings_assign(&updated, &server->settings);
    g_clear_pointer(&updated.remote_password, g_free);       /* no plaintext on disk */
    g_clear_pointer(&updated.remote_hmac_salt, g_free);
    g_clear_pointer(&updated.remote_password_hash, g_free);
    updated.remote_hmac_salt = g_strdup(new_salt);
    updated.remote_password_hash = g_strdup(new_hash);

    g_autoptr(GError) save_error = NULL;
    if (!skm_settings_save(&updated, server->settings_path, &save_error)) {
      skm_app_settings_clear(&updated);
      *out_status = 500;
      return skm_build_json_detail(500,
        save_error != NULL ? save_error->message : "Failed to persist new password.");
    }

    /* Disk write succeeded — commit to memory. set_password() also
     * clears the old auth_token so any in-flight request using the
     * previous token is rejected on its next call. */
    skm_remote_server_set_password(server, next);
    skm_app_settings_clear(&updated);

    return g_strdup_printf("{\"ok\":true,\"token\":\"%s\",\"auth_required\":true}",
                           server->auth_token);
  }

  if (g_strcmp0(path, "/api/settings") == 0) {
    if (g_strcmp0(method, "GET") == 0) {
      return skm_build_settings_json(server);
    }
    if (g_strcmp0(method, "POST") == 0) {
      result = skm_handle_settings_action(server, values);
      *out_notice_success = result->success;
      *out_notice_message = g_strdup(result->message);
      if (!result->success) {
        *out_status = result->permission_denied ? 403 : 500;
        response = skm_build_json_detail(*out_status, result->message);
      } else {
        response = skm_build_settings_json(server);
      }
      skm_operation_result_free(result);
      return response;
    }
  }

  service  = skm_service_new("/sys", "/proc");
  snapshot = skm_service_read_snapshot(service);

  /* ── Fan ─────────────────────────────────────────────────────────────── */
  if (g_strcmp0(path, "/api/fan/threshold") == 0) {
    if (g_strcmp0(method, "GET") == 0) {
      response = skm_build_fan_threshold_json(
        snapshot->fan.has_threshold_c ? snapshot->fan.threshold_c : SKM_FAN_THRESHOLD_DEFAULT,
        FALSE);
    } else if (g_strcmp0(method, "POST") == 0) {
      gint threshold = skm_form_get_int(values, "threshold", SKM_FAN_THRESHOLD_DEFAULT);
      result = skm_service_apply_fan(service, threshold);
      *out_notice_success = result->success;
      *out_notice_message = g_strdup(result->message);
      if (!result->success) {
        *out_status = result->permission_denied ? 403 : 500;
        response = skm_build_json_detail(*out_status, result->message);
      } else {
        response = skm_build_fan_threshold_json(threshold, TRUE);
      }
    }

  /* ── LED ─────────────────────────────────────────────────────────────── */
  } else if (g_strcmp0(path, "/api/led/profiles") == 0 && g_strcmp0(method, "GET") == 0) {
    response = skm_build_led_profiles_json(&snapshot->led);
  } else if (g_strcmp0(path, "/api/led/active") == 0 && g_strcmp0(method, "GET") == 0) {
    response = skm_build_led_active_json(&snapshot->led);
  } else if (g_strcmp0(path, "/api/led") == 0 && g_strcmp0(method, "POST") == 0) {
    const gchar *profile = g_hash_table_lookup(values, "profile");
    result = skm_service_apply_led(service, profile != NULL ? profile : "off", FALSE, 2000);
    *out_notice_success = result->success;
    *out_notice_message = g_strdup(result->message);
    if (!result->success) {
      *out_status = result->permission_denied ? 403 : 500;
      response = skm_build_json_detail(*out_status, result->message);
    } else {
      g_autofree gchar *escaped = skm_json_escape(profile != NULL ? profile : "off");
      response = g_strdup_printf("{\"profile\":\"%s\"}", escaped);
    }

  /* ── Processes ───────────────────────────────────────────────────────── */
  } else if (g_strcmp0(path, "/api/system/processes") == 0 && g_strcmp0(method, "GET") == 0) {
    const gchar *limit_str = g_hash_table_lookup(query, "limit");
    const gchar *sort_by = g_hash_table_lookup(query, "sort_by");
    gint limit = limit_str ? CLAMP(atoi(limit_str), 1, SKM_REMOTE_PROCESS_LIMIT_MAX)
                           : SKM_REMOTE_PROCESS_LIMIT_DEFAULT;
    response = skm_build_processes_json(limit, sort_by);

  } else if (g_strcmp0(path, "/api/system/process/kill") == 0 && g_strcmp0(method, "POST") == 0) {
    const gchar *pid_str = g_hash_table_lookup(values, "pid");
    const gchar *sig_str = g_hash_table_lookup(values, "signal");
    if (pid_str == NULL) {
      *out_status = 422;
      response = skm_build_json_detail(422, "pid required.");
    } else {
      pid_t pid = (pid_t) atoi(pid_str);
      int sig = SIGTERM;
      if (g_strcmp0(sig_str, "SIGKILL") == 0) sig = SIGKILL;
      else if (g_strcmp0(sig_str, "SIGSTOP") == 0) sig = SIGSTOP;
      else if (g_strcmp0(sig_str, "SIGCONT") == 0) sig = SIGCONT;
      else if (g_strcmp0(sig_str, "SIGHUP")  == 0) sig = SIGHUP;

      const gchar *reject_reason = NULL;
      if (pid <= 1) {
        reject_reason = "Refusing to signal init / PID <= 1.";
      } else if (pid == 2) {
        reject_reason = "Refusing to signal kthreadd.";
      } else {
        /* Reject kernel threads (parent PID == 0 or == 2): they can't be
         * killed from userspace and attempting to do so is always a mistake. */
        g_autofree gchar *status_path = g_strdup_printf("/proc/%d/status", (int) pid);
        g_autofree gchar *status_body = NULL;
        if (g_file_get_contents(status_path, &status_body, NULL, NULL) && status_body != NULL) {
          const gchar *p = strstr(status_body, "\nPPid:");
          if (p == NULL && g_str_has_prefix(status_body, "PPid:")) p = status_body;
          if (p != NULL) {
            while (*p && (*p == 'P' || *p == 'i' || *p == 'd' || *p == ':' || *p == '\n' || *p == '\t' || *p == ' ')) p++;
            gint ppid = atoi(p);
            if (ppid == 0 || ppid == 2) {
              reject_reason = "Refusing to signal kernel thread.";
            }
          }
        } else {
          /* /proc lookup failed — process likely doesn't exist. Let the
           * kill() call produce the canonical ESRCH error below. */
        }

        /* Deny-list common critical system daemons by comm name. This is
         * not security (a malicious caller can still use SIGKILL on any
         * PID they can reach) — it's a guard rail so that an accidental
         * tap on the iOS Processes tab can't take the PS4 offline. */
        if (reject_reason == NULL) {
          g_autofree gchar *comm_path = g_strdup_printf("/proc/%d/comm", (int) pid);
          g_autofree gchar *comm = NULL;
          if (g_file_get_contents(comm_path, &comm, NULL, NULL) && comm != NULL) {
            g_strstrip(comm);
            static const gchar *critical[] = {
              "systemd", "init", "kthreadd", "ksoftirqd", "migration",
              "rcu_sched", "rcu_bh", "watchdog", "kworker",
              "strawberry-kernel-manager", /* don't kill ourselves */
              NULL,
            };
            for (gsize i = 0; critical[i] != NULL; i++) {
              if (g_str_has_prefix(comm, critical[i])) {
                reject_reason = "Refusing to signal a critical system process.";
                break;
              }
            }
          }
        }
      }

      if (reject_reason != NULL) {
        *out_status = 403;
        response = skm_build_json_detail(403, reject_reason);
      } else if (kill(pid, sig) == 0) {
        response = g_strdup_printf("{\"pid\":%d,\"signal\":\"%s\",\"ok\":true}",
                                   (int) pid, sig_str != NULL ? sig_str : "SIGTERM");
      } else {
        *out_status = (errno == EPERM) ? 403 : 404;
        response = skm_build_json_detail(*out_status, g_strerror(errno));
      }
    }

  /* ── System journal ──────────────────────────────────────────────────── */
  } else if (g_strcmp0(path, "/api/system/logs") == 0 && g_strcmp0(method, "GET") == 0) {
    const gchar *lines_str = g_hash_table_lookup(query, "lines");
    const gchar *priority  = g_hash_table_lookup(query, "priority");
    /* Default is 500 — enough to tell "what's happening right now" on a
     * crash without producing a multi-MB body on every open. 2000 is the
     * hard ceiling enforced inside skm_build_logs_json. */
    gint lines = lines_str != NULL ? atoi(lines_str) : 500;
    response = skm_build_logs_json(lines, priority, out_status);

  /* ── Power ───────────────────────────────────────────────────────────── */
  } else if (g_str_has_prefix(path, "/api/power/") && g_strcmp0(method, "POST") == 0) {
    const gchar *act = path + strlen("/api/power/");
    /* Defer reboot()/poweroff() until the HTTP response has been flushed to
     * the client and the connection closed. Calling reboot() inline here
     * meant the client saw a TCP RST partway through the response and
     * reported the command as failed, even though the kernel was already
     * tearing things down. */
    if (g_strcmp0(act, "reboot") == 0) {
      response = g_strdup("{\"action\":\"reboot\",\"ok\":true}");
      if (out_post_action != NULL) *out_post_action = SKM_POST_ACTION_REBOOT;
    } else if (g_strcmp0(act, "poweroff") == 0 || g_strcmp0(act, "shutdown") == 0) {
      response = g_strdup("{\"action\":\"poweroff\",\"ok\":true}");
      if (out_post_action != NULL) *out_post_action = SKM_POST_ACTION_POWEROFF;
    } else {
      *out_status = 422;
      response = skm_build_json_detail(422, "Unknown power action.");
    }

  /* ── Tunnel (stub — no cloudflared on PS4) ───────────────────────────── */
  } else if (g_str_has_prefix(path, "/api/tunnel/")) {
    response = skm_build_tunnel_status_json();

  /* ── Files ───────────────────────────────────────────────────────────── */
  } else if (g_strcmp0(path, "/api/files/list") == 0 && g_strcmp0(method, "GET") == 0) {
    const gchar *query_path = g_hash_table_lookup(query, "path");
    response = skm_build_files_json(query_path);

  } else if (g_strcmp0(path, "/api/files/download") == 0 && g_strcmp0(method, "GET") == 0) {
    /* Handled pre-dispatch in skm_remote_server_run so the file can stream
     * straight to the socket without being buffered into @response. If we
     * reach this branch it means the pre-dispatch decided not to take it
     * (e.g. unreachable in practice), so surface a clear 500. */
    *out_status = 500;
    response = skm_build_json_detail(500, "Download handler missed pre-dispatch.");

  } else if (g_strcmp0(path, "/api/files/upload") == 0 && g_strcmp0(method, "POST") == 0) {
    /* Same story as download — upload is handled pre-dispatch to bypass
     * the 64 KB body buffer. */
    *out_status = 500;
    response = skm_build_json_detail(500, "Upload handler missed pre-dispatch.");

  } else if (g_strcmp0(path, "/api/files/delete") == 0 && g_strcmp0(method, "DELETE") == 0) {
    const gchar *target_path = g_hash_table_lookup(query, "path");
    const gchar *reason = NULL;
    g_autofree gchar *safe_path = skm_files_validate_path(target_path, &reason);
    if (safe_path == NULL) {
      *out_status = 400;
      response = skm_build_json_detail(400, reason != NULL ? reason : "Path not allowed.");
    } else {
      GStatBuf st;
      if (g_stat(safe_path, &st) != 0) {
        *out_status = 404;
        response = skm_build_json_detail(404, g_strerror(errno));
      } else if (S_ISDIR(st.st_mode)) {
        /* Recursive directory delete is a foot-gun for a remote UI — one
         * swipe could wipe /home. Require the caller to send a follow-up
         * request for each file, or to enable `recursive=1` explicitly. */
        const gchar *recursive = g_hash_table_lookup(query, "recursive");
        if (g_strcmp0(recursive, "1") != 0 && g_strcmp0(recursive, "true") != 0) {
          *out_status = 409;
          response = skm_build_json_detail(409,
            "Target is a directory; pass ?recursive=1 to confirm.");
        } else if (g_rmdir(safe_path) == 0) {
          /* Escape safe_path — Linux filenames can contain " and \, which
           * would produce malformed JSON without this pass. */
          g_autofree gchar *escaped = skm_json_escape(safe_path);
          response = g_strdup_printf(
            "{\"ok\":true,\"path\":\"%s\",\"kind\":\"dir\"}", escaped);
        } else {
          *out_status = errno == ENOTEMPTY ? 409 : 500;
          response = skm_build_json_detail(*out_status, g_strerror(errno));
        }
      } else if (g_unlink(safe_path) == 0) {
        g_autofree gchar *escaped = skm_json_escape(safe_path);
        response = g_strdup_printf(
          "{\"ok\":true,\"path\":\"%s\",\"kind\":\"file\"}", escaped);
      } else {
        *out_status = errno == EACCES ? 403 : 500;
        response = skm_build_json_detail(*out_status, g_strerror(errno));
      }
    }

  /* ── GPU level control (SKM-native) ──────────────────────────────────── */
  } else if (g_str_has_prefix(path, "/api/gpu/")) {
    const gchar *gpu_act = path + strlen("/api/gpu/");
    if (g_strcmp0(gpu_act, "manual") == 0 && g_strcmp0(method, "POST") == 0) {
      gboolean enabled = g_strcmp0(g_hash_table_lookup(values, "enabled"), "true") == 0;
      result = skm_service_set_gpu_manual(service, enabled);
      response = result->success ? g_strdup("{\"ok\":true}")
                                 : skm_build_json_detail(500, result->message);
    } else if (g_strcmp0(gpu_act, "level") == 0 && g_strcmp0(method, "POST") == 0) {
      gint level = skm_form_get_int(values, "level", 0);
      result = skm_service_apply_gpu_level(service, level);
      response = result->success ? g_strdup("{\"ok\":true}")
                                 : skm_build_json_detail(500, result->message);
    } else {
      *out_status = 404;
      response = skm_build_json_detail(404, "GPU route not found.");
    }

  /* ── Unknown API route / action fallback ────────────────────────────── */
  } else {
    action = g_hash_table_lookup(values, "action");
    if (action != NULL) {
      result = skm_handle_action(action, values);
      *out_notice_success = result->success;
      *out_notice_message = g_strdup(result->message);
      if (!result->success) {
        *out_status = result->permission_denied ? 403 : 500;
        response = skm_build_json_detail(*out_status, result->message);
      } else {
        response = g_strdup("{\"ok\":true}");
      }
    }
  }

  if (result   != NULL) skm_operation_result_free(result);
  if (snapshot != NULL) skm_snapshot_free(snapshot);
  skm_service_free(service);

  if (response == NULL) {
    *out_status = 404;
    response = skm_build_json_detail(404, "Route not found.");
  }
  return response;
}

static gboolean
skm_remote_server_run(GThreadedSocketService *service,
                      GSocketConnection *connection,
                      GObject *source_object,
                      gpointer user_data)
{
  SkmRemoteServer *server = user_data;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;
  g_autoptr(GDataInputStream) data_input = NULL;
  g_autofree gchar *request_line = NULL;
  g_autofree gchar *method = NULL;
  g_autofree gchar *target = NULL;
  g_autofree gchar *path = NULL;
  g_autofree gchar *query_text = NULL;
  g_autofree gchar *body = NULL;
  g_autofree gchar *response_body = NULL;
  g_autofree gchar *peer = NULL;
  g_autofree gchar *peer_ip = NULL;
  g_autofree gchar *notice_message = NULL;
  g_autoptr(GHashTable) headers = NULL;
  g_autoptr(GHashTable) query = NULL;
  g_autoptr(GHashTable) values = NULL;
  gchar **parts = NULL;
  gint status = 200;
  gsize line_length = 0;
  gsize content_length = 0;
  const gchar *content_type = "text/plain; charset=utf-8";
  gboolean notice_success = FALSE;
  gboolean emit_notice = FALSE;
  SkmService *snapshot_service = NULL;
  SkmSnapshot *snapshot = NULL;
  SkmPostAction post_action = SKM_POST_ACTION_NONE;

  (void) service;
  (void) source_object;

  input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
  output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
  data_input = g_data_input_stream_new(input);
  g_data_input_stream_set_newline_type(data_input, G_DATA_STREAM_NEWLINE_TYPE_CR_LF);

  request_line = g_data_input_stream_read_line(data_input, &line_length, NULL, NULL);
  if (request_line == NULL || line_length == 0) {
    goto done;
  }

  parts = g_strsplit(request_line, " ", 3);
  if (parts[0] == NULL || parts[1] == NULL) {
    status = 400;
    response_body = g_strdup("Bad request.");
    goto done;
  }

  method = g_strdup(parts[0]);
  target = g_strdup(parts[1]);
  g_strfreev(parts);
  parts = NULL;

  headers = skm_read_headers(data_input, &content_length);

  path = g_strdup(target);
  if (path != NULL) {
    gchar *query_sep = strchr(path, '?');

    if (query_sep != NULL) {
      query_text = g_strdup(query_sep + 1);
      *query_sep = '\0';
    }
  }
  skm_normalize_path(path);
  query = skm_parse_form(query_text);

  /* ── File upload / download pre-dispatch ─────────────────────────────────
   * These two endpoints need streaming I/O: the 64 KB body ceiling and the
   * skm_build_json_detail response path are both the wrong shape for
   * multi-MB file transfers. Handle them here, before the size cap is
   * enforced on normal requests, and bypass skm_write_response() so we can
   * stream chunks straight to the socket. */
  if (g_strcmp0(path, "/api/files/download") == 0 && g_strcmp0(method, "GET") == 0) {
    if (!skm_remote_auth_check(server, headers)) {
      skm_write_response(output, 401, "application/json; charset=utf-8",
                         skm_build_json_detail(401, "Invalid or missing token."));
      g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
      return TRUE;
    }
    const gchar *target_path = g_hash_table_lookup(query, "path");
    const gchar *reason = NULL;
    g_autofree gchar *safe_path = skm_files_validate_path(target_path, &reason);
    if (safe_path == NULL) {
      skm_write_response(output, 400, "application/json; charset=utf-8",
                         skm_build_json_detail(400, reason != NULL ? reason : "Path not allowed."));
    } else if (!skm_stream_file_response(output, safe_path)) {
      skm_write_response(output, 500, "application/json; charset=utf-8",
                         skm_build_json_detail(500, g_strerror(errno)));
    }
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    return TRUE;
  }

  if (g_strcmp0(path, "/api/files/upload") == 0 && g_strcmp0(method, "POST") == 0) {
    /* Cap upload size at 128 MiB. Anything larger should be pushed over
     * SCP / the PS4's usual transfer path; the daemon's job is convenience,
     * not bulk-data serving. */
    const gsize upload_max = (gsize) 128 * 1024 * 1024;
    if (!skm_remote_auth_check(server, headers)) {
      skm_write_response(output, 401, "application/json; charset=utf-8",
                         skm_build_json_detail(401, "Invalid or missing token."));
      g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
      return TRUE;
    }
    if (content_length == 0) {
      skm_write_response(output, 411, "application/json; charset=utf-8",
                         skm_build_json_detail(411, "Content-Length required for upload."));
      g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
      return TRUE;
    }
    if (content_length > upload_max) {
      skm_write_response(output, 413, "application/json; charset=utf-8",
                         skm_build_json_detail(413, "Upload exceeds 128 MiB limit."));
      g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
      return TRUE;
    }
    const gchar *target_path = g_hash_table_lookup(query, "path");
    const gchar *reason = NULL;
    g_autofree gchar *safe_path = skm_files_validate_upload_target(target_path, &reason);
    if (safe_path == NULL) {
      skm_write_response(output, 400, "application/json; charset=utf-8",
                         skm_build_json_detail(400, reason != NULL ? reason : "Path not allowed."));
      g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
      return TRUE;
    }
    gsize written = 0;
    if (!skm_stream_upload_to_file(G_INPUT_STREAM(data_input), safe_path, content_length, &written)) {
      skm_write_response(output, 500, "application/json; charset=utf-8",
                         skm_build_json_detail(500, g_strerror(errno)));
    } else {
      g_autofree gchar *escaped = skm_json_escape(safe_path);
      g_autofree gchar *resp = g_strdup_printf(
        "{\"ok\":true,\"path\":\"%s\",\"bytes\":%" G_GSIZE_FORMAT "}",
        escaped, written);
      skm_write_response(output, 200, "application/json; charset=utf-8", resp);
    }
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    return TRUE;
  }

  if (content_length > 65536) {
    status = 400;
    response_body = g_strdup("Request body too large.");
    goto done;
  }

  if (content_length > 0) {
    /* Keep reading from the buffered data stream.
     * Switching back to raw socket stream can block if body bytes were
     * already prefetched while parsing headers. */
    body = skm_read_body(G_INPUT_STREAM(data_input), content_length);
  }
  peer = skm_remote_identify_peer(connection);
  skm_remote_record_client(server, peer, method, path, g_hash_table_lookup(headers, "user-agent"));

  if (skm_is_websocket_request(headers)) {
    /* Modern clients send "Authorization: Bearer <token>" on the WS
     * upgrade request. Legacy clients (pre-iOS-PR#15) pass the token
     * via ?token=<hex> in the query string — audit finding C7 — which
     * leaks the token into every access log on the path. Accept both
     * but warn once per connection when only the legacy path is used,
     * so deployers can spot stragglers before the fallback is removed. */
    const gchar *qs_token = g_hash_table_lookup(query, "token");
    const gchar *auth_hdr = g_hash_table_lookup(headers, "authorization");
    const gboolean has_bearer =
      auth_hdr != NULL && g_str_has_prefix(auth_hdr, "Bearer ");

    if (qs_token != NULL && !has_bearer) {
      g_warning(
        "deprecated WS auth via ?token= query string from %s (path %s); "
        "client should send \"Authorization: Bearer <token>\" on the "
        "upgrade request instead (audit C7).",
        peer != NULL ? peer : "?", path);
    }

    /* Auth guard for WS endpoints — mirror the REST guard precedence:
     * Authorization: Bearer wins; fall back to ?token= query string for
     * legacy clients. */
    if (server->auth_required) {
      const gchar *bearer = NULL;
      if (has_bearer) {
        bearer = auth_hdr + 7;
      } else if (qs_token != NULL) {
        bearer = qs_token;
      }
      if (bearer == NULL || !skm_token_equal(bearer, server->auth_token)) {
        skm_write_response(output, 401, "application/json; charset=utf-8",
                           skm_build_json_detail(401, "Unauthorized"));
        g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
        return TRUE;
      }
    }

    if (g_strcmp0(path, "/ws/telemetry") == 0) {
      skm_handle_ws_telemetry(connection, headers);
    } else if (g_strcmp0(path, "/ws/terminal") == 0) {
      skm_handle_ws_terminal(connection, headers);
    } else {
      response_body = skm_build_json_detail(404, "WebSocket route not found.");
      skm_write_response(output, 404, "application/json; charset=utf-8", response_body);
    }
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    return TRUE;
  }

  if (g_strcmp0(method, "GET") != 0 &&
      g_strcmp0(method, "POST") != 0 &&
      g_strcmp0(method, "DELETE") != 0) {
    status = 405;
    response_body = g_strdup("Method not allowed.");
    goto done;
  }

  if (g_strcmp0(path, "/") == 0 && g_strcmp0(method, "GET") == 0) {
    response_body = skm_build_health_json(server);
    content_type = "application/json; charset=utf-8";
    goto done;
  }

  values = skm_values_from_json_or_form(g_hash_table_lookup(headers, "content-type"), body);
  /* peer_ip is declared at the top of the function (not here) because several
   * earlier `goto done` edges — including the `GET /` short-circuit — skip
   * past this point. Leaving the declaration here meant g_autofree's cleanup
   * attribute fired on an uninitialized stack slot, which showed up as
   * `free(): invalid size` on -O2 release builds (ASan's -O0 zero-init
   * happened to mask it). */
  peer_ip = skm_remote_peer_ip(peer);
  response_body = skm_handle_api_request(
    server,
    method,
    path,
    query,
    values,
    headers,
    peer_ip,
    &status,
    &notice_success,
    &notice_message,
    &post_action);
  if (notice_message != NULL) {
    skm_remote_emit_notice(server, notice_success, notice_success, notice_message);
  }
  content_type = "application/json; charset=utf-8";

done:
  if (response_body == NULL) {
    response_body = g_strdup("Request failed.");
  }
  skm_write_response(output, status, content_type, response_body);

  if (snapshot != NULL) {
    skm_snapshot_free(snapshot);
  }
  if (snapshot_service != NULL) {
    skm_service_free(snapshot_service);
  }
  g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
  (void) emit_notice;

  /* Now that the response has been flushed and the TCP connection closed,
   * it is safe to initiate the reboot/poweroff. The client will have seen a
   * clean 200 instead of a half-written response followed by RST. */
  if (post_action == SKM_POST_ACTION_REBOOT) {
    sync();
    reboot(RB_AUTOBOOT);
  } else if (post_action == SKM_POST_ACTION_POWEROFF) {
    sync();
    reboot(RB_POWER_OFF);
  }

  return TRUE;
}

SkmRemoteServer *
skm_remote_server_new(SkmRemoteNoticeFunc notice_cb, gpointer user_data)
{
  SkmRemoteServer *server = g_new0(SkmRemoteServer, 1);
  gboolean migrated = FALSE;

  server->notice_cb = notice_cb;
  server->notice_user_data = user_data;
  server->recent_clients = g_ptr_array_new_with_free_func(g_free);
  g_mutex_init(&server->lock);
  g_mutex_init(&server->login_lock);
  server->login_attempts = g_hash_table_new_full(
    g_str_hash, g_str_equal, g_free, g_free);

  skm_settings_load(&server->settings, &server->settings_path, NULL);

  /* Legacy migration: older settings.ini stored remote_password in plaintext.
   * Derive a per-install salt + hash once, then drop plaintext on next save. */
  if (server->settings.remote_password != NULL &&
      server->settings.remote_password_hash == NULL) {
    g_clear_pointer(&server->settings.remote_hmac_salt, g_free);
    server->settings.remote_hmac_salt = skm_generate_hmac_salt();
    server->settings.remote_password_hash = skm_hmac_sha256_hex(
      server->settings.remote_password,
      server->settings.remote_hmac_salt);
    migrated = TRUE;
  }

  if (server->settings.remote_password_hash != NULL &&
      server->settings.remote_hmac_salt != NULL) {
    server->auth_required = TRUE;
    server->auth_token = g_strdup(server->settings.remote_password_hash);
  } else {
    server->auth_required = FALSE;
  }

  /* Wipe any plaintext we only read to trigger migration. */
  if (server->settings.remote_password != NULL) {
    memset(server->settings.remote_password, 0,
           strlen(server->settings.remote_password));
    g_clear_pointer(&server->settings.remote_password, g_free);
  }

  if (migrated && server->settings_path != NULL) {
    skm_settings_save(&server->settings, server->settings_path, NULL);
  }

  return server;
}

void
skm_remote_server_set_password(SkmRemoteServer *server, const gchar *password)
{
  g_return_if_fail(server != NULL);

  g_clear_pointer(&server->auth_token, g_free);

  if (password != NULL && *password != '\0') {
    /* Generate a fresh salt on each rotation so old leaked hashes can't be
     * reused, and update the in-memory settings so the next save persists
     * the new values (plaintext is never stored). */
    g_clear_pointer(&server->settings.remote_hmac_salt, g_free);
    g_clear_pointer(&server->settings.remote_password_hash, g_free);
    server->settings.remote_hmac_salt = skm_generate_hmac_salt();
    server->settings.remote_password_hash = skm_hmac_sha256_hex(
      password, server->settings.remote_hmac_salt);
    server->auth_required = TRUE;
    server->auth_token = g_strdup(server->settings.remote_password_hash);
  } else {
    g_clear_pointer(&server->settings.remote_hmac_salt, g_free);
    g_clear_pointer(&server->settings.remote_password_hash, g_free);
    server->auth_required = FALSE;
  }
}

void
skm_remote_server_sync_settings(SkmRemoteServer *server,
                                const SkmAppSettings *settings,
                                const gchar *settings_path)
{
  g_autofree gchar *path_copy = NULL;

  g_return_if_fail(server != NULL);
  g_return_if_fail(settings != NULL);

  path_copy = settings_path != NULL ? g_strdup(settings_path) : NULL;
  skm_app_settings_assign(&server->settings, settings);
  if (path_copy != NULL) {
    g_clear_pointer(&server->settings_path, g_free);
    server->settings_path = g_steal_pointer(&path_copy);
  }

  /* If the caller supplied a plaintext password, rotate through
   * set_password() which regenerates salt+hash. Otherwise, adopt whatever
   * hashed form was passed in (may be NULL to disable auth). */
  if (server->settings.remote_password != NULL &&
      *server->settings.remote_password != '\0') {
    g_autofree gchar *plaintext = g_strdup(server->settings.remote_password);
    memset(server->settings.remote_password, 0,
           strlen(server->settings.remote_password));
    g_clear_pointer(&server->settings.remote_password, g_free);
    skm_remote_server_set_password(server, plaintext);
    memset(plaintext, 0, strlen(plaintext));
  } else {
    g_clear_pointer(&server->auth_token, g_free);
    if (server->settings.remote_password_hash != NULL &&
        server->settings.remote_hmac_salt != NULL) {
      server->auth_required = TRUE;
      server->auth_token = g_strdup(server->settings.remote_password_hash);
    } else {
      server->auth_required = FALSE;
    }
    g_clear_pointer(&server->settings.remote_password, g_free);
  }
}

void
skm_remote_server_free(SkmRemoteServer *server)
{
  if (server == NULL) {
    return;
  }

  skm_remote_server_stop(server);
  g_mutex_clear(&server->lock);
  g_mutex_clear(&server->login_lock);
  g_clear_pointer(&server->login_attempts, g_hash_table_unref);
  g_clear_pointer(&server->recent_clients, g_ptr_array_unref);
  g_clear_pointer(&server->last_client,    g_free);
  g_clear_pointer(&server->auth_token,     g_free);
  g_clear_pointer(&server->settings_path,  g_free);
  skm_app_settings_clear(&server->settings);
  g_free(server);
}

gboolean
skm_remote_server_start(SkmRemoteServer *server, gint port, GError **error)
{
  g_return_val_if_fail(server != NULL, FALSE);

  if (server->service != NULL && server->port == port) {
    return TRUE;
  }

  skm_remote_server_stop(server);

  server->service = G_THREADED_SOCKET_SERVICE(g_threaded_socket_service_new(8));
  g_signal_connect(server->service, "run", G_CALLBACK(skm_remote_server_run), server);

  if (server->auth_required) {
    /* Password is set — it's safe to accept connections from any interface. */
    if (!g_socket_listener_add_inet_port(
          G_SOCKET_LISTENER(server->service),
          (guint16) port,
          NULL,
          error)) {
      g_clear_object(&server->service);
      return FALSE;
    }
  } else {
    /* No password configured. The server exposes power-off/reboot,
     * process kill, sysfs writes, etc. — binding to the world would be
     * catastrophic on a shared LAN. Restrict to loopback so the user must
     * intentionally tunnel (SSH, VPN) to reach it. */
    g_autoptr(GInetAddress) loopback =
      g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    g_autoptr(GSocketAddress) sockaddr =
      g_inet_socket_address_new(loopback, (guint16) port);
    if (!g_socket_listener_add_address(
          G_SOCKET_LISTENER(server->service),
          sockaddr,
          G_SOCKET_TYPE_STREAM,
          G_SOCKET_PROTOCOL_TCP,
          NULL,
          NULL,
          error)) {
      g_clear_object(&server->service);
      return FALSE;
    }
  }

  g_socket_service_start(G_SOCKET_SERVICE(server->service));
  server->port = port;
  return TRUE;
}

void
skm_remote_server_stop(SkmRemoteServer *server)
{
  if (server == NULL || server->service == NULL) {
    return;
  }

  g_socket_service_stop(G_SOCKET_SERVICE(server->service));
  g_socket_listener_close(G_SOCKET_LISTENER(server->service));
  g_clear_object(&server->service);
  server->port = 0;
}

gboolean
skm_remote_server_is_running(SkmRemoteServer *server)
{
  return server != NULL && server->service != NULL;
}

gint
skm_remote_server_get_port(SkmRemoteServer *server)
{
  return server != NULL ? server->port : 0;
}

int
skm_remote_run_headless(const SkmAppSettings *settings, gint port_override)
{
  g_autoptr(GMainLoop) loop = NULL;
  g_autoptr(GError) error = NULL;
  SkmAppSettings file_settings = { 0 };
  g_autofree gchar *settings_path = NULL;
  SkmRemoteServer *server = NULL;
  gint port = port_override > 0 ? port_override : settings->remote_port;

  server = skm_remote_server_new(NULL, NULL);
  skm_settings_load(&file_settings, &settings_path, NULL);
  skm_remote_server_sync_settings(server, settings, settings_path);
  skm_app_settings_clear(&file_settings);

  if (!skm_remote_server_start(server, port, &error)) {
    g_printerr("Failed starting headless remote server on port %d: %s\n", port, error->message);
    skm_remote_server_free(server);
    return 1;
  }

  /* The bind address mirrors the auth state: with a password set we listen
   * on every interface, without a password we listen on loopback only (see
   * skm_remote_server_start). Print whichever actually applies so the user
   * isn't surprised when iOS can't reach the daemon from the LAN. */
  g_print("Strawberry Kernel Manager remote listening on http://%s:%d\n",
          server->auth_required ? "0.0.0.0" : "127.0.0.1", port);
  g_print("Strawberry Manager API + telemetry WS + PTY terminal: same host/port\n");
  if (server->auth_required)
    g_print("Auth: password required (HMAC-SHA256 token).\n");
  else
    g_print("Auth: none. Set remote_password in settings.ini to enable.\n");

  loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(loop);

  skm_remote_server_free(server);
  return 0;
}
