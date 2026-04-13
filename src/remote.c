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

/* ── CPU delta tracking (protected by g_cpu_mutex) ─────────────────────── */
static GMutex  g_cpu_mutex;
static guint64 g_cpu_prev_total = 0;
static guint64 g_cpu_prev_idle  = 0;
static gdouble g_cpu_percent    = 0.0;

/* ── PTY terminal singleton ─────────────────────────────────────────────── */
static GMutex g_term_mutex;
static int    g_term_master_fd = -1;
static pid_t  g_term_pid       = -1;

typedef struct {
  guint64 rx_bytes;
  guint64 tx_bytes;
} SkmRemoteNetCounter;

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
  gchar *auth_password;
  gchar *auth_token;
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

  g_clear_pointer(&settings->remote_password, g_free);
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

/* ── CPU% reader (/proc/stat delta) ────────────────────────────────────── */

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
  gint   pid;
  gchar  comm[64];
  gchar  state;
  gulong utime;
  gulong stime;
  gulong rss_kb;
  gdouble cpu_percent;
} SkmProcEntry;

/* Simple compare for qsort: sort by cpu_percent desc */
static int
skm_proc_cmp_cpu(const void *a, const void *b)
{
  const SkmProcEntry *pa = a, *pb = b;
  if (pa->cpu_percent > pb->cpu_percent) return -1;
  if (pa->cpu_percent < pb->cpu_percent) return  1;
  return 0;
}

static GArray *
skm_read_processes(gint limit)
{
  GArray *arr = g_array_new(FALSE, TRUE, sizeof(SkmProcEntry));
  DIR *proc = opendir("/proc");
  struct dirent *ent;

  if (proc == NULL) return arr;

  while ((ent = readdir(proc)) != NULL && (gint) arr->len < MIN(limit, SKM_REMOTE_PROCESS_LIMIT_MAX)) {
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
      e.utime = utime;
      e.stime = stime;
      /* rss is in pages */
      e.rss_kb = (gulong)rss * (gulong)(sysconf(_SC_PAGESIZE) / 1024);
      /* naive cpu_percent: (utime+stime) / uptime ticks — approximate */
      e.cpu_percent = 0.0; /* filled below */
      g_array_append_val(arr, e);
    }
    fclose(sf);
  }
  closedir(proc);

  /* Compute approximate CPU% using total from /proc/stat */
  g_mutex_lock(&g_cpu_mutex);
  guint64 total_ticks = g_cpu_prev_total;
  g_mutex_unlock(&g_cpu_mutex);
  long hz = sysconf(_SC_CLK_TCK);
  if (hz <= 0) hz = 100;

  for (guint i = 0; i < arr->len; i++) {
    SkmProcEntry *p = &g_array_index(arr, SkmProcEntry, i);
    if (total_ticks > 0) {
      p->cpu_percent = ((gdouble)(p->utime + p->stime) / (gdouble)total_ticks) * (gdouble)hz;
      p->cpu_percent = MIN(p->cpu_percent, 100.0);
    }
  }

  qsort(arr->data, arr->len, sizeof(SkmProcEntry), skm_proc_cmp_cpu);
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

/* ── PTY terminal helpers ───────────────────────────────────────────────── */

