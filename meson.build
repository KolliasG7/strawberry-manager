project(
  'strawberry-kernel-manager',
  'c',
  version: '0.2.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
  ],
)

cc = meson.get_compiler('c')
gtk_dep = dependency('gtk4')
gio_dep = dependency('gio-2.0')
glib_dep = dependency('glib-2.0')
m_dep = cc.find_library('m', required: true)

inc = include_directories('include')

sources = files(
  'src/main.c',
  'src/ui.c',
  'src/ui-common.c',
  'src/ui-update.c',
  'src/ui-actions.c',
  'src/service.c',
  'src/service-system.c',
  'src/service-fan.c',
  'src/service-led.c',
  'src/service-gpu.c',
  'src/sysfs.c',
)

executable(
  'strawberry-kernel-manager',
  sources,
  include_directories: inc,
  dependencies: [gtk_dep, gio_dep, glib_dep, m_dep],
  install: true,
)

install_data(
  'data/theme.css',
  install_dir: join_paths(get_option('datadir'), meson.project_name()),
)
