# widgets/uptime/meson.build

# This will install to /usr/local/lib/aurora-shell/widgets/
widget_install_dir = get_option('libdir') / 'aurora-shell' / 'widgets'

dependencies = [
  dependency('gtk4'),
  dependency('json-glib-1.0')
]

shared_library('uptime', 'uptime.c',
  dependencies: dependencies,
  install: true,
  install_dir: widget_install_dir,
  name_prefix: '', 
)