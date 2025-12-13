# daemons/auroranotifyd/meson.build

# Define dependencies
# We need SQLite for the history feature, plus GIO/GLib for D-Bus
sqlite_dep = dependency('sqlite3')
gio_dep = dependency('gio-2.0')
glib_dep = dependency('glib-2.0')

# Define the executable
executable('auroranotifyd',
           'auroranotifyd.c',
           dependencies: [gio_dep, glib_dep, sqlite_dep],
           install: true
)