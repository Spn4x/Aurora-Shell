# widgets/launcher/meson.build

cargo = find_program('cargo', required: true)
cc = meson.get_compiler('c')

# Define the Rust source files so Meson knows when to rebuild
rust_sources = files(
  'backend/Cargo.toml',
  'backend/src/lib.rs',
  'backend/src/app.rs',
  'backend/src/calc.rs'
)

# 1. Build the Rust backend
rust_backend = custom_target(
  'rust_backend',
  input: rust_sources,  # <--- THIS IS THE FIX
  output: 'libaurora_launcher_backend.a',
  command: [
    'sh', '-c',
    '@0@ build --manifest-path @1@/backend/Cargo.toml --release && cp @1@/backend/target/release/libaurora_launcher_backend.a @2@'
    .format(cargo.full_path(), meson.current_source_dir(), '@OUTPUT@')
  ],
  console: true,
  build_by_default: true
)

# 2. Wrap the static library in a dependency object
launcher_rust_dep = declare_dependency(
  link_args: meson.current_build_dir() / 'libaurora_launcher_backend.a',
  sources: rust_backend
)

launcher_sources = [
  'launcher.c',
]

launcher_deps = [
  dependency('gtk4'),
  dependency('gtk4-layer-shell-0'),
  dependency('gio-2.0'),
  cc.find_library('m', required: true),
  launcher_rust_dep,
]

shared_library('launcher',
  launcher_sources,
  dependencies: launcher_deps,
  name_prefix: '',
  install: true,
  install_dir: get_option('libdir') / 'aurora-shell' / 'widgets'
)