use nmrs::{NetworkManager, WifiSecurity, DeviceType};
use std::ffi::{c_void, CStr, CString};
use std::os::raw::{c_char, c_uchar};
use std::thread;
use tokio::runtime::Builder;
use zbus::{Connection, Result as ZResult};
use std::collections::{HashSet, HashMap};
use zbus::zvariant::{Value, OwnedValue};

// --- C-Compatible Structures ---
#[repr(C)]
pub struct RsWifiNetwork {
    pub ssid: *mut c_char,
    pub object_path: *mut c_char,
    pub strength: c_uchar,
    pub is_secure: bool,
    pub is_active: bool,
    pub is_known: bool,
    pub connectivity_state: u32,
}

#[repr(C)]
pub struct RsContext {
    dummy: usize,
}

// --- Helpers ---
fn string_to_c_ptr(s: String) -> *mut c_char {
    CString::new(s).unwrap_or_default().into_raw()
}

fn c_str_to_string(ptr: *const c_char) -> String {
    if ptr.is_null() { return String::new(); }
    unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() }
}

fn run_async_op<F, Fut>(
    callback: extern "C" fn(bool, *mut c_void),
    user_data: *mut c_void,
    op_factory: F,
) where
    F: FnOnce() -> Fut + Send + 'static,
    Fut: std::future::Future<Output = bool> + 'static,
{
    let cb_addr = callback as usize;
    let ud_addr = user_data as usize;

    thread::spawn(move || {
        let rt = Builder::new_current_thread().enable_all().build().unwrap();
        let future = op_factory();
        let success = rt.block_on(future);
        let cb_ptr: extern "C" fn(bool, *mut c_void) = unsafe { std::mem::transmute(cb_addr) };
        let ud_ptr = ud_addr as *mut c_void;
        cb_ptr(success, ud_ptr);
    });
}

// --- RAW D-BUS LOGIC ---

