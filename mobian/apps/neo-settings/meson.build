project('neo-settings', 'c', version: '0.2', default_options: ['warning_level=2', 'c_std=gnu11'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')
add_project_arguments('-DNS_VERSION="@0@"'.format(meson.project_version()), language: 'c')

gnome = import('gnome')
cc = meson.get_compiler('c')
deps = [dependency('gtk4', version: '>= 4.14'), dependency('libadwaita-1', version: '>= 1.5'),
        dependency('json-glib-1.0'), dependency('goa-backend-1.0'), dependency('gio-unix-2.0'), dependency('gmodule-2.0'),
        dependency('libnm'), dependency('libdrm'), dependency('libpulse-mainloop-glib'),
        # the pattern dialog checks the current password the way the lock screen does
        cc.find_library('pam'), cc.find_library('m', required: false)]
resources = gnome.compile_resources('ns-resources', 'src/neo-settings.gresource.xml', source_dir: 'src')

executable('neo-settings',
  'src/main.c', 'src/ns-ui.c', 'src/ns-pages.c', 'src/ns-parallax.c', 'src/ns-net.c', 'src/ns-bt.c',
  'src/ns-sound.c', 'src/ns-apps.c', 'src/ns-shell.c', 'src/ns-pattern.c', 'src/ns-privacy.c', 'src/ns-system.c',
  resources,
  dependencies: deps,
  install: true)
install_data('data/de.yesman.NeoSettings.desktop', install_dir: get_option('datadir') / 'applications')
# D-Bus activation: Phosh's quick settings open a page through "launch-panel" without a running app
service = configuration_data()
service.set('bindir', get_option('prefix') / get_option('bindir'))
configure_file(input: 'data/de.yesman.NeoSettings.service.in', output: 'de.yesman.NeoSettings.service',
               configuration: service, install_dir: get_option('datadir') / 'dbus-1' / 'services')
