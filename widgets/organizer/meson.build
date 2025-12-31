organizer_sources = [
  'src/organizer.c',
  'src/calendar_widget.c',
  'src/eds_event_source.c',
  'src/mpris_widget.c',
]

organizer_deps = [
  dependency('gtk4'),
  dependency('gtk4-layer-shell-0'),
  dependency('gio-2.0'),
  dependency('json-glib-1.0'),
  dependency('libedataserver-1.2'),
  dependency('libecal-2.0'),
  dependency('libical'),
  dependency('libadwaita-1'),
]

shared_library('organizer',
  organizer_sources,
  dependencies: organizer_deps,
  name_prefix: '',
  install: true,
  install_dir: get_option('libdir') / 'aurora-shell' / 'widgets'
)