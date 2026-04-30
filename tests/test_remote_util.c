/* Unit coverage for the pure helpers in src/remote-util.c.
 * Run via `meson test -C builddir`. */

#include "../src/skm-remote-util.h"

#include <glib.h>

static void
test_json_escape_null_becomes_empty(void)
{
  g_autofree gchar *escaped = skm_json_escape(NULL);
  g_assert_nonnull(escaped);
  g_assert_cmpstr(escaped, ==, "");
}

static void
test_json_escape_empty_string_stays_empty(void)
{
  g_autofree gchar *escaped = skm_json_escape("");
  g_assert_cmpstr(escaped, ==, "");
}

static void
test_json_escape_passes_through_plain_ascii(void)
{
  g_autofree gchar *escaped = skm_json_escape("hello world 123");
  g_assert_cmpstr(escaped, ==, "hello world 123");
}

static void
test_json_escape_handles_each_named_control_char(void)
{
  const gchar input[] = {
    'a', '\\', 'b', '"', 'c', '\n', 'd', '\r', 'e', '\t', 'f', '\b', 'g', '\f', 'h', 0
  };
  g_autofree gchar *escaped = skm_json_escape(input);
  g_assert_cmpstr(escaped, ==, "a\\\\b\\\"c\\nd\\re\\tf\\bg\\fh");
}

static void
test_json_escape_preserves_utf8_bytes(void)
{
  const gchar *input =
    "\xce\xa3\xcf\x84\xcf\x81\xce\xb1\xcf\x84\xcf\x8c"
    "\xcf\x80\xce\xb5\xce\xb4\xce\xbf - na\xc3\xafve r\xc3\xa9sum\xc3\xa9";
  g_autofree gchar *escaped = skm_json_escape(input);
  g_assert_cmpstr(escaped, ==, input);
}

static void
test_json_escape_handles_consecutive_specials(void)
{
  g_autofree gchar *escaped = skm_json_escape("\"\"\\\\\n\n");
  g_assert_cmpstr(escaped, ==, "\\\"\\\"\\\\\\\\\\n\\n");
}

static void
test_json_escape_other_control_bytes_use_unicode_escapes(void)
{
  const gchar input[] = { 'a', 0x01, 'b', 0x1f, 'c', 0 };
  g_autofree gchar *escaped = skm_json_escape(input);
  g_assert_cmpstr(escaped, ==, "a\\u0001b\\u001fc");
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/remote-util/json-escape/null-becomes-empty",
                  test_json_escape_null_becomes_empty);
  g_test_add_func("/remote-util/json-escape/empty-string-stays-empty",
                  test_json_escape_empty_string_stays_empty);
  g_test_add_func("/remote-util/json-escape/plain-ascii-passthrough",
                  test_json_escape_passes_through_plain_ascii);
  g_test_add_func("/remote-util/json-escape/named-control-chars",
                  test_json_escape_handles_each_named_control_char);
  g_test_add_func("/remote-util/json-escape/utf8-preserved",
                  test_json_escape_preserves_utf8_bytes);
  g_test_add_func("/remote-util/json-escape/consecutive-specials",
                  test_json_escape_handles_consecutive_specials);
  g_test_add_func("/remote-util/json-escape/other-control-bytes-use-unicode-escapes",
                  test_json_escape_other_control_bytes_use_unicode_escapes);

  return g_test_run();
}
