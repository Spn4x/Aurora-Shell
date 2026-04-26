qt6 = import('qt6')

# Dependencies
qt_deps = dependency('qt6', modules: ['Core', 'Gui', 'Qml', 'Quick', 'DBus'])
gio_dep = dependency('gio-2.0')

# LayerShell discovery
layershell_dep = dependency('LayerShellQt', method: 'cmake', modules: ['LayerShellQt::Interface'], required: false)
if not layershell_dep.found()
  cpp = meson.get_compiler('cpp')
  ls_lib = cpp.find_library('LayerShellQt', required: true)
  layershell_dep = declare_dependency(dependencies: ls_lib)
endif

processed_files = qt6.preprocess(
  moc_headers: 'process/LauncherBackend.h',
  qresources: 'resources.qrc'
)

src_files = [
  'process/main.cpp',
  'process/LauncherBackend.cpp',
  processed_files
]

executable('aurora-foresight',
  src_files,
  dependencies: [qt_deps, layershell_dep, gio_dep],
  install: true,
  install_dir: get_option('bindir')
)