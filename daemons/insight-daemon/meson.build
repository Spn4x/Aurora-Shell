# This file should start directly with the dependencies and target
sqlite_dep = dependency('sqlite3')

executable('aurora-insight-daemon',
  'insight-daemon.c',
  dependencies : [sqlite_dep],
  install : true
)