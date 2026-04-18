use std::error::Error;
use tokio::io::AsyncBufReadExt;
use tokio::process::Command;
use tokio::sync::mpsc;
use tokio::time::{interval, Duration, MissedTickBehavior};
use zbus::{dbus_proxy, Connection};

#[dbus_proxy(
    interface = "com.meismeric.auranotify.UI",
    default_service = "com.meismeric.auranotify.UI",
    default_path = "/com/meismeric/auranotify/UI"
)]
trait UI {
    #[dbus_proxy(name = "ShowOSD")]
    fn show_osd(&self, icon: &str, level: f64) -> zbus::Result<()>;
}

#[derive(Debug)]
enum OsdEvent {
    Volume,
    Brightness,
    BatteryUpdate,
}

// -------------------------------------------------------------------------
// VOLUME LOGIC
// -------------------------------------------------------------------------
async fn handle_volume(proxy: &UIProxy<'_>, last_vol: &mut f64, last_muted: &mut bool) {
    let mut is_muted = false;
    let mut level = 0.0;
    let mut parsed_successfully = false;

    if let Ok(vol_out) = Command::new("wpctl").args(&["get-volume", "@DEFAULT_AUDIO_SINK@"]).output().await {
        let vol_str = String::from_utf8_lossy(&vol_out.stdout);
        
        if vol_str.contains("[MUTED]") {
            is_muted = true;
        }
        
        if let Some(vol_part) = vol_str.split_whitespace().nth(1) {
            if let Ok(vol) = vol_part.parse::<f64>() {
                level = vol;
                parsed_successfully = true;
            }
        }
    }

    if !parsed_successfully { return; }

    if (level - *last_vol).abs() < 0.001 && is_muted == *last_muted {
        return; 
    }

    *last_vol = level;
    *last_muted = is_muted;

    if level == 0.0 { is_muted = true; }

    let icon = if is_muted {
        "audio-volume-muted-symbolic"
    } else if level < 0.33 {
        "audio-volume-low-symbolic"
    } else if level < 0.66 {
        "audio-volume-medium-symbolic"
    } else if level <= 1.0 {
        "audio-volume-high-symbolic"
    } else {
        "audio-volume-overamplified-symbolic"
    };

    let _ = proxy.show_osd(icon, level).await;
}

// -------------------------------------------------------------------------
// BRIGHTNESS LOGIC
// -------------------------------------------------------------------------
async fn handle_brightness(proxy: &UIProxy<'_>, last_bri: &mut f64) {
    let curr = Command::new("brightnessctl").arg("get").output().await;
    let max = Command::new("brightnessctl").arg("max").output().await;

    if let (Ok(c_out), Ok(m_out)) = (curr, max) {
        let c_str = String::from_utf8_lossy(&c_out.stdout);
        let m_str = String::from_utf8_lossy(&m_out.stdout);

        let c: f64 = c_str.trim().parse().unwrap_or(0.0);
        let m: f64 = m_str.trim().parse().unwrap_or(1.0);

        if m > 0.0 {
            let mut level = c / m;
            if level > 1.0 { level = 1.0; }

            if (level - *last_bri).abs() < 0.001 { return; }
            *last_bri = level;

            let _ = proxy.show_osd("display-brightness-symbolic", level).await;
        }
    }
}

