use std::error::Error;
use std::sync::{Arc, Mutex};
use zbus::{ConnectionBuilder, dbus_interface, dbus_proxy};
use tokio::time::{sleep, Duration};

#[dbus_proxy(interface = "org.freedesktop.DBus", default_service = "org.freedesktop.DBus", default_path = "/org/freedesktop/DBus")]
trait DBus {
    fn list_names(&self) -> zbus::Result<Vec<String>>;
}

#[dbus_proxy(interface = "org.mpris.MediaPlayer2.Player", default_path = "/org/mpris/MediaPlayer2")]
trait Player {
    #[dbus_proxy(property)]
    fn playback_status(&self) -> zbus::Result<String>;
}

struct MediaManager {
    active_player: String,
    manual_override: Arc<Mutex<Option<String>>>,
}

#[dbus_interface(name = "com.meismeric.aurora.MediaManager")]
impl MediaManager {
    #[dbus_interface(property)]
    fn active_player(&self) -> String {
        self.active_player.clone()
    }

    // NEW: Allow the UI to manually lock onto a specific player
    fn select_player(&mut self, name: String) {
        if let Ok(mut over) = self.manual_override.lock() {
            if name.is_empty() {
                *over = None; // Reset to auto
            } else {
                *over = Some(name);
            }
        }
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error>> {
    let manual_override = Arc::new(Mutex::new(None));
    let provider = MediaManager { 
        active_player: String::new(),
        manual_override: manual_override.clone(),
    };
    
    let conn = ConnectionBuilder::session()?
        .name("com.meismeric.aurora.MediaManager")?
        .serve_at("/com/meismeric/aurora/MediaManager", provider)?
        .build()
        .await?;

    println!("auroramediad: MPRIS Centralization Daemon running.");

    let dbus_proxy = DBusProxy::new(&conn).await?;
    let mut last_active = String::new();
    let mut sigterm = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())?;

    loop {
        tokio::select! {
            _ = sleep(Duration::from_millis(500)) => {
                let mut best_player = String::new();
                let mut available_players = vec![];

                if let Ok(names) = dbus_proxy.list_names().await {
                    available_players = names.into_iter()
                        .filter(|n| n.starts_with("org.mpris.MediaPlayer2.") && !n.contains("playerctld"))
                        .collect();
                }

                // Check manual override
                let mut manual_choice = None;
                if let Ok(mut over) = manual_override.lock() {
                    if let Some(ref m) = *over {
                        if !available_players.contains(m) {
                            *over = None; // Player closed, clear override
                        } else {
                            manual_choice = Some(m.clone());
                        }
                    }
                }

                if let Some(m) = manual_choice {
                    // Lock to user selection
                    best_player = m;
                } else {
                    // Auto-select logic
                    let mut best_score = -1;
                    for name in available_players {
                        if let Ok(builder) = PlayerProxy::builder(&conn).destination(name.as_str()) {
                            if let Ok(player_proxy) = builder.build().await {
                                let status = player_proxy.playback_status().await.unwrap_or_else(|_| "Stopped".to_string());
                                let score = match status.as_str() {
                                    "Playing" => 2,
                                    "Paused" => 1,
                                    _ => 0,
                                };

                                let bias = if name == last_active { 1 } else { 0 };

                                if score * 10 + bias > best_score {
                                    best_score = score * 10 + bias;
                                    best_player = name;
                                }
                            }
                        }
                    }
                }

                // Broadcast if changed
                if best_player != last_active {
                    last_active = best_player.clone();
                    let object_server = conn.object_server();
                    let iface_ref = object_server.interface::<_, MediaManager>("/com/meismeric/aurora/MediaManager").await?;
                    let mut iface = iface_ref.get_mut().await;
                    
                    iface.active_player = best_player.clone();
                    iface.active_player_changed(iface_ref.signal_context()).await?;
                    
                    println!("auroramediad: Active player synced to -> {}", if best_player.is_empty() { "None" } else { &best_player });
                }
            },
            _ = tokio::signal::ctrl_c() => break,
            _ = sigterm.recv() => break,
        }
    }
    Ok(())
}