project('neo-weather', 'c', version: '0.1', default_options: ['warning_level=2', 'c_std=gnu11'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')

gnome = import('gnome')
deps = [dependency('gtk4', version: '>= 4.12'), dependency('libadwaita-1', version: '>= 1.5'),
        dependency('json-glib-1.0'), meson.get_compiler('c').find_library('m')]
resources = gnome.compile_resources('wx-resources', 'src/neo-weather.gresource.xml', source_dir: 'src')

executable('neo-weather', 'src/main.c', resources, dependencies: deps, install: true)
install_data('data/de.yesman.NeoWeather.desktop', install_dir: get_option('datadir') / 'applications')
