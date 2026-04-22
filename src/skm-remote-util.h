/* skm-remote-util.h — pure helpers extracted from remote.c for unit
 * testing. Everything here must be free of global state and daemon
 * lifecycle dependencies so the test binary can link just this
 * translation unit against glib without dragging in GTK, systemd, or
 * the REST server. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Escape a UTF-8 string for embedding inside a JSON string literal.
 * Escapes backslash, double-quote, LF, CR, and TAB; other bytes
 * (including valid non-ASCII UTF-8) pass through verbatim. Does NOT
 * wrap the result in quotes. Accepts NULL (returns an allocated
 * empty string so the caller can treat the return as always
 * non-NULL). Caller owns the returned pointer and must free with
 * g_free(). */
gchar *skm_json_escape(const gchar *text);

G_END_DECLS
