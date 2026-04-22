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
  'src/ui-settings.c',
  'src/service.c',
  'src/service-system.c',
  'src/service-fan.c',
  'src/service-led.c',
  'src/service-gpu.c',
  'src/settings.c',
  'src/remote.c',
  'src/remote-util.c',
  'src/sysfs.c',
)

executable(
  'strawberry-kernel-manager',
  sources,
  include_directories: inc,
  dependencies: [gtk_dep, gio_dep, glib_dep, m_dep],
  install: true,
)

# ── Unit tests ─────────────────────────────────────────────────────────
# Pure-helper coverage. Each test binary links only the translation units
# it actually exercises (no GTK, no full REST server), so `meson test` is
# cheap enough to run on every commit without booting the daemon.
test_remote_util = executable(
  'test-remote-util',
  ['tests/test_remote_util.c', 'src/remote-util.c'],
  include_directories: include_directories('src'),
  dependencies: [glib_dep],
  build_by_default: false,
  install: false,
)
test('remote-util', test_remote_util)

install_data(
  'data/theme.css',
  'data/strawberry-logo.svg',
  install_dir: join_paths(get_option('datadir'), meson.project_name()),
)

# systemd unit for the headless remote daemon. ExecStart is resolved
# against the install prefix's bindir so distro packagers don't have to
# patch the template.
systemd_unit_dir = get_option('systemdsystemunitdir')
if systemd_unit_dir == ''
  systemd_dep = dependency('systemd', required: false)
  if systemd_dep.found()
    systemd_unit_dir = systemd_dep.get_variable(pkgconfig: 'systemdsystemunitdir')
  else
    systemd_unit_dir = join_paths(get_option('prefix'), 'lib/systemd/system')
  endif
endif

service_conf = configuration_data()
service_conf.set('bindir', join_paths(get_option('prefix'), get_option('bindir')))

configure_file(
  input: 'data/strawberry-manager.service.in',
  output: 'strawberry-manager.service',
  configuration: service_conf,
  install: true,
  install_dir: systemd_unit_dir,
)