static void
skm_term_ensure(void)
{
  g_mutex_lock(&g_term_mutex);
  if (g_term_pid > 0) {
    if (kill(g_term_pid, 0) == 0) {
      g_mutex_unlock(&g_term_mutex);
      return; /* already alive */
    }
    close(g_term_master_fd);
    g_term_master_fd = -1;
    g_term_pid = -1;
  }

  int master_fd, slave_fd;
  if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) < 0) {
    g_mutex_unlock(&g_term_mutex);
    return;
  }

  /* set non-blocking on master */
  int fl = fcntl(master_fd, F_GETFL, 0);
  fcntl(master_fd, F_SETFL, fl | O_NONBLOCK);

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
  if (pid < 0) { close(master_fd); g_mutex_unlock(&g_term_mutex); return; }
  g_term_master_fd = master_fd;
  g_term_pid       = pid;
  g_mutex_unlock(&g_term_mutex);
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
  } else if (status == 426) {
    reason = "Upgrade Required";
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

static gchar *
skm_remote_shorten_agent(const gchar *user_agent)
{
  if (user_agent == NULL || *user_agent == '\0') {
    return g_strdup("unknown client");
  }

  if (g_strrstr(user_agent, "Braska") != NULL) {
    return g_strdup("Braska");
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
    server->settings.remote_password != NULL ? "\"***\"" : "null",
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
  return g_strdup("{\"state\":\"stopped\",\"url\":null}");
}

static gchar *
skm_build_processes_json(gint limit)
{
  GArray *arr = skm_read_processes(limit > 0 ? limit : SKM_REMOTE_PROCESS_LIMIT_DEFAULT);
  GString *json = g_string_new("{\"processes\":[");

  for (guint i = 0; i < arr->len; i++) {
    SkmProcEntry *p = &g_array_index(arr, SkmProcEntry, i);
    g_autofree gchar *escaped_comm = skm_json_escape(p->comm);
    if (i > 0) g_string_append_c(json, ',');
    g_string_append_printf(json,
      "{\"pid\":%d,\"name\":\"%s\",\"state\":\"%c\","
      "\"cpu_percent\":%.1f,\"memory_mb\":%.1f}",
      p->pid, escaped_comm, p->state,
      p->cpu_percent, (gdouble) p->rss_kb / 1024.0);
  }
  g_string_append_printf(json, "],\"count\":%u}", arr->len);
  g_array_free(arr, TRUE);
  return g_string_free(json, FALSE);
}

static gchar *
skm_build_files_json(const gchar *path)
{
  const gchar *safe_path = (path != NULL && *path != '\0') ? path : "/";
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

static gchar *
skm_build_telemetry_json(SkmService *service)
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

  cpu_pct = skm_read_cpu_percent();
  skm_read_cpuinfo((gint *) &cores, &freq_mhz);
  skm_read_loadavg(&l1, &l5, &l15);
  skm_read_meminfo(&mem_total, &mem_used, &mem_avail, &mem_cached, &mem_buffers, &mem_pct,
                   &swap_total, &swap_used);

  json = g_string_new("{");
  g_string_append_printf(json, "\"ts\":%.3f,", g_get_real_time() / 1000000.0);

  /* cpu block */
  g_string_append_printf(json,
    "\"cpu\":{\"percent\":%.1f,\"per_core\":[%.1f],\"core_count\":%d,"
    "\"freq_mhz\":%.0f,\"freq_max_mhz\":%.0f,"
    "\"load_1\":%.2f,\"load_5\":%.2f,\"load_15\":%.2f},",
    cpu_pct, cpu_pct, (gint) cores,
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

  /* net block — summarised per-interface bytes/s (snapshot, no delta) */
  {
    GArray *ifaces = skm_read_net_dev();
    g_string_append(json, "\"net\":[");
    for (guint i = 0; i < ifaces->len; i++) {
      SkmNetIface *n = &g_array_index(ifaces, SkmNetIface, i);
      g_autofree gchar *esc = skm_json_escape(n->iface);
      if (i > 0) g_string_append_c(json, ',');
      /* bytes_sent_s / bytes_recv_s: report 0 (no inter-frame delta here) */
      g_string_append_printf(json,
        "{\"iface\":\"%s\",\"bytes_sent_s\":0,\"bytes_recv_s\":0,"
        "\"packets_sent\":0,\"packets_recv\":0,\"errin\":0,\"errout\":0}",
        esc);
    }
    g_string_append(json, "],");
    g_array_free(ifaces, TRUE);
  }

  g_string_append(json, "\"disk\":[],");
  g_string_append_printf(json, "\"uptime_s\":%d,", uptime_seconds);
  g_string_append(json, "\"tunnel\":{\"state\":\"stopped\",\"url\":null}");
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

static gboolean
skm_ws_read_and_discard(GInputStream *input)
{
  guint8 buffer[4096];

  for (;;) {
    gssize bytes = g_input_stream_read(input, buffer, sizeof(buffer), NULL, NULL);

    if (bytes <= 0) {
      return FALSE;
    }
  }
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
  g_clear_pointer(&updated.remote_password, g_free);
  updated.remote_password = (password != NULL && *password != '\0')
    ? g_strdup(password)
    : NULL;
  skm_app_settings_clamp(&updated);

  password_changed = g_strcmp0(updated.remote_password, server->settings.remote_password) != 0;
  port_changed = updated.remote_port != server->settings.remote_port;

  if (!skm_settings_save(&updated, server->settings_path, &error)) {
    skm_app_settings_clear(&updated);
    return skm_operation_result_new(FALSE, FALSE, error->message);
  }

  skm_remote_server_sync_settings(server, &updated, server->settings_path);

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

static gboolean
skm_handle_ws_telemetry(GSocketConnection *connection, GHashTable *headers)
{
  GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
  GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
  const gchar *key = g_hash_table_lookup(headers, "sec-websocket-key");
  SkmService *service = NULL;

  if (!skm_ws_send_handshake(output, key)) {
    return FALSE;
  }

  service = skm_service_new("/sys", "/proc");
  for (;;) {
    g_autofree gchar *payload = skm_build_telemetry_json(service);

    if (!skm_ws_write_text(output, payload)) {
      break;
    }
    g_usleep(2000000);
  }
  skm_service_free(service);
  (void) input;
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

  skm_term_ensure();

  g_mutex_lock(&g_term_mutex);
  int master_fd = g_term_master_fd;
  g_mutex_unlock(&g_term_mutex);

  if (master_fd < 0) {
    skm_ws_write_text(output, "Failed to spawn shell.\r\n");
    return FALSE;
  }

  /* Run until WS disconnects or pty dies.
   * We do a simple poll loop: read pty → send WS frame,
   * read WS frame → write pty. */
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
          kill(g_term_pid, SIGWINCH);
          g_free(payload);
          continue;
        }
      }

      if (payload_len > 0)
        write(master_fd, payload, payload_len);
      g_free(payload);
    }
  }
  return TRUE;
}

static gchar *
skm_handle_api_request(SkmRemoteServer *server,
                       const gchar *method,
                       const gchar *path,
                       GHashTable *query,
                       GHashTable *values,
                       GHashTable *headers,
                       gint *out_status,
                       gboolean *out_notice_success,
                       gchar **out_notice_message)
{
  SkmSnapshot *snapshot = NULL;
  SkmService *service = NULL;
  SkmOperationResult *result = NULL;
  gchar *response = NULL;
  const gchar *action = NULL;

  *out_status = 200;

  /* ── /auth/login ────────────────────────────────────────────────────── */
  if (g_strcmp0(path, "/auth/login") == 0 && g_strcmp0(method, "POST") == 0) {
    if (!server->auth_required) {
      /* No password configured — return stable token for Braska reconnects. */
      return g_strdup_printf("{\"token\":\"%s\",\"auth_required\":false}",
                             SKM_REMOTE_OPEN_TOKEN);
    }
    const gchar *submitted = g_hash_table_lookup(values, "password");
    if (submitted == NULL || *submitted == '\0') {
      *out_status = 401;
      return skm_build_json_detail(401, "Password required.");
    }
    g_autofree gchar *submitted_token = skm_hmac_sha256_hex(submitted, "braska_v1");
    if (!g_str_equal(submitted_token, server->auth_token)) {
      *out_status = 401;
      return skm_build_json_detail(401, "Wrong password.");
    }
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

    if (bearer == NULL || !g_str_equal(bearer, server->auth_token)) {
      *out_status = 401;
      return skm_build_json_detail(401, "Invalid or missing token.");
    }
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
    gint limit = limit_str ? CLAMP(atoi(limit_str), 1, SKM_REMOTE_PROCESS_LIMIT_MAX)
                           : SKM_REMOTE_PROCESS_LIMIT_DEFAULT;
    response = skm_build_processes_json(limit);

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

      if (pid <= 1) {
        *out_status = 403;
        response = skm_build_json_detail(403, "Refusing to signal PID <= 1.");
      } else if (kill(pid, sig) == 0) {
        response = g_strdup_printf("{\"pid\":%d,\"signal\":\"%s\",\"ok\":true}",
                                   (int) pid, sig_str != NULL ? sig_str : "SIGTERM");
      } else {
        *out_status = (errno == EPERM) ? 403 : 404;
        response = skm_build_json_detail(*out_status, g_strerror(errno));
      }
    }

  /* ── Power ───────────────────────────────────────────────────────────── */
  } else if (g_str_has_prefix(path, "/api/power/") && g_strcmp0(method, "POST") == 0) {
    const gchar *act = path + strlen("/api/power/");
    if (g_strcmp0(act, "reboot") == 0) {
      response = g_strdup("{\"action\":\"reboot\",\"ok\":true}");
      skm_service_free(service); skm_snapshot_free(snapshot);
      sync();
      reboot(RB_AUTOBOOT);
      return response; /* unreachable, but keeps compiler happy */
    } else if (g_strcmp0(act, "poweroff") == 0 || g_strcmp0(act, "shutdown") == 0) {
      response = g_strdup("{\"action\":\"poweroff\",\"ok\":true}");
      skm_service_free(service); skm_snapshot_free(snapshot);
      sync();
      reboot(RB_POWER_OFF);
      return response;
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
    *out_status = 501;
    response = skm_build_json_detail(501, "File download: use HTTP GET /api/files/download?path=...");

  } else if (g_strcmp0(path, "/api/files/upload") == 0 && g_strcmp0(method, "POST") == 0) {
    *out_status = 501;
    response = skm_build_json_detail(501, "File upload not yet implemented.");

  } else if (g_strcmp0(path, "/api/files/delete") == 0 && g_strcmp0(method, "DELETE") == 0) {
    *out_status = 501;
    response = skm_build_json_detail(501, "File delete not yet implemented.");

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
  peer = skm_remote_identify_peer(connection);
  skm_remote_record_client(server, peer, method, path, g_hash_table_lookup(headers, "user-agent"));

  if (skm_is_websocket_request(headers)) {
    /* For WS, Braska passes ?token=<hex> in query string.
     * Inject it as a synthetic "token" header so the auth guard finds it. */
    const gchar *qs_token = g_hash_table_lookup(query, "token");
    if (qs_token != NULL)
      g_hash_table_replace(headers, g_strdup("token"), g_strdup(qs_token));

    /* Auth guard for WS endpoints */
    if (server->auth_required) {
      const gchar *bearer = g_hash_table_lookup(headers, "token");
      if (bearer == NULL || !g_str_equal(bearer, server->auth_token)) {
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
  response_body = skm_handle_api_request(
    server,
    method,
    path,
    query,
    values,
    headers,
    &status,
    &notice_success,
    &notice_message);
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
  return TRUE;
}

SkmRemoteServer *
skm_remote_server_new(SkmRemoteNoticeFunc notice_cb, gpointer user_data)
{
  SkmRemoteServer *server = g_new0(SkmRemoteServer, 1);
  g_autofree gchar *password = NULL;

  server->notice_cb = notice_cb;
  server->notice_user_data = user_data;
  server->recent_clients = g_ptr_array_new_with_free_func(g_free);
  skm_settings_load(&server->settings, &server->settings_path, NULL);
  password = g_strdup(server->settings.remote_password);
  skm_remote_server_set_password(server, password);
  g_mutex_init(&server->lock);
  return server;
}

void
skm_remote_server_set_password(SkmRemoteServer *server, const gchar *password)
{
  g_return_if_fail(server != NULL);

  g_clear_pointer(&server->auth_password, g_free);
  g_clear_pointer(&server->auth_token,    g_free);
  g_clear_pointer(&server->settings.remote_password, g_free);

  if (password != NULL && *password != '\0') {
    server->auth_required = TRUE;
    server->auth_password = g_strdup(password);
    server->auth_token    = skm_hmac_sha256_hex(password, "braska_v1");
    server->settings.remote_password = g_strdup(password);
  } else {
    server->auth_required = FALSE;
  }
}

void
skm_remote_server_sync_settings(SkmRemoteServer *server,
                                const SkmAppSettings *settings,
                                const gchar *settings_path)
{
  g_autofree gchar *password = NULL;
  g_autofree gchar *path_copy = NULL;

  g_return_if_fail(server != NULL);
  g_return_if_fail(settings != NULL);

  path_copy = settings_path != NULL ? g_strdup(settings_path) : NULL;
  skm_app_settings_assign(&server->settings, settings);
  if (path_copy != NULL) {
    g_clear_pointer(&server->settings_path, g_free);
    server->settings_path = g_steal_pointer(&path_copy);
  }
  password = g_strdup(server->settings.remote_password);
  skm_remote_server_set_password(server, password);
}

void
skm_remote_server_free(SkmRemoteServer *server)
{
  if (server == NULL) {
    return;
  }

  skm_remote_server_stop(server);
  g_mutex_clear(&server->lock);
  g_clear_pointer(&server->recent_clients, g_ptr_array_unref);
  g_clear_pointer(&server->last_client,    g_free);
  g_clear_pointer(&server->auth_password,  g_free);
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

  if (!g_socket_listener_add_inet_port(
        G_SOCKET_LISTENER(server->service),
        (guint16) port,
        NULL,
        error)) {
    g_clear_object(&server->service);
    return FALSE;
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

  g_print("Strawberry Kernel Manager remote listening on http://0.0.0.0:%d\n", port);
  g_print("Braska API + telemetry WS + PTY terminal: same host/port\n");
  if (server->auth_required)
    g_print("Auth: password required (HMAC-SHA256 token).\n");
  else
    g_print("Auth: none. Set remote_password in settings.ini to enable.\n");

  loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(loop);

  skm_remote_server_free(server);
  return 0;
}
