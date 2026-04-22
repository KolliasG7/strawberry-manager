/* remote-util.c — pure helpers (no daemon state, no GTK, no systemd)
 * split out of remote.c so unit tests can link against them without
 * having to bring up the whole REST server. See skm-remote-util.h. */

#include "skm-remote-util.h"

gchar *
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
