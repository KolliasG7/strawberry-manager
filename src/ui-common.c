#include "skm-ui-private.h"

typedef struct {
  GtkWidget *button;
  gchar *css_class;
} SkmFlashData;

GtkWidget *
skm_make_label(const gchar *text, const gchar *css_class, gfloat xalign, gboolean wrap)
{
  GtkWidget *label = gtk_label_new(text);

  gtk_label_set_xalign(GTK_LABEL(label), xalign);
  gtk_label_set_wrap(GTK_LABEL(label), wrap);
  gtk_label_set_selectable(GTK_LABEL(label), FALSE);
  if (css_class != NULL) {
    gtk_widget_add_css_class(label, css_class);
  }
  return label;
}

GtkWidget *
skm_make_button(const gchar *text, const gchar *css_class)
{
  GtkWidget *button = gtk_button_new_with_label(text);

  if (css_class != NULL) {
    gtk_widget_add_css_class(button, css_class);
  }
  return button;
}

GtkWidget *
skm_make_logo_widget(void)
{
  const gchar *candidates[] = {
    "data/strawberry-logo.svg",
    "./data/strawberry-logo.svg",
    "../data/strawberry-logo.svg",
    NULL,
  };
  guint i = 0;

  for (i = 0; candidates[i] != NULL; i++) {
    if (g_file_test(candidates[i], G_FILE_TEST_EXISTS)) {
      GtkWidget *picture = gtk_picture_new_for_filename(candidates[i]);

      gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
      gtk_widget_set_size_request(picture, 56, 56);
      gtk_widget_add_css_class(picture, "hero-logo-image");
      return picture;
    }
  }

  return skm_make_label("🍓", "hero-logo", 0.0f, FALSE);
}

GtkWidget *
skm_attach_info_row(GtkGrid *grid, gint row, const gchar *title, GtkWidget **out_value)
{
  GtkWidget *key = skm_make_label(title, "dim-label", 0.0f, FALSE);
  GtkWidget *value = skm_make_label("—", "value-label", 1.0f, FALSE);

  gtk_widget_set_halign(value, GTK_ALIGN_END);
  gtk_widget_set_hexpand(value, TRUE);
  gtk_grid_attach(grid, key, 0, row, 1, 1);
  gtk_grid_attach(grid, value, 1, row, 1, 1);

  *out_value = value;
  return value;
}

GtkWidget *
skm_make_metric_tile(const gchar *title, GtkWidget **out_value)
{
  GtkWidget *tile = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *value = NULL;

  gtk_widget_add_css_class(tile, "metric-card");
  gtk_widget_set_hexpand(tile, TRUE);
  gtk_box_append(GTK_BOX(tile), skm_make_label(title, "metric-title", 0.0f, FALSE));
  value = skm_make_label("—", "metric-value", 0.0f, FALSE);
  gtk_box_append(GTK_BOX(tile), value);

  *out_value = value;
  return tile;
}

GtkWidget *
skm_make_control_row(const gchar *title, GtkWidget *widget, GtkWidget *value_widget)
{
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *title_label = skm_make_label(title, "dim-label", 0.0f, FALSE);
  gboolean widget_expands = GTK_IS_RANGE(widget) || GTK_IS_SPIN_BUTTON(widget) || GTK_IS_DROP_DOWN(widget);

  gtk_widget_set_hexpand(row, TRUE);
  gtk_widget_set_size_request(title_label, 150, -1);
  gtk_widget_set_hexpand(widget, widget_expands);
  gtk_widget_set_halign(widget, widget_expands ? GTK_ALIGN_FILL : GTK_ALIGN_END);

  gtk_box_append(GTK_BOX(row), title_label);
  gtk_box_append(GTK_BOX(row), widget);
  if (value_widget != NULL) {
    gtk_box_append(GTK_BOX(row), value_widget);
  }

  return row;
}

