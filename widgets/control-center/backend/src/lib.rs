// widgets/control-center/backend/src/lib.rs

// Expose the network library module.
// The #[no_mangle] extern "C" functions inside it will be exported 
// by the static library automatically.
pub mod netlib;