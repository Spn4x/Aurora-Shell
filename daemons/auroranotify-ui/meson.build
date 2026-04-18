gtk_dep = dependency('gtk4')
layershell_dep = dependency('gtk4-layer-shell-0')
gio_dep = dependency('gio-2.0')
json_dep = dependency('json-glib-1.0') # <-- NEW

aurora_datadir = get_option('datadir') / 'aurora-shell'
datadir_c_arg = '-DDATADIR="' + aurora_datadir + '"'

executable('auroranotify-ui',
  'auroranotify-ui.c',
  'island_widget.c',
  'privacy_widget.c',
  c_args: [datadir_c_arg],
  dependencies: [
    gtk_dep,
    layershell_dep,
    gio_dep,
    json_dep # <-- NEW
  ],
  install: true
)