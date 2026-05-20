# daemons/insight-daemon/meson.build

# Define the dependencies required by the updated daemon
sqlite_dep = dependency('sqlite3')
glib_dep = dependency('glib-2.0')
gio_dep = dependency('gio-2.0')
json_dep = dependency('json-glib-1.0')

executable('aurora-insight-daemon',
  'insight-daemon.c',
  dependencies : [
    sqlite_dep,
    glib_dep,
    gio_dep,
    json_dep
  ],
  install : true
)