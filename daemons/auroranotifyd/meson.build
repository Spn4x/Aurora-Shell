# daemons/auroranotifyd/meson.build

# This daemon only needs GIO for D-Bus communication.
gio_dep = dependency('gio-2.0')

# Define the executable to be built and installed.
executable('auroranotifyd',
  'auroranotifyd.c',
  dependencies: [gio_dep],
  install: true
)