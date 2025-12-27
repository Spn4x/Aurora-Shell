use std::ffi::CString;
use std::fs;
use std::os::raw::c_char;
use std::path::Path;

#[repr(C)]
pub struct UptimeData {
    pub os_name: *mut c_char,
    pub uptime_str: *mut c_char,
}

fn to_c(s: String) -> *mut c_char {
    CString::new(s).unwrap_or_default().into_raw()
}

fn get_os_name() -> String {
    let content = fs::read_to_string("/etc/os-release").unwrap_or_default();
    
    // Look for PRETTY_NAME first, then NAME
    let mut name = "Unknown OS".to_string();
    
    for line in content.lines() {
        if let Some((key, val)) = line.split_once('=') {
            if key == "PRETTY_NAME" || (key == "NAME" && name == "Unknown OS") {
                // Remove quotes if present
                name = val.trim().trim_matches('"').to_string();
                if key == "PRETTY_NAME" { break; }
            }
        }
    }
    name
}

fn get_uptime_string() -> String {
    let content = fs::read_to_string("/proc/uptime").unwrap_or_default();
    
    // /proc/uptime contains "total_seconds idle_seconds"
    // We only need the first number
    let seconds_total = content
        .split_whitespace()
        .next()
        .and_then(|s| s.parse::<f64>().ok())
        .unwrap_or(0.0) as u64;

    let days = seconds_total / 86400;
    let hours = (seconds_total % 86400) / 3600;
    let minutes = (seconds_total % 3600) / 60;

    let mut parts = Vec::new();
    if days > 0 { parts.push(format!("{} day{}", days, if days > 1 { "s" } else { "" })); }
    if hours > 0 { parts.push(format!("{} hour{}", hours, if hours > 1 { "s" } else { "" })); }
    if minutes > 0 { parts.push(format!("{} minute{}", minutes, if minutes > 1 { "s" } else { "" })); }

    if parts.is_empty() {
        "just now".to_string()
    } else {
        parts.join(", ")
    }
}

// --- FFI EXPORTS ---

#[no_mangle]
pub extern "C" fn aurora_uptime_get_data() -> *mut UptimeData {
    let data = UptimeData {
        os_name: to_c(get_os_name()),
        uptime_str: to_c(get_uptime_string()),
    };
    Box::into_raw(Box::new(data))
}

#[no_mangle]
pub extern "C" fn aurora_uptime_free_data(ptr: *mut UptimeData) {
    if ptr.is_null() { return; }
    unsafe {
        let data = Box::from_raw(ptr);
        let _ = CString::from_raw(data.os_name);
        let _ = CString::from_raw(data.uptime_str);
        // data dropped here
    }
}