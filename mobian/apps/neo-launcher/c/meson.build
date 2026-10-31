project('neo-launcher', 'c', version: '2.0.0', default_options: ['warning_level=2', 'c_std=gnu11'])

gnome = import('gnome')
gtk = dependency('gtk+-3.0', version: '>= 3.24')
deps = [
  gtk,
  dependency('gtk+-wayland-3.0'),
  dependency('gtk-layer-shell-0'),
  dependency('libhandy-1'),
  dependency('json-glib-1.0'),
  dependency('gio-unix-2.0'),
  dependency('wayland-client'),
  meson.get_compiler('c').find_library('m'),
]
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')

# Wayland protocols: phoc's layer-shell effects, and the wlr layer shell (and xdg-shell) whose interfaces they name
scanner = find_program('wayland-scanner')
wp_dir = dependency('wayland-protocols').get_variable(pkgconfig: 'pkgdatadir')
protocols = [
  'protocols/phoc-layer-shell-effects-unstable-v1.xml',
  'protocols/wlr-layer-shell-unstable-v1.xml',
  wp_dir / 'stable/xdg-shell/xdg-shell.xml',
  wp_dir / 'stable/viewporter/viewporter.xml',
  wp_dir / 'staging/alpha-modifier/alpha-modifier-v1.xml',
  wp_dir / 'staging/single-pixel-buffer/single-pixel-buffer-v1.xml',
]
proto_src = []
foreach p : protocols
  name = p.split('/')[-1].replace('.xml', '')
  proto_src += custom_target(name + '-code', input: p, output: name + '-protocol.c',
                             command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  proto_src += custom_target(name + '-header', input: p, output: name + '-client-protocol.h',
                             command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
endforeach

resources = gnome.compile_resources('neo-resources', 'src/neo-launcher.gresource.xml', source_dir: 'src')

executable('neo-launcher',
  'src/main.c', 'src/neo-apps.c', 'src/neo-home.c', 'src/neo-icons.c', 'src/neo-launch.c', 'src/neo-model.c',
  'src/neo-parallax.c', 'src/neo-recents.c', 'src/neo-edges.c', 'src/neo-settings.c', 'src/neo-surf.c', 'src/neo-widgets.c', 'src/neo-wall.c',
  'src/neo-wayland.c',
  proto_src, resources,
  dependencies: deps,
  install: true)