async fn get_saved_ssids() -> HashSet<String> {
    let mut saved = HashSet::new();
    let conn = match Connection::system().await { Ok(c) => c, Err(_) => return saved };
    let proxy = zbus::Proxy::new(&conn, "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager/Settings", "org.freedesktop.NetworkManager.Settings").await;

    if let Ok(p) = proxy {
        // FIX: Let Rust infer inputs, define return type explicitly variable-side
        let paths_res: ZResult<Vec<zbus::zvariant::OwnedObjectPath>> = p.call("ListConnections", &()).await;
        
        if let Ok(paths) = paths_res {
            for path in paths {
                let conn_proxy = zbus::Proxy::new(&conn, "org.freedesktop.NetworkManager", path.as_str(), "org.freedesktop.NetworkManager.Settings.Connection").await;
                if let Ok(cp) = conn_proxy {
                    type SettingsMap = HashMap<String, HashMap<String, OwnedValue>>;
                    
                    // FIX: Define return type explicitly here
                    let settings_res: ZResult<SettingsMap> = cp.call("GetSettings", &()).await;
                    
                    if let Ok(settings) = settings_res {
                        if let Some(wifi) = settings.get("802-11-wireless") {
                            if let Some(ssid_val) = wifi.get("ssid") {
                                if let Ok(ssid_bytes) = Vec::<u8>::try_from(ssid_val.clone()) {
                                    if let Ok(ssid_str) = String::from_utf8(ssid_bytes) {
                                        saved.insert(ssid_str);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    saved
}

// Manually find and DELETE a connection by SSID
async fn raw_forget_connection(ssid: &str) -> bool {
    println!("🦀 [RUST] Attempting to forget network: '{}'", ssid);
    let conn = match Connection::system().await { Ok(c) => c, Err(_) => return false };
    let proxy = zbus::Proxy::new(&conn, "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager/Settings", "org.freedesktop.NetworkManager.Settings").await;

    if let Ok(p) = proxy {
        let paths_res: ZResult<Vec<zbus::zvariant::OwnedObjectPath>> = p.call("ListConnections", &()).await;
        if let Ok(paths) = paths_res {
            for path in paths {
                let conn_proxy = zbus::Proxy::new(&conn, "org.freedesktop.NetworkManager", path.as_str(), "org.freedesktop.NetworkManager.Settings.Connection").await;
                if let Ok(cp) = conn_proxy {
                    type SettingsMap = HashMap<String, HashMap<String, OwnedValue>>;
                    let settings_res: ZResult<SettingsMap> = cp.call("GetSettings", &()).await;

                    if let Ok(settings) = settings_res {
                        if let Some(wifi) = settings.get("802-11-wireless") {
                            if let Some(ssid_val) = wifi.get("ssid") {
                                if let Ok(ssid_bytes) = Vec::<u8>::try_from(ssid_val.clone()) {
                                    if let Ok(current_ssid) = String::from_utf8(ssid_bytes) {
                                        if current_ssid == ssid {
                                            println!("🦀 [RUST] Found profile at {}. Deleting...", path.as_str());
                                            
                                            // FIX: Explicitly type return as empty tuple () to match void
                                            let delete_res: ZResult<()> = cp.call("Delete", &()).await;
                                            
                                            match delete_res {
                                                Ok(_) => {
                                                    println!("🦀 [RUST] Profile deleted successfully.");
                                                    return true;
                                                },
                                                Err(e) => {
                                                    println!("🦀 [RUST] Failed to delete profile: {}", e);
                                                    return false;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    println!("🦀 [RUST] Connection profile not found.");
    false
}

async fn raw_add_and_activate(nm: &NetworkManager, ssid: &str) -> bool {
    println!("🦀 [RUST] Raw AddAndActivate for '{}'...", ssid);
    let devices = match nm.list_devices().await { Ok(d) => d, Err(_) => return false };
    let wifi_dev = devices.into_iter().find(|d| d.device_type == DeviceType::Wifi);
    let device_path = match wifi_dev {
        Some(d) => lookup_device_path(&d.interface).await.unwrap_or("/".to_string()),
        None => return false,
    };

    let mut connection_settings: HashMap<&str, HashMap<&str, Value>> = HashMap::new();
    let mut s_con = HashMap::new();
    s_con.insert("id", Value::from(ssid));
    s_con.insert("type", Value::from("802-11-wireless"));
    connection_settings.insert("connection", s_con);

    let mut s_wifi = HashMap::new();
    s_wifi.insert("ssid", Value::from(ssid.as_bytes()));
    s_wifi.insert("mode", Value::from("infrastructure"));
    connection_settings.insert("802-11-wireless", s_wifi);

    let mut s_sec = HashMap::new();
    s_sec.insert("key-mgmt", Value::from("wpa-psk"));
    s_sec.insert("auth-alg", Value::from("open"));
    connection_settings.insert("802-11-wireless-security", s_sec);

    let conn = match Connection::system().await { Ok(c) => c, Err(_) => return false };
    let proxy = zbus::Proxy::new(&conn, "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager").await;

    if let Ok(p) = proxy {
        let dev_obj = zbus::zvariant::ObjectPath::try_from(device_path).ok();
        let ap_obj = zbus::zvariant::ObjectPath::try_from("/").ok();
        if let (Some(d), Some(ap)) = (dev_obj, ap_obj) {
             let res: ZResult<(zbus::zvariant::OwnedObjectPath, zbus::zvariant::OwnedObjectPath)> = 
                p.call("AddAndActivateConnection", &(connection_settings, d, ap)).await;
             return res.is_ok();
        }
    }
    false
}

async fn lookup_device_path(iface: &str) -> Option<String> {
    let conn = Connection::system().await.ok()?;
    let proxy = zbus::Proxy::new(&conn, "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager").await.ok()?;
    let path: ZResult<zbus::zvariant::OwnedObjectPath> = proxy.call("GetDeviceByIpIface", &(iface)).await;
    path.ok().map(|p| p.as_str().to_string())
}

// --- FFI Exports ---

#[no_mangle]
pub extern "C" fn nm_rust_init() -> *mut RsContext {
    println!("\n🦀 [RUST] Backend Initialized (Full Control Mode)\n");
    Box::into_raw(Box::new(RsContext { dummy: 0 }))
}

#[no_mangle]
pub extern "C" fn nm_rust_free(ctx: *mut RsContext) {
    if !ctx.is_null() { unsafe { let _ = Box::from_raw(ctx); } }
}

#[no_mangle]
pub extern "C" fn nm_rust_is_wifi_enabled(_ctx: *mut RsContext) -> bool {
    let rt = Builder::new_current_thread().enable_all().build().unwrap();
    rt.block_on(async {
        let nm = NetworkManager::new().await.ok()?;
        nm.wifi_enabled().await.ok()
    }).unwrap_or(false)
}

#[no_mangle]
pub extern "C" fn nm_rust_set_wifi_enabled(_ctx: *mut RsContext, enabled: bool) {
    thread::spawn(move || {
        let rt = Builder::new_current_thread().enable_all().build().unwrap();
        rt.block_on(async {
            if let Ok(nm) = NetworkManager::new().await {
                let _ = nm.set_wifi_enabled(enabled).await;
            }
        });
    });
}

#[no_mangle]
pub extern "C" fn nm_rust_get_networks(_ctx: *mut RsContext, count_out: *mut usize) -> *mut RsWifiNetwork {
    let rt = Builder::new_current_thread().enable_all().build().unwrap();
    
    let (networks, current_ssid, saved_ssids) = rt.block_on(async {
        let nm = match NetworkManager::new().await {
            Ok(n) => n,
            Err(_) => return (vec![], None, HashSet::new()),
        };
        let nets = nm.list_networks().await.unwrap_or_default();
        let curr = nm.current_ssid().await;
        let saved = get_saved_ssids().await;
        (nets, curr, saved)
    });

    let mut results = networks.into_iter().map(|net| {
        let is_active = match &current_ssid {
            Some(active_ssid) => active_ssid == &net.ssid,
            None => false,
        };
        let is_known = saved_ssids.contains(&net.ssid);
        let is_secure = true; 

        RsWifiNetwork {
            ssid: string_to_c_ptr(net.ssid),
            object_path: string_to_c_ptr(net.bssid.unwrap_or_default()),
            strength: net.strength.unwrap_or(0) as c_uchar,
            is_secure,
            is_active,
            is_known,
            connectivity_state: if is_active { 4 } else { 0 },
        }
    }).collect::<Vec<_>>();

    results.sort_by(|a, b| {
        if a.is_active != b.is_active { return b.is_active.cmp(&a.is_active); }
        if a.is_known != b.is_known { return b.is_known.cmp(&a.is_known); }
        b.strength.cmp(&a.strength)
    });

    unsafe { *count_out = results.len(); }
    let slice = results.into_boxed_slice();
    Box::into_raw(slice) as *mut RsWifiNetwork
}

#[no_mangle]
pub extern "C" fn nm_rust_free_networks(ptr: *mut RsWifiNetwork, count: usize) {
    if ptr.is_null() { return; }
    unsafe {
        let slice = std::slice::from_raw_parts_mut(ptr, count);
        for item in slice.iter() {
            let _ = CString::from_raw(item.ssid);
            let _ = CString::from_raw(item.object_path);
        }
        let _ = Box::from_raw(slice);
    }
}

// --- Actions ---

#[no_mangle]
pub extern "C" fn nm_rust_disconnect(_ctx: *mut RsContext, callback: extern "C" fn(bool, *mut c_void), user_data: *mut c_void) {
    run_async_op(callback, user_data, || async { false });
}

#[no_mangle]
pub extern "C" fn nm_rust_add_and_activate(
    _ctx: *mut RsContext,
    ssid_c: *const c_char,
    _ap_path_c: *const c_char,
    password_c: *const c_char,
    is_secure: bool,
    callback: extern "C" fn(bool, *mut c_void),
    user_data: *mut c_void,
) {
    let ssid = c_str_to_string(ssid_c);
    let password = c_str_to_string(password_c);

    run_async_op(callback, user_data, move || async move {
        if is_secure && password.is_empty() {
            if let Ok(nm) = NetworkManager::new().await {
                return raw_add_and_activate(&nm, &ssid).await;
            }
            return false;
        }
        let security = if !is_secure { WifiSecurity::Open } else { WifiSecurity::WpaPsk { psk: password } };
        if let Ok(nm) = NetworkManager::new().await {
            match nm.connect(&ssid, security).await {
                Ok(_) => true,
                Err(e) => { println!("🦀 Connect Error: {}", e); false }
            }
        } else { false }
    });
}

#[no_mangle]
pub extern "C" fn nm_rust_forget_connection(_ctx: *mut RsContext, ssid_c: *const c_char, callback: extern "C" fn(bool, *mut c_void), user_data: *mut c_void) {
    let ssid = c_str_to_string(ssid_c);
    run_async_op(callback, user_data, move || async move {
        raw_forget_connection(&ssid).await
    });
}

#[no_mangle]
pub extern "C" fn nm_rust_activate_existing(
    _ctx: *mut RsContext,
    _conn_path_c: *const c_char,
    ap_path_c: *const c_char,
    password_c: *const c_char,
    is_secure: bool,
    callback: extern "C" fn(bool, *mut c_void),
    user_data: *mut c_void,
) {
    let bssid = c_str_to_string(ap_path_c);
    let password = c_str_to_string(password_c);
    run_async_op(callback, user_data, move || async move {
        if let Ok(nm) = NetworkManager::new().await {
            if let Ok(networks) = nm.list_networks().await {
                if let Some(net) = networks.iter().find(|n| n.bssid.as_deref() == Some(&bssid)) {
                    let security = if is_secure { WifiSecurity::WpaPsk { psk: password } } else { WifiSecurity::Open };
                    return nm.connect(&net.ssid, security).await.is_ok();
                }
            }
        }
        false
    });
}

#[no_mangle]
pub extern "C" fn nm_rust_find_connection_path(_ctx: *mut RsContext, _ssid_c: *const c_char) -> *mut c_char {
    std::ptr::null_mut()
}

#[no_mangle]
pub extern "C" fn nm_rust_free_string(ptr: *mut c_char) {
    if !ptr.is_null() { unsafe { let _ = CString::from_raw(ptr); } }
}