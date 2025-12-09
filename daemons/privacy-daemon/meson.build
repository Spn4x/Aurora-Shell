# daemons/privacy-daemon/meson.build

executable('aurora-privacy-daemon',
  'privacy-daemon.c',
  dependencies: [
    dependency('gio-2.0'),
    dependency('libpipewire-0.3', version: '>= 0.3.38'),
    dependency('libspa-0.2'),
    # Use GLib's threading, which is handled by gio-2.0
  ],
  # Add the C flag to expose POSIX definitions
  c_args: '-D_GNU_SOURCE',
  install: true
)