#include "skm-ui.h"
#include "skm-remote.h"
#include "skm-settings.h"

#include <gtk/gtk.h>

static void
skm_load_css(void)
{
  GtkCssProvider *provider = NULL;
  GdkDisplay *display = NULL;
  const gchar *candidates[] = {
    "data/theme.css",
    "./data/theme.css",
    "../data/theme.css",
    NULL,
  };
  guint i = 0;

  provider = gtk_css_provider_new();
  for (i = 0; candidates[i] != NULL; i++) {
    if (g_file_test(candidates[i], G_FILE_TEST_EXISTS)) {
      gtk_css_provider_load_from_path(provider, candidates[i]);
      break;
    }
  }

  display = gdk_display_get_default();
  if (display != NULL) {
    gtk_style_context_add_provider_for_display(
      display,
      GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }

  g_object_unref(provider);
}

static void
skm_on_activate(GtkApplication *application, gpointer user_data)
{
  GtkWindow *window = NULL;
  (void) user_data;

  skm_load_css();
  window = gtk_application_get_active_window(application);
  if (window == NULL) {
    window = skm_app_window_new(application);
  }
  gtk_window_present(window);
}

int
main(int argc, char **argv)
{
  GtkApplication *application = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GOptionContext) options = NULL;
  SkmAppSettings settings = { 0 };
  gboolean headless = FALSE;
  gint port_override = 0;
  int status = 0;
  GOptionEntry entries[] = {
    { "headless", 0, 0, G_OPTION_ARG_NONE, &headless, "Run Strawberry Manager remote API without GTK window", NULL },
    { "port", 0, 0, G_OPTION_ARG_INT, &port_override, "Override Strawberry Manager remote API port", "PORT" },
    { NULL },
  };

  options = g_option_context_new(NULL);
  g_option_context_add_main_entries(options, entries, NULL);
  if (!g_option_context_parse(options, &argc, &argv, &error)) {
    g_printerr("Option parse failed: %s\n", error->message);
    return 1;
  }

  skm_settings_load(&settings, NULL, NULL);
  if (headless) {
    return skm_remote_run_headless(&settings, port_override);
  }

  application = gtk_application_new(
    "io.strawberry.kernelmanager",
    G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(application, "activate", G_CALLBACK(skm_on_activate), NULL);
  status = g_application_run(G_APPLICATION(application), argc, argv);
  g_object_unref(application);
  return status;
}
