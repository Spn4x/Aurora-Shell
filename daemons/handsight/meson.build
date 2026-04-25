# daemons/handsight/meson.build

qt6 = import('qt6')

# Standard Qt dependencies
qt_deps = dependency('qt6', modules: ['Core', 'Gui', 'Qml', 'Quick', 'DBus'])

# Get the C++ compiler to perform manual lookups if necessary
cpp = meson.get_compiler('cpp')

# Strategy 1: Try standard pkg-config or CMake discovery
layershell_dep = dependency('LayerShellQt', required: false)

if not layershell_dep.found()
  # Strategy 2: Try the specific CMake Interface target found in your standalone test
  layershell_dep = dependency('LayerShellQt', method: 'cmake', modules: ['LayerShellQt::Interface'], required: false)
endif

if not layershell_dep.found()
  # Strategy 3: Brute-force. Search for the library file 'libLayerShellQt.so' directly
  # This works on most systems even when CMake/Pkg-config metadata is broken.
  ls_lib = cpp.find_library('LayerShellQt', required: true)
  layershell_dep = declare_dependency(dependencies: ls_lib)
endif

# Process Qt Meta-Object Compiler (MOC) and Resources (QRC)
processed_files = qt6.preprocess(
  moc_headers: 'process/NotificationBackend.h',
  qresources: 'resources.qrc'
)

src_files = [
  'process/main.cpp',
  'process/NotificationBackend.cpp',
  processed_files
]

executable('aurora-handsight',
  src_files,
  dependencies: [qt_deps, layershell_dep],
  install: true,
  install_dir: get_option('bindir')
)