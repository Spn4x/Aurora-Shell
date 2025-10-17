# aurora-shell/widgets/launcher/meson.build

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
  install: true,
  name_prefix: '', # This prevents 'lib' prefix, making it 'launcher.so'
)