// -------------------------------------------------------------------------
// BATTERY LOGIC
// -------------------------------------------------------------------------
async fn handle_battery(last_cap: &mut f64, last_status: &mut String, allow_notify: bool) {
    let mut cap = -1.0;
    let mut status = String::new();
    
    if let Ok(entries) = std::fs::read_dir("/sys/class/power_supply") {
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().to_string();
            if name.starts_with("BAT") {
                let path = entry.path();
                if let Ok(c) = std::fs::read_to_string(path.join("capacity")) {
                    cap = c.trim().parse().unwrap_or(-1.0);
                }
                if let Ok(s) = std::fs::read_to_string(path.join("status")) {
                    status = s.trim().to_string();
                }
                break;
            }
        }
    }

    if cap >= 0.0 && !status.is_empty() {
        let is_discharging = status == "Discharging";
        let was_discharging = *last_status == "Discharging";

        let plugged_state_changed = is_discharging != was_discharging && !last_status.is_empty();
        let hit_50 = cap == 50.0 && *last_cap != 50.0 && is_discharging;

        *last_cap = cap;
        *last_status = status.clone();

        if !allow_notify { return; }

        if plugged_state_changed || hit_50 {
            let is_full = status == "Full" || status == "Not charging" || cap >= 99.0;
            
            let icon_base = match cap as i32 {
                90..=100 => "battery-full",
                70..=89 => "battery-good",
                30..=69 => "battery-half",
                10..=29 => "battery-low",
                _ => "battery-empty",
            };

            let charging_suffix = if !is_discharging { "-charging-symbolic" } else { "-symbolic" };
            let final_icon = if is_full && !is_discharging { 
                "battery-full-charged-symbolic".to_string() 
            } else { 
                format!("{}{}", icon_base, charging_suffix) 
            };

            if hit_50 {
                let title = format!("Battery at 50%");
                let _ = Command::new("notify-send")
                    .args(&["-u", "normal", "-i", &final_icon, &title])
                    .spawn();
            } else if plugged_state_changed {
                let title = if is_discharging {
                    format!("Charger Disconnected ({}%)", cap)
                } else if is_full {
                    format!("Battery Full ({}%)", cap)
                } else {
                    format!("Charger Plugged In ({}%)", cap)
                };

                let _ = Command::new("notify-send")
                    .args(&["-u", "normal", "-i", &final_icon, &title])
                    .spawn();
            }
        }
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error>> {
    let connection = Connection::session().await?;
    let proxy = UIProxy::builder(&connection)
        .cache_properties(zbus::CacheProperties::No)
        .build()
        .await?;

    let (tx, mut rx) = mpsc::unbounded_channel::<OsdEvent>();

    // 1. Volume Watcher
    let tx_vol = tx.clone();
    tokio::spawn(async move {
        loop {
            if let Ok(mut child) = Command::new("pactl")
                .arg("subscribe")
                .env("LANG", "C")
                .stdout(std::process::Stdio::piped())
                .spawn()
            {
                if let Some(stdout) = child.stdout.take() {
                    let mut reader = tokio::io::BufReader::new(stdout).lines();
                    while let Ok(Some(line)) = reader.next_line().await {
                        // THE FIX: Strictly listen *only* for changes to "sink" (speakers).
                        // Ignore server events, sources (mics), and inputs (apps connecting).
                        if line.contains("change") && line.contains("on sink") && !line.contains("input") {
                            let _ = tx_vol.send(OsdEvent::Volume);
                        }
                    }
                }
                let _ = child.wait().await;
            }
            tokio::time::sleep(Duration::from_secs(2)).await; 
        }
    });

    // 2. Brightness Watcher
    let tx_bri = tx.clone();
    tokio::spawn(async move {
        loop {
            if let Ok(mut child) = Command::new("udevadm")
                .args(&["monitor", "--subsystem-match=backlight"])
                .stdout(std::process::Stdio::piped())
                .spawn()
            {
                if let Some(stdout) = child.stdout.take() {
                    let mut reader = tokio::io::BufReader::new(stdout).lines();
                    while let Ok(Some(line)) = reader.next_line().await {
                        if line.contains("change") || line.contains("backlight") {
                            let _ = tx_bri.send(OsdEvent::Brightness);
                        }
                    }
                }
                let _ = child.wait().await;
            }
            tokio::time::sleep(Duration::from_secs(2)).await; 
        }
    });

    // 3. Battery Event Watcher
    let tx_bat = tx.clone();
    tokio::spawn(async move {
        loop {
            if let Ok(mut child) = Command::new("udevadm")
                .args(&["monitor", "--subsystem-match=power_supply"])
                .stdout(std::process::Stdio::piped())
                .spawn()
            {
                if let Some(stdout) = child.stdout.take() {
                    let mut reader = tokio::io::BufReader::new(stdout).lines();
                    while let Ok(Some(line)) = reader.next_line().await {
                        if line.contains("change") || line.contains("power_supply") {
                            tokio::time::sleep(Duration::from_millis(100)).await;
                            let _ = tx_bat.send(OsdEvent::BatteryUpdate);
                        }
                    }
                }
                let _ = child.wait().await;
            }
            tokio::time::sleep(Duration::from_secs(2)).await; 
        }
    });

    let mut sigterm = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())?;
    
    let mut vol_pending = false;
    let mut bri_pending = false;
    let mut bat_pending = false;

    let mut last_volume: f64 = -1.0;
    let mut last_muted: bool = false;
    let mut last_brightness: f64 = -1.0;
    
    let mut last_bat_cap: f64 = -1.0;
    let mut last_bat_status = String::new();

    // THE FIX 2: Pre-initialize volume and brightness so that IF an edge-case 
    // event slips through the watcher upon startup, it compares against the 
    // REAL baseline rather than triggering a popup against "-1.0".
    if let Ok(vol_out) = std::process::Command::new("wpctl").args(&["get-volume", "@DEFAULT_AUDIO_SINK@"]).output() {
        let vol_str = String::from_utf8_lossy(&vol_out.stdout);
        if vol_str.contains("[MUTED]") { last_muted = true; }
        if let Some(vol_part) = vol_str.split_whitespace().nth(1) {
            if let Ok(vol) = vol_part.parse::<f64>() { last_volume = vol; }
        }
    }

    if let (Ok(c_out), Ok(m_out)) = (
        std::process::Command::new("brightnessctl").arg("get").output(),
        std::process::Command::new("brightnessctl").arg("max").output()
    ) {
        let c: f64 = String::from_utf8_lossy(&c_out.stdout).trim().parse().unwrap_or(0.0);
        let m: f64 = String::from_utf8_lossy(&m_out.stdout).trim().parse().unwrap_or(1.0);
        if m > 0.0 { last_brightness = c / m; }
    }

    handle_battery(&mut last_bat_cap, &mut last_bat_status, false).await;

    let mut ticker = interval(Duration::from_millis(50));
    ticker.set_missed_tick_behavior(MissedTickBehavior::Skip);

    loop {
        tokio::select! {
            Some(event) = rx.recv() => {
                match event {
                    OsdEvent::Volume => vol_pending = true,
                    OsdEvent::Brightness => bri_pending = true,
                    OsdEvent::BatteryUpdate => bat_pending = true,
                }
            },
            
            _ = ticker.tick() => {
                if vol_pending {
                    vol_pending = false;
                    handle_volume(&proxy, &mut last_volume, &mut last_muted).await;
                }
                if bri_pending {
                    bri_pending = false;
                    handle_brightness(&proxy, &mut last_brightness).await;
                }
                if bat_pending {
                    bat_pending = false;
                    handle_battery(&mut last_bat_cap, &mut last_bat_status, true).await;
                }
            },
            
            _ = tokio::signal::ctrl_c() => break,
            _ = sigterm.recv() => break,
        }
    }

    Ok(())
}