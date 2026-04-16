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
}

// THE FIX: We now pass the last known volume/mute state to compare before showing the UI
async fn handle_volume(proxy: &UIProxy<'_>, last_vol: &mut f64, last_muted: &mut bool) {
    let mut is_muted = false;
    if let Ok(mute_out) = Command::new("pactl").args(&["get-sink-mute", "@DEFAULT_SINK@"]).output().await {
        let mute_str = String::from_utf8_lossy(&mute_out.stdout).to_lowercase();
        if mute_str.contains("yes") {
            is_muted = true;
        }
    }

    let mut level = 0.0;
    if let Ok(vol_out) = Command::new("pactl").args(&["get-sink-volume", "@DEFAULT_SINK@"]).output().await {
        let vol_str = String::from_utf8_lossy(&vol_out.stdout);
        if let Some(idx) = vol_str.find('%') {
            let substr = &vol_str[..idx];
            if let Some(last_space) = substr.rfind(' ') {
                if let Ok(vol) = substr[last_space + 1..].trim().parse::<f64>() {
                    level = vol / 100.0;
                }
            }
        }
    }

    // THE FIX: If the volume and mute state are exactly the same as last time, do nothing!
    if (level - *last_vol).abs() < 0.001 && is_muted == *last_muted {
        return; 
    }

    // Update state
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

// THE FIX: Also state-track brightness just in case
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

            // Check if it actually changed
            if (level - *last_bri).abs() < 0.001 {
                return;
            }
            *last_bri = level;

            let _ = proxy.show_osd("display-brightness-symbolic", level).await;
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
                        // THE FIX: We MUST ensure "sink-input" (media streams) is ignored
                        if (line.contains("on sink ") || line.contains("on server")) && !line.contains("sink-input") {
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

    let mut sigterm = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())?;
    
    let mut vol_pending = false;
    let mut bri_pending = false;

    // Trackers for the current hardware state
    let mut last_volume: f64 = -1.0;
    let mut last_muted: bool = false;
    let mut last_brightness: f64 = -1.0;

    let mut ticker = interval(Duration::from_millis(50));
    ticker.set_missed_tick_behavior(MissedTickBehavior::Skip);

    loop {
        tokio::select! {
            Some(event) = rx.recv() => {
                match event {
                    OsdEvent::Volume => vol_pending = true,
                    OsdEvent::Brightness => bri_pending = true,
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
            },
            
            _ = tokio::signal::ctrl_c() => break,
            _ = sigterm.recv() => break,
        }
    }

    Ok(())
}