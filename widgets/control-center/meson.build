# widgets/control-center/meson.build

control_center_sources = [
  'src/main.c',
  'src/audio_manager.c',
  'src/bluetooth_manager.c',
  'src/bluetooth_scanner.c',
  'src/brightness_manager.c',
  'src/network_manager.c',
  'src/system_monitor.c',
  'src/utils.c',
  'src/wifi_scanner.c',
]

control_center_deps = [
  dependency('gtk4'),
  dependency('libadwaita-1'),
  dependency('gio-2.0'),
  dependency('gio-unix-2.0'),
]

# Note: Your config.json looks for 'control-center.so'.
# Building as shared_library with name_prefix is the most consistent way.
shared_library('control-center',
  control_center_sources,
  dependencies: control_center_deps,
  name_prefix: '',
  # THE FIX IS HERE:
  install: true,
  install_dir: get_option('libdir') / 'aurora-shell' / 'widgets'
)