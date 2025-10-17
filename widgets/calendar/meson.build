# widgets/calendar/meson.build

widget_install_dir = get_option('libdir') / 'aurora-shell' / 'widgets'
data_install_dir = get_option('datadir') / 'aurora-shell' / 'calendar'

dependencies = [
  dependency('gtk4'),
  dependency('json-glib-1.0'),
]

shared_library('calendar', 'calendar.c',
  dependencies: dependencies,
  install: true,
  install_dir: widget_install_dir,
  name_prefix: '',
)

install_data(
  ['data/events.json', 'data/permanent_events.json'],
  install_dir: data_install_dir
)