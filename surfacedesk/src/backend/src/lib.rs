use std::ffi::{CStr, CString};
use std::os::raw::{c_char};
use std::ptr;

mod system;
mod media;
mod theme; 

// --- THEME ---
#[no_mangle]
pub extern "C" fn rust_generate_theme(path_ptr: *const c_char) -> bool {
    match theme::run_theme_process(path_ptr) {
        Ok(_) => true,
        Err(e) => {
            eprintln!("Rust Theme Error: {}", e);
            false
        }
    }
}

// --- SYSTEM ---
#[no_mangle]
pub extern "C" fn rust_get_system_stats(
    prev_total: u64, 
    prev_idle: u64, 
    out_stats: *mut system::SystemStats
) {
    if out_stats.is_null() { return; }
    let stats = system::fetch_stats(prev_total, prev_idle);
    unsafe {
        *out_stats = stats;
    }
}

// --- MEDIA ---
#[no_mangle]
pub extern "C" fn rust_media_get_cache_path(url: *const c_char) -> *mut c_char {
    if url.is_null() { return ptr::null_mut(); }
    let c_str = unsafe { CStr::from_ptr(url) };
    let url_str = c_str.to_str().unwrap_or("");
    
    match media::get_cached_art_path(url_str) {
        Some(path) => CString::new(path).unwrap().into_raw(),
        None => ptr::null_mut(),
    }
}

#[no_mangle]
pub extern "C" fn rust_media_download(url: *const c_char, dest: *const c_char) -> bool {
    if url.is_null() || dest.is_null() { return false; }
    let url_str = unsafe { CStr::from_ptr(url).to_str().unwrap_or("") };
    let dest_str = unsafe { CStr::from_ptr(dest).to_str().unwrap_or("") };
    
    media::download_art(url_str, dest_str)
}

#[no_mangle]
pub extern "C" fn rust_free_string(s: *mut c_char) {
    if s.is_null() { return; }
    unsafe { let _ = CString::from_raw(s); }
}