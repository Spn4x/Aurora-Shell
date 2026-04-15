use std::fs;
use std::error::Error;
use zbus::{ConnectionBuilder, dbus_interface};
use tokio::time::{sleep, Duration};

struct SystemStats {
    cpu_usage: f64,
    ram_usage: f64,
    temp_c: f64,
    battery_percent: f64,
}

#[dbus_interface(name = "com.meismeric.aurora.Stats")]
impl SystemStats {
    #[dbus_interface(property)]
    fn cpu_usage(&self) -> f64 { self.cpu_usage }

    #[dbus_interface(property)]
    fn ram_usage(&self) -> f64 { self.ram_usage }

    #[dbus_interface(property)]
    fn temp_c(&self) -> f64 { self.temp_c }

    #[dbus_interface(property)]
    fn battery_percent(&self) -> f64 { self.battery_percent }
}

fn get_cpu_usage(prev_total: &mut u64, prev_idle: &mut u64) -> f64 {
    if let Ok(content) = fs::read_to_string("/proc/stat") {
        if let Some(line) = content.lines().next() {
            let parts: Vec<&str> = line.split_whitespace().collect();
            if parts.len() > 4 {
                let user: u64 = parts[1].parse().unwrap_or(0);
                let nice: u64 = parts[2].parse().unwrap_or(0);
                let system: u64 = parts[3].parse().unwrap_or(0);
                let idle: u64 = parts[4].parse().unwrap_or(0);
                let iowait: u64 = parts[5].parse().unwrap_or(0);
                let irq: u64 = parts[6].parse().unwrap_or(0);
                let softirq: u64 = parts[7].parse().unwrap_or(0);
                let steal: u64 = parts[8].parse().unwrap_or(0);

                let current_idle = idle + iowait;
                let current_total = user + nice + system + current_idle + irq + softirq + steal;

                let total_diff = current_total.saturating_sub(*prev_total);
                let idle_diff = current_idle.saturating_sub(*prev_idle);

                *prev_total = current_total;
                *prev_idle = current_idle;

                if total_diff > 0 {
                    return (total_diff - idle_diff) as f64 / total_diff as f64;
                }
            }
        }
    }
    0.0
}

fn get_ram_usage() -> f64 {
    let mut total = 0.0;
    let mut avail = 0.0;
    if let Ok(content) = fs::read_to_string("/proc/meminfo") {
        for line in content.lines() {
            if line.starts_with("MemTotal:") {
                total = line.split_whitespace().nth(1).unwrap_or("0").parse().unwrap_or(0.0);
            } else if line.starts_with("MemAvailable:") {
                avail = line.split_whitespace().nth(1).unwrap_or("0").parse().unwrap_or(0.0);
            }
            if total > 0.0 && avail > 0.0 { break; }
        }
    }
    if total > 0.0 { (total - avail) / total } else { 0.0 }
}

fn get_temp() -> f64 {
    if let Ok(entries) = fs::read_dir("/sys/class/hwmon") {
        for entry in entries.flatten() {
            let path = entry.path().join("temp1_input");
            if path.exists() {
                if let Ok(c) = fs::read_to_string(path) {
                    let temp_m: f64 = c.trim().parse().unwrap_or(0.0);
                    return temp_m / 1000.0;
                }
            }
        }
    }
    0.0
}

fn get_battery() -> f64 {
    if let Ok(entries) = fs::read_dir("/sys/class/power_supply") {
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().to_string();
            if name.starts_with("BAT") {
                let cap_path = entry.path().join("capacity");
                if let Ok(cap_str) = fs::read_to_string(cap_path) {
                    return cap_str.trim().parse().unwrap_or(100.0);
                }
            }
        }
    }
    100.0
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error>> {
    let provider = SystemStats {
        cpu_usage: 0.0, ram_usage: 0.0, temp_c: 0.0, battery_percent: 100.0,
    };

    let conn = ConnectionBuilder::session()?
        .name("com.meismeric.aurora.Stats")?
        .serve_at("/com/meismeric/aurora/Stats", provider)?
        .build()
        .await?;

    println!("aurorawidgetd: System Stats D-Bus provider running.");

    let mut prev_total = 0;
    let mut prev_idle = 0;

    // --- THE FIX: GRACEFUL SHUTDOWN ---
    let mut sigterm = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())?;

    loop {
        tokio::select! {
            _ = sleep(Duration::from_secs(2)) => {
                let new_cpu = get_cpu_usage(&mut prev_total, &mut prev_idle);
                let new_ram = get_ram_usage();
                let new_temp = get_temp();
                let new_bat = get_battery();

                // Push updates to D-Bus
                let object_server = conn.object_server();
                let iface_ref = object_server.interface::<_, SystemStats>("/com/meismeric/aurora/Stats").await?;
                
                let mut iface = iface_ref.get_mut().await;
                let mut changed = false;

                if (iface.cpu_usage - new_cpu).abs() > 0.001 { iface.cpu_usage = new_cpu; changed = true; }
                if (iface.ram_usage - new_ram).abs() > 0.001 { iface.ram_usage = new_ram; changed = true; }
                if (iface.temp_c - new_temp).abs() > 0.1 { iface.temp_c = new_temp; changed = true; }
                if (iface.battery_percent - new_bat).abs() > 0.1 { iface.battery_percent = new_bat; changed = true; }

                if changed {
                    // Emits the org.freedesktop.DBus.Properties.PropertiesChanged signal for the UI
                    iface.cpu_usage_changed(iface_ref.signal_context()).await?;
                    iface.ram_usage_changed(iface_ref.signal_context()).await?;
                    iface.temp_c_changed(iface_ref.signal_context()).await?;
                    iface.battery_percent_changed(iface_ref.signal_context()).await?;
                }
            },
            _ = tokio::signal::ctrl_c() => {
                println!("aurorawidgetd: Received SIGINT. Shutting down...");
                break;
            },
            _ = sigterm.recv() => {
                println!("aurorawidgetd: Received SIGTERM. Shutting down...");
                break;
            }
        }
    }

    Ok(())
}