launcher_sources = [
  'launcher.c',
]

launcher_deps = [
  dependency('gtk4'),
  dependency('gtk4-layer-shell-0'),
  dependency('gio-2.0'),
]

shared_library('launcher',
  launcher_sources,
  dependencies: launcher_deps,
  name_prefix: '',
  install: true,
  install_dir: get_option('libdir') / 'aurora-shell' / 'widgets'
)