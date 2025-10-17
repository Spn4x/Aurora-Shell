# widgets/launcher/meson.build

launcher_sources = [
  'launcher.c',
  'modules/apps.c',
  'modules/calculator.c',
  'modules/commands.c',
]

launcher_deps = [
  dependency('gtk4'),
  dependency('gtk4-layer-shell-0'),
]

shared_library('launcher',
  launcher_sources,
  dependencies: launcher_deps,
  name_prefix: '',
  # THE FIX IS HERE:
  install: true,
  install_dir: get_option('libdir') / 'aurora-shell' / 'widgets'
)