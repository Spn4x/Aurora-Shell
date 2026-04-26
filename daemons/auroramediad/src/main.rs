use std::error::Error;
use std::sync::{Arc, Mutex};
use std::collections::HashMap;
use std::fs;
use std::path::PathBuf;
use futures_util::StreamExt;
use reqwest::Client;
use serde::{Deserialize, Serialize};
use tokio::time::{interval, Duration, MissedTickBehavior};
use zbus::{ConnectionBuilder, dbus_interface, dbus_proxy, fdo::DBusProxy};
use regex::Regex;

#[dbus_proxy(interface = "org.mpris.MediaPlayer2.Player", default_path = "/org/mpris/MediaPlayer2")]
trait Player {
    #[dbus_proxy(property)]
    fn playback_status(&self) -> zbus::Result<String>;
    #[dbus_proxy(property)]
    fn metadata(&self) -> zbus::Result<std::collections::HashMap<String, zbus::zvariant::OwnedValue>>;
    #[dbus_proxy(property)]
    fn position(&self) -> zbus::Result<i64>;
}

// --- NEW: Proxy to talk to Handsight ---
#[dbus_proxy(
    interface = "com.meismeric.auranotify.UI",
    default_service = "com.meismeric.auranotify.UI",
    default_path = "/com/meismeric/auranotify/UI"
)]
trait UI {
    fn update_media_info(&self, player: &str, title: &str, artist: &str, art_url: &str, status: &str) -> zbus::Result<()>;
    fn trigger_media_peek(&self) -> zbus::Result<()>;
}

#[derive(Serialize, Deserialize, Default, Clone)]
struct SavedTrackData {
    lyric_id: Option<i64>,
    sync_offset: i32,
}

struct MediaManager {
    active_player: String,
    current_lyrics: String,
    current_lyric_index: i32,
    sync_offset: i32,
    
    manual_override: Arc<Mutex<Option<String>>>,
    force_lyric_refresh: Arc<Mutex<bool>>,
    offset_update: Arc<Mutex<Option<i32>>>,
}

#[dbus_interface(name = "com.meismeric.aurora.MediaManager")]
impl MediaManager {
    #[dbus_interface(property)]
    fn active_player(&self) -> String { self.active_player.clone() }
    #[dbus_interface(property)]
    fn current_lyrics(&self) -> String { self.current_lyrics.clone() }
    #[dbus_interface(property)]
    fn current_lyric_index(&self) -> i32 { self.current_lyric_index }
    #[dbus_interface(property)]
    fn sync_offset(&self) -> i32 { self.sync_offset }

    fn select_player(&mut self, name: String) {
        if let Ok(mut over) = self.manual_override.lock() { *over = if name.is_empty() { None } else { Some(name) }; }
    }
    fn set_sync_offset(&mut self, offset: i32) {
        if let Ok(mut update) = self.offset_update.lock() { *update = Some(offset); }
    }
    fn save_current_lyrics(&mut self) {
        if let Ok(mut refresh) = self.force_lyric_refresh.lock() { *refresh = true; }
    }
}

// --- LYRICS ENGINE ---
#[derive(Clone)]
struct TrackMeta {
    title: String,
    artist: String,
    album: String,
    duration_s: u64,
    signature: String,
}

fn get_cache_path() -> PathBuf {
    let home = std::env::var("HOME").unwrap_or_else(|_| "/tmp".into());
    let path = PathBuf::from(home).join(".cache/aurora-shell/lyrics.json");
    if let Some(p) = path.parent() { fs::create_dir_all(p).ok(); }
    path
}

fn load_saved_data(sig: &str) -> SavedTrackData {
    if let Ok(data) = fs::read_to_string(get_cache_path()) {
        if let Ok(map) = serde_json::from_str::<HashMap<String, SavedTrackData>>(&data) {
            if let Some(d) = map.get(sig) { return d.clone(); }
        }
    }
    SavedTrackData { lyric_id: None, sync_offset: -650 }
}

