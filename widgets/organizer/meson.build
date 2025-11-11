# widgets/organizer/meson.build

# Source files are now inside the 'src' subdirectory.
organizer_sources = [
  'src/organizer.c',
  'src/calendar_widget.c',
]

organizer_deps = [
  dependency('gtk4'),
  dependency('gtk4-layer-shell-0'),
  dependency('gio-2.0'),
  dependency('json-glib-1.0'),
]

shared_library('organizer',
  organizer_sources,
  dependencies: organizer_deps,
  name_prefix: '',
  install: true,
  install_dir: get_option('libdir') / 'aurora-shell' / 'widgets'
)

# Install the data files for the calendar
install_data(
  'data/events.json',
  'data/permanent_events.json',
  install_dir: get_option('datadir') / 'aurora-shell' / 'calendar',
  install_mode: 'rw-r--r--'
)