mod calc;
mod app;

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_uint};

#[repr(C)]
pub struct AuroraSearchResult {
    pub title: *mut c_char,
    pub description: *mut c_char,
    pub icon: *mut c_char,
    pub result_type: c_uint, // 0=App, 1=Calc, 2=Cmd
    pub score: c_int,
    pub exec_data: *mut c_char,
}

fn to_c(s: &str) -> *mut c_char {
    CString::new(s).unwrap_or_default().into_raw()
}

#[no_mangle]
pub extern "C" fn aurora_backend_query(
    query_ptr: *const c_char, 
    count_out: *mut usize
) -> *mut AuroraSearchResult {
    
    let query = unsafe {
        if query_ptr.is_null() { return std::ptr::null_mut(); }
        CStr::from_ptr(query_ptr).to_string_lossy()
    };

    let mut results = Vec::new();

    // 1. Calculator
    if let Some(calc) = calc::search(&query) {
        results.push(AuroraSearchResult {
            title: to_c(&calc.value),
            description: to_c(&format!("Result: {}", calc.value)),
            icon: to_c("accessories-calculator-symbolic"),
            result_type: 1, 
            score: 120,
            exec_data: to_c(&calc.value),
        });
    }

    // 2. Command
    if let Some(cmd) = query.strip_prefix("> ") {
        if !cmd.trim().is_empty() {
            results.push(AuroraSearchResult {
                title: to_c(cmd.trim()),
                description: to_c("Run command"),
                icon: to_c("utilities-terminal-symbolic"),
                result_type: 2, 
                score: 110,
                exec_data: to_c(cmd.trim()),
            });
        }
    }

    // 3. Apps
    if !query.starts_with("> ") {
        let mut app_hits = app::search(&query);
        for app in app_hits.drain(..std::cmp::min(app_hits.len(), 15)) {
            results.push(AuroraSearchResult {
                title: to_c(&app.name),
                description: to_c(&app.description),
                icon: to_c(&app.icon),
                result_type: 0,
                score: app.score as c_int,
                exec_data: to_c(&app.id),
            });
        }
    }

    *unsafe { &mut *count_out } = results.len();

    if results.is_empty() {
        return std::ptr::null_mut();
    }

    // MEMORY SAFETY FIX:
    // Convert Vec to Box<[T]>, then leak to raw pointer.
    // This returns a pointer to the first element, valid for C access.
    let slice = results.into_boxed_slice();
    Box::into_raw(slice) as *mut AuroraSearchResult
}

#[no_mangle]
pub extern "C" fn aurora_backend_free_results(ptr: *mut AuroraSearchResult, len: usize) {
    if ptr.is_null() || len == 0 { return; }
    unsafe {
        // Reconstruct the slice from raw parts.
        // We use from_raw_parts_mut to get a mutable slice to the data.
        let slice = std::slice::from_raw_parts_mut(ptr, len);
        
        // 1. Free inner CStrings manually
        for item in slice.iter() {
            if !item.title.is_null() { let _ = CString::from_raw(item.title); }
            if !item.description.is_null() { let _ = CString::from_raw(item.description); }
            if !item.icon.is_null() { let _ = CString::from_raw(item.icon); }
            if !item.exec_data.is_null() { let _ = CString::from_raw(item.exec_data); }
        }
        
        // 2. Free the array allocation using Box::from_raw
        // Note: We cast back to *mut [AuroraSearchResult] which Box expects for slices.
        let _ = Box::from_raw(slice); 
    }
}