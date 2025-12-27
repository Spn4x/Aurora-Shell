# widgets/uptime/meson.build

cargo = find_program('cargo', required: true)

rust_sources = files(
  'backend/Cargo.toml',
  'backend/src/lib.rs'
)

# 1. Build Rust
rust_backend = custom_target(
  'rust_uptime_backend',
  input: rust_sources,
  output: 'libaurora_uptime_backend.a',
  command: [
    'sh', '-c',
    '@0@ build --manifest-path @1@/backend/Cargo.toml --release && cp @1@/backend/target/release/libaurora_uptime_backend.a @2@'
    .format(cargo.full_path(), meson.current_source_dir(), '@OUTPUT@')
  ],
  console: true,
  build_by_default: true
)

# 2. Wrap as dependency
uptime_rust_dep = declare_dependency(
  link_args: meson.current_build_dir() / 'libaurora_uptime_backend.a',
  sources: rust_backend
)

# 3. Build Shared Library
shared_library('uptime', 
  'uptime.c',
  dependencies: [
    dependency('gtk4'),
    dependency('json-glib-1.0'),
    uptime_rust_dep
  ],
  install: true,
  install_dir: get_option('libdir') / 'aurora-shell' / 'widgets',
  name_prefix: '', 
)