fn save_track_data(sig: &str, data: SavedTrackData) {
    let mut map: HashMap<String, SavedTrackData> = HashMap::new();
    if let Ok(content) = fs::read_to_string(get_cache_path()) {
        if let Ok(m) = serde_json::from_str(&content) { map = m; }
    }
    map.insert(sig.to_string(), data);
    if let Ok(json) = serde_json::to_string_pretty(&map) { fs::write(get_cache_path(), json).ok(); }
}

async fn fetch_lyrics(meta: &TrackMeta, saved_id: Option<i64>) -> Option<String> {
    let client = Client::new();
    let user_agent = "AuroraMediaDaemon/1.0";

    if let Some(id) = saved_id {
        let url = format!("https://lrclib.net/api/get/{}", id);
        if let Ok(resp) = client.get(&url).header("User-Agent", user_agent).send().await {
            if let Ok(json) = resp.json::<serde_json::Value>().await {
                if let Some(lrc) = json.get("syncedLyrics").and_then(|s| s.as_str()) {
                    if !lrc.is_empty() { return Some(lrc.to_string()); }
                }
            }
        }
    }

    let url = format!("https://lrclib.net/api/get?track_name={}&artist_name={}&album_name={}&duration={}",
        urlencoding::encode(&meta.title), urlencoding::encode(&meta.artist), 
        urlencoding::encode(&meta.album), meta.duration_s);
    
    if let Ok(resp) = client.get(&url).header("User-Agent", user_agent).send().await {
        if let Ok(json) = resp.json::<serde_json::Value>().await {
            if let Some(lrc) = json.get("syncedLyrics").and_then(|s| s.as_str()) {
                if !lrc.is_empty() { return Some(lrc.to_string()); }
            }
        }
    }

    let url = format!("https://lrclib.net/api/search?track_name={}&artist_name={}",
        urlencoding::encode(&meta.title), urlencoding::encode(&meta.artist));
    
    if let Ok(resp) = client.get(&url).header("User-Agent", user_agent).send().await {
        if let Ok(json) = resp.json::<serde_json::Value>().await {
            if let Some(arr) = json.as_array() {
                for item in arr {
                    if let Some(lrc) = item.get("syncedLyrics").and_then(|s| s.as_str()) {
                        if !lrc.is_empty() { return Some(lrc.to_string()); }
                    }
                }
            }
        }
    }
    None
}

