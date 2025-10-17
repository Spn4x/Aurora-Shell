
# project('aurora-widget-control-center', 'c',
#  default_options: ['c_std=c11']
# )

# All the source files from your control center project
control_center_sources = [
  'src/main.c',
  'src/audio_manager.c',
  'src/bluetooth_manager.c',
  'src/bluetooth_scanner.c',
  'src/brightness_manager.c',
  'src/network_manager.c',
  #'src/password_prompt.c',
  'src/system_monitor.c',
  'src/utils.c',
  'src/wifi_scanner.c',
]

# The dependencies your code ACTUALLY needs
control_center_deps = [
  dependency('gtk4'),
  dependency('libadwaita-1'),
  dependency('gio-2.0'),
  dependency('gio-unix-2.0'),
]

# Build a shared_module, not an executable
shared_module('control-center',
  control_center_sources,
  dependencies: control_center_deps,
  name_prefix: '',
  install: false
)