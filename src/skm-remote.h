#pragma once

#include "skm-settings.h"

#include <glib.h>

typedef struct _SkmRemoteServer SkmRemoteServer;
typedef void (*SkmRemoteNoticeFunc)(gpointer user_data,
                                    gboolean success,
                                    gboolean refresh,
                                    const gchar *message);

SkmRemoteServer *skm_remote_server_new(SkmRemoteNoticeFunc notice_cb, gpointer user_data);
void skm_remote_server_free(SkmRemoteServer *server);
void skm_remote_server_set_password(SkmRemoteServer *server, const gchar *password);
void skm_remote_server_sync_settings(SkmRemoteServer *server,
                                     const SkmAppSettings *settings,
                                     const gchar *settings_path);

gboolean skm_remote_server_start(SkmRemoteServer *server, gint port, GError **error);
void skm_remote_server_stop(SkmRemoteServer *server);
gboolean skm_remote_server_is_running(SkmRemoteServer *server);
gint skm_remote_server_get_port(SkmRemoteServer *server);

int skm_remote_run_headless(const SkmAppSettings *settings, gint port_override);