fn parse_lrc_timestamps(lrc: &str) -> Vec<i64> {
    let mut times = Vec::new();
    let re = Regex::new(r"\[(\d{2}):(\d{2})[.:](\d{2,3})\]").unwrap();
    for line in lrc.lines() {
        if let Some(caps) = re.captures(line) {
            let min: i64 = caps[1].parse().unwrap_or(0);
            let sec: i64 = caps[2].parse().unwrap_or(0);
            let mut cs: i64 = caps[3].parse().unwrap_or(0);
            if caps[3].len() == 2 { cs *= 10; }
            times.push(min * 60000 + sec * 1000 + cs);
        }
    }
    times
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error>> {
    let manual_override = Arc::new(Mutex::new(None));
    let force_lyric_refresh = Arc::new(Mutex::new(false));
    let offset_update = Arc::new(Mutex::new(None));

    let provider = MediaManager { 
        active_player: String::new(),
        current_lyrics: String::new(),
        current_lyric_index: -1,
        sync_offset: -550,
        manual_override: manual_override.clone(),
        force_lyric_refresh: force_lyric_refresh.clone(),
        offset_update: offset_update.clone(),
    };
    
    let conn = ConnectionBuilder::session()?
        .name("com.meismeric.aurora.MediaManager")?
        .serve_at("/com/meismeric/aurora/MediaManager", provider)?
        .build()
        .await?;

    println!("auroramediad: Smart MPRIS Coordinator running.");

    let fdo_proxy = DBusProxy::new(&conn).await?;
    let mut owner_stream = fdo_proxy.receive_name_owner_changed().await?;
    
    // UI PROXY (Handsight)
    let ui_proxy = UIProxy::new(&conn).await?;

    let mut available_players: Vec<String> = fdo_proxy.list_names().await?
        .into_iter()
        .map(|n| n.to_string()) 
        .filter(|n| n.starts_with("org.mpris.MediaPlayer2.") && !n.contains("playerctld"))
        .collect();

    let mut sigterm = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())?;
    
    let mut ticker = interval(Duration::from_millis(150));
    ticker.set_missed_tick_behavior(MissedTickBehavior::Skip);

    let mut last_active = String::new();
    let mut current_meta_sig = String::new();
    let mut current_timestamps: Vec<i64> = Vec::new();

    // UI Tracking
    let mut last_ui_status = String::new();
    let mut last_ui_title = String::new();

    loop {
        tokio::select! {
            Some(sig) = owner_stream.next() => {
                if let Ok(args) = sig.args() {
                    if args.name().starts_with("org.mpris.MediaPlayer2.") && !args.name().contains("playerctld") {
                        if let Some(new_owner_ref) = args.new_owner().as_ref() {
                            if !new_owner_ref.is_empty() && !available_players.contains(&args.name().to_string()) {
                                available_players.push(args.name().to_string());
                            }
                        } else {
                            available_players.retain(|x| x.as_str() != args.name().as_str());
                        }
                    }
                }
            }

            _ = ticker.tick() => {
                let mut manual_choice = None;
                if let Ok(mut over) = manual_override.lock() {
                    if let Some(ref m) = *over {
                        if !available_players.contains(m) { *over = None; } 
                        else { manual_choice = Some(m.clone()); }
                    }
                }

                let mut best_player = manual_choice.unwrap_or_default();
                if best_player.is_empty() {
                    let mut best_score = -1;
                    for name in &available_players {
                        if let Ok(builder) = PlayerProxy::builder(&conn).destination(name.as_str()) {
                            if let Ok(player_proxy) = builder.build().await {
                                let status = player_proxy.playback_status().await.unwrap_or_else(|_| "Stopped".to_string());
                                let score = match status.as_str() { "Playing" => 2, "Paused" => 1, _ => 0 };
                                let bias = if name == &last_active { 1 } else { 0 };
                                if score * 10 + bias > best_score {
                                    best_score = score * 10 + bias;
                                    best_player = name.clone();
                                }
                            }
                        }
                    }
                }

                let object_server = conn.object_server();
                let iface_ref = object_server.interface::<_, MediaManager>("/com/meismeric/aurora/MediaManager").await?;

                if best_player != last_active {
                    last_active = best_player.clone();
                    let mut iface = iface_ref.get_mut().await;
                    iface.active_player = best_player.clone();
                    iface.active_player_changed(iface_ref.signal_context()).await?;
                }

                if !best_player.is_empty() {
                    if let Ok(builder) = PlayerProxy::builder(&conn).destination(best_player.as_str()) {
                        if let Ok(player_proxy) = builder.build().await {
                            
                            let mut apply_offset = None;
                            if let Ok(mut u) = offset_update.lock() {
                                if let Some(val) = *u { apply_offset = Some(val); *u = None; }
                            }
                            if let Some(val) = apply_offset {
                                let mut iface = iface_ref.get_mut().await;
                                iface.sync_offset = val;
                                iface.sync_offset_changed(iface_ref.signal_context()).await?;
                                let mut saved = load_saved_data(&current_meta_sig);
                                saved.sync_offset = val;
                                save_track_data(&current_meta_sig, saved);
                            }

                            // --- METADATA EXTRACTION ---
                            let mut title = String::new();
                            let mut artist = String::new();
                            let mut album = String::new();
                            let mut art_url = String::new();
                            let mut dur_us: u64 = 0;

                            if let Ok(metadata) = player_proxy.metadata().await {
                                let get_str = |key: &str| -> String {
                                    metadata.get(key).and_then(|v| {
                                        if let zbus::zvariant::Value::Str(s) = &**v { Some(s.as_str().to_string()) }
                                        else if let zbus::zvariant::Value::Array(a) = &**v { 
                                            a.get().first().and_then(|i| if let zbus::zvariant::Value::Str(s) = i { Some(s.as_str().to_string()) } else { None })
                                        } else { None }
                                    }).unwrap_or_default()
                                };
                                
                                title = get_str("xesam:title");
                                artist = get_str("xesam:artist");
                                album = get_str("xesam:album");
                                art_url = get_str("mpris:artUrl");
                                
                                dur_us = metadata.get("mpris:length").and_then(|v| {
                                    if let zbus::zvariant::Value::U64(d) = &**v { Some(*d) }
                                    else if let zbus::zvariant::Value::I64(d) = &**v { Some(*d as u64) }
                                    else { None }
                                }).unwrap_or(0);
                            }

                            let status = player_proxy.playback_status().await.unwrap_or_else(|_| "Stopped".to_string());
                            let signature = format!("{} - {}", artist, title);

                            // --- PUSH TO HANDSIGHT UI ---
                            let _ = ui_proxy.update_media_info(&best_player, &title, &artist, &art_url, &status).await;

                            if !title.is_empty() && (title != last_ui_title || (status == "Playing" && last_ui_status != "Playing")) {
                                let _ = ui_proxy.trigger_media_peek().await;
                            }
                            
                            last_ui_title = title.clone();
                            last_ui_status = status.clone();

                            // --- LYRICS FETCHING ---
                            let mut do_refresh = false;
                            if let Ok(mut r) = force_lyric_refresh.lock() {
                                if *r { do_refresh = true; *r = false; }
                            }

                            if !title.is_empty() && (signature != current_meta_sig || do_refresh) {
                                current_meta_sig = signature.clone();
                                let saved_data = load_saved_data(&current_meta_sig);
                                
                                let mut iface = iface_ref.get_mut().await;
                                iface.sync_offset = saved_data.sync_offset;
                                iface.sync_offset_changed(iface_ref.signal_context()).await?;
                                iface.current_lyrics = String::new();
                                iface.current_lyric_index = -1;
                                iface.current_lyrics_changed(iface_ref.signal_context()).await?;
                                iface.current_lyric_index_changed(iface_ref.signal_context()).await?;
                                current_timestamps.clear();

                                let t_meta = TrackMeta { title, artist, album, duration_s: dur_us / 1_000_000, signature: signature.clone() };
                                let iface_clone = iface_ref.clone();
                                
                                tokio::spawn(async move {
                                    if let Some(lrc) = fetch_lyrics(&t_meta, saved_data.lyric_id).await {
                                        let mut iface = iface_clone.get_mut().await;
                                        iface.current_lyrics = lrc;
                                        let _ = iface.current_lyrics_changed(iface_clone.signal_context()).await;
                                    }
                                });
                            }

                            let current_lrc = iface_ref.get().await.current_lyrics.clone();
                            if !current_lrc.is_empty() && current_timestamps.is_empty() {
                                current_timestamps = parse_lrc_timestamps(&current_lrc);
                            }

                            // --- LYRICS SYNCING ---
                            if !current_timestamps.is_empty() && status == "Playing" {
                                if let Ok(pos_us) = player_proxy.position().await {
                                    let offset = iface_ref.get().await.sync_offset as i64;
                                    let pos_ms = std::cmp::max(0, (pos_us / 1000) - offset);
                                    
                                    let mut new_index = -1;
                                    for (i, &ts) in current_timestamps.iter().enumerate().rev() {
                                        if pos_ms >= ts {
                                            new_index = i as i32;
                                            break;
                                        }
                                    }

                                    let mut iface = iface_ref.get_mut().await;
                                    if new_index != iface.current_lyric_index {
                                        iface.current_lyric_index = new_index;
                                        iface.current_lyric_index_changed(iface_ref.signal_context()).await?;
                                    }
                                }
                            }
                        }
                    }
                } else {
                    // Empty player state
                    let _ = ui_proxy.update_media_info("", "", "", "", "Stopped").await;
                }
            },
            
            _ = tokio::signal::ctrl_c() => break,
            _ = sigterm.recv() => break,
        }
    }
    Ok(())
}