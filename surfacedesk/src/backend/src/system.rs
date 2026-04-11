use std::fs;
use std::path::Path;

#[repr(C)]
pub struct SystemStats {
    pub cpu_usage: f64,
    pub ram_usage: f64,
    pub temp_c: f64,
    pub new_total: u64,
    pub new_idle: u64,
}

static mut TEMP_FILE_PATH: Option<String> = None;

fn get_cpu_usage(prev_total: u64, prev_idle: u64) -> (f64, u64, u64) {
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

                let total_diff = current_total.saturating_sub(prev_total);
                let idle_diff = current_idle.saturating_sub(prev_idle);

                let usage = if total_diff > 0 {
                    (total_diff - idle_diff) as f64 / total_diff as f64
                } else {
                    0.0
                };

                return (usage, current_total, current_idle);
            }
        }
    }
    (0.0, prev_total, prev_idle)
}

fn get_ram_usage() -> f64 {
    let mut total = 0.0;
    let mut avail = 0.0;
    
    if let Ok(content) = fs::read_to_string("/proc/meminfo") {
        for line in content.lines() {
            if line.starts_with("MemTotal:") {
                if let Some(val) = line.split_whitespace().nth(1) {
                    total = val.parse().unwrap_or(0.0);
                }
            } else if line.starts_with("MemAvailable:") {
                if let Some(val) = line.split_whitespace().nth(1) {
                    avail = val.parse().unwrap_or(0.0);
                }
            }
            if total > 0.0 && avail > 0.0 { break; }
        }
    }
    
    if total > 0.0 { (total - avail) / total } else { 0.0 }
}

fn get_temp() -> f64 {
    unsafe {
        if TEMP_FILE_PATH.is_none() {
            let base = Path::new("/sys/class/hwmon");
            if let Ok(entries) = fs::read_dir(base) {
                for entry in entries.flatten() {
                    let path = entry.path().join("temp1_input");
                    if path.exists() {
                        TEMP_FILE_PATH = Some(path.to_string_lossy().to_string());
                        break;
                    }
                }
            }
        }

        if let Some(ref path) = TEMP_FILE_PATH {
            if let Ok(c) = fs::read_to_string(path) {
                let temp_m: f64 = c.trim().parse().unwrap_or(0.0);
                return temp_m / 1000.0;
            }
        }
    }
    0.0
}

pub fn fetch_stats(prev_total: u64, prev_idle: u64) -> SystemStats {
    let (cpu, nt, ni) = get_cpu_usage(prev_total, prev_idle);
    SystemStats {
        cpu_usage: cpu,
        ram_usage: get_ram_usage(),
        temp_c: get_temp(),
        new_total: nt,
        new_idle: ni,
    }
}