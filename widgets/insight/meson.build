# This file should start directly with the dependencies and target
gtk_dep = dependency('gtk4')
layershell_dep = dependency('gtk4-layer-shell-0')
sqlite_dep = dependency('sqlite3')
math_dep = cc.find_library('m', required : true)

shared_library('aurora-insight',
  'insight.c',
  dependencies : [
    gtk_dep,
    layershell_dep,
    sqlite_dep,
    math_dep
  ],
  name_prefix : '',
  install : true,
  install_dir : get_option('libdir') / 'aurora-shell/widgets'
)