static void
skm_toggle_pill_refresh(GtkToggleButton *button, gpointer user_data)
{
  const gchar *off_text = g_object_get_data(G_OBJECT(button), "skm-pill-off");
  const gchar *on_text = g_object_get_data(G_OBJECT(button), "skm-pill-on");
  gboolean active = gtk_toggle_button_get_active(button);

  (void) user_data;

  gtk_button_set_label(GTK_BUTTON(button), active ? on_text : off_text);
  if (active) {
    gtk_widget_add_css_class(GTK_WIDGET(button), "toggle-pill-on");
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(button), "toggle-pill-on");
  }
}

GtkWidget *
skm_make_toggle_pill(const gchar *off_text, const gchar *on_text)
{
  GtkWidget *button = gtk_toggle_button_new_with_label(off_text);

  gtk_widget_add_css_class(button, "toggle-pill");
  g_object_set_data_full(G_OBJECT(button), "skm-pill-off", g_strdup(off_text), g_free);
  g_object_set_data_full(G_OBJECT(button), "skm-pill-on", g_strdup(on_text), g_free);
  g_signal_connect(button, "toggled", G_CALLBACK(skm_toggle_pill_refresh), NULL);
  skm_toggle_pill_refresh(GTK_TOGGLE_BUTTON(button), NULL);
  return button;
}

gboolean
skm_toggle_pill_get_active(GtkWidget *widget)
{
  return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
}

void
skm_toggle_pill_set_active(GtkWidget *widget, gboolean active)
{
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), active);
}

void
skm_create_section_card(const gchar *title,
                        const gchar *subtitle,
                        GtkWidget **out_card,
                        GtkWidget **out_content,
                        GtkWidget **out_actions,
                        GtkWidget **out_unavailable)
{
  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
  GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *unavailable = skm_make_label("Not exposed by this Strawberry PS4 kernel build.", "section-unavailable", 0.0f, TRUE);
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

  gtk_widget_add_css_class(card, "glass-card");
  gtk_widget_set_hexpand(title_box, TRUE);
  gtk_widget_set_halign(actions, GTK_ALIGN_END);

  gtk_box_append(GTK_BOX(title_box), skm_make_label(title, "card-title", 0.0f, FALSE));
  gtk_box_append(GTK_BOX(title_box), skm_make_label(subtitle, "card-subtitle", 0.0f, TRUE));
  gtk_box_append(GTK_BOX(header), title_box);
  gtk_box_append(GTK_BOX(header), actions);
  gtk_box_append(GTK_BOX(card), header);

  gtk_widget_set_visible(unavailable, FALSE);
  gtk_box_append(GTK_BOX(card), unavailable);
  gtk_box_append(GTK_BOX(card), content);

  *out_card = card;
  *out_content = content;
  *out_actions = actions;
  *out_unavailable = unavailable;
}

static gboolean
skm_clear_button_flash_cb(gpointer data)
{
  SkmFlashData *flash = data;

  gtk_widget_remove_css_class(flash->button, flash->css_class);
  g_free(flash->css_class);
  g_free(flash);
  return G_SOURCE_REMOVE;
}

void
skm_flash_button(GtkWidget *button, gboolean success)
{
  SkmFlashData *flash = g_new0(SkmFlashData, 1);

  flash->button = button;
  flash->css_class = g_strdup(success ? "button-success" : "button-error");
  gtk_widget_add_css_class(button, flash->css_class);
  g_timeout_add(900, skm_clear_button_flash_cb, flash);
}

static gboolean
skm_hide_notice_cb(gpointer user_data)
{
  SkmAppWindow *self = user_data;

  self->notice_source_id = 0;
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->notice_revealer), FALSE);
  return G_SOURCE_REMOVE;
}

void
skm_show_notice(SkmAppWindow *self, const gchar *kind, const gchar *message)
{
  gtk_widget_remove_css_class(self->notice_box, "notice-success");
  gtk_widget_remove_css_class(self->notice_box, "notice-error");
  gtk_widget_remove_css_class(self->notice_box, "notice-warning");
  gtk_widget_add_css_class(self->notice_box, kind);
  gtk_label_set_text(GTK_LABEL(self->notice_label), message);
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->notice_revealer), TRUE);

  if (self->notice_source_id != 0) {
    g_source_remove(self->notice_source_id);
  }

  self->notice_source_id = g_timeout_add_seconds(4, skm_hide_notice_cb, self);
}
