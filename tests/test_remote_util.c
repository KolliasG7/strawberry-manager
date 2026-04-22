/* tests/test_remote_util.c — unit coverage for the pure helpers in
 * src/remote-util.c. Run via `meson test -C builddir`. */

#include "../src/skm-remote-util.h"

#include <glib.h>

/* ── skm_json_escape ──────────────────────────────────────────────── */

static void
test_json_escape_null_becomes_empty(void)
{
  /* Callers use the result unconditionally as a JSON string body, so
   * NULL must round-trip to an allocated empty string — not NULL, not
   * crash — otherwise every error path that passes a missing detail
   * would dereference NULL in the composed JSON. */
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
test_json_escape_handles_each_control_char(void)
{
  /* Every escape the function knows about, in one pass. This is the
   * contract every JSON-embedding call site relies on; a regression
   * here would produce invalid JSON and crash the iOS JSON parser. */
  g_autofree gchar *escaped = skm_json_escape("a\\b\"c\nd\re\tf");
  g_assert_cmpstr(escaped, ==, "a\\\\b\\\"c\\nd\\re\\tf");
}

static void
test_json_escape_preserves_utf8_bytes(void)
{
  /* Non-ASCII UTF-8 is valid JSON inside a string (JSON requires only
   * the control chars to be escaped), so the bytes must pass through
   * verbatim rather than being mangled into \uXXXX. This matters for
   * command output in process lists and filenames on the file
   * browser. */
  const gchar *input = "Στρατόπεδο — naïve résumé";
  g_autofree gchar *escaped = skm_json_escape(input);
  g_assert_cmpstr(escaped, ==, input);
}

static void
test_json_escape_handles_consecutive_specials(void)
{
  /* Repeated quote + backslash regressed an earlier GString-backed
   * implementation (each append took the wrong branch). Pin it. */
  g_autofree gchar *escaped = skm_json_escape("\"\"\\\\\n\n");
  g_assert_cmpstr(escaped, ==, "\\\"\\\"\\\\\\\\\\n\\n");
}

static void
test_json_escape_other_control_bytes_pass_through(void)
{
  /* The JSON spec technically requires that every byte below 0x20 be
   * escaped. This function only handles the five shapes everything in
   * the daemon actually emits (\\, \", \n, \r, \t) and leaves other
   * low bytes alone. That's intentional — we never produce a \x01 in
   * our own payloads, and doing the full \uXXXX treatment would bloat
   * the hot path on every process name and log line. Pin the
   * behaviour so future readers don't "fix" it by accident. */
  const gchar input[] = { 'a', 0x01, 'b', 0x1f, 'c', 0 };
  g_autofree gchar *escaped = skm_json_escape(input);
  g_assert_cmpstr(escaped, ==, input);
}

/* ── main ─────────────────────────────────────────────────────────── */

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
  g_test_add_func("/remote-util/json-escape/control-chars-each-escaped",
                  test_json_escape_handles_each_control_char);
  g_test_add_func("/remote-util/json-escape/utf8-preserved",
                  test_json_escape_preserves_utf8_bytes);
  g_test_add_func("/remote-util/json-escape/consecutive-specials",
                  test_json_escape_handles_consecutive_specials);
  g_test_add_func("/remote-util/json-escape/other-control-bytes-passthrough",
                  test_json_escape_other_control_bytes_pass_through);

  return g_test_run();
}
