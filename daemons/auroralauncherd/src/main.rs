use evalexpr::eval;
use fuzzy_matcher::skim::SkimMatcherV2;
use fuzzy_matcher::FuzzyMatcher;
use gio::prelude::*;
use std::sync::{Arc, RwLock};
use std::error::Error;
use std::collections::HashMap;
use zbus::{ConnectionBuilder, dbus_interface};
use gio::{Icon, ThemedIcon, FileIcon};
use rusqlite::Connection;
use std::time::Duration;

mod clipboard;
use clipboard::ClipboardManager;

#[derive(Clone)]
struct CachedApp {
    name: String,
    description: String,
    icon: String,
    id: String,
}

fn get_icon_string(icon: &Icon) -> String {
    if let Some(themed) = icon.downcast_ref::<ThemedIcon>() {
        let names = themed.names();
        if !names.is_empty() { return names[0].to_string(); }
    }
    if let Some(file_icon) = icon.downcast_ref::<FileIcon>() {
        if let Some(path) = file_icon.file().path() {
            return path.to_string_lossy().to_string();
        }
    }
    if let Some(s) = IconExt::to_string(icon) { return s.to_string(); }
    "application-x-executable".to_string()
}

fn load_apps() -> Vec<CachedApp> {
    let mut cache = Vec::new();
    let apps = gio::AppInfo::all();
    for app in apps {
        if !app.should_show() { continue; }
        cache.push(CachedApp {
            name: app.name().to_string(),
            description: app.description().map(|s| s.to_string()).unwrap_or_else(|| "Application".to_string()),
            icon: if let Some(gicon) = app.icon() { get_icon_string(&gicon) } else { "application-x-executable".to_string() },
            id: app.id().map(|s| s.to_string()).unwrap_or_default(),
        });
    }
    cache
}

fn get_usage_scores() -> HashMap<String, i32> {
    let mut scores = HashMap::new();
    let db_path = format!("{}/.local/share/aurora-insight.db", std::env::var("HOME").unwrap_or_default());
    if let Ok(conn) = Connection::open(db_path) {
        if let Ok(mut stmt) = conn.prepare("SELECT app_class, SUM(usage_seconds) FROM app_usage GROUP BY app_class") {
            if let Ok(rows) = stmt.query_map([], |row| Ok((row.get::<_, String>(0)?, row.get::<_, i32>(1)?))) {
                for row in rows.flatten() {
                    scores.insert(row.0.to_lowercase(), row.1); 
                }
            }
        }
    }
    scores
}

struct SearchProvider {
    app_cache: Arc<RwLock<Vec<CachedApp>>>,
    clipboard: Arc<ClipboardManager>,
    usage_cache: Arc<RwLock<HashMap<String, i32>>>,
}

#[dbus_interface(name = "com.meismeric.auroralauncher.Search")]
impl SearchProvider {
    async fn clear_clipboard(&self) { self.clipboard.clear().await; }
    async fn query_clipboard(&self, term: String) -> Vec<(u32, String, String, String, String, i32)> { self.clipboard.query(&term).await }
    async fn delete_clipboard_item(&self, payload: String) { self.clipboard.delete_item(&payload).await; }
    async fn set_clipboard_item(&self, payload: String) { self.clipboard.set_item(&payload).await; }

    async fn query(&self, term: String) -> Vec<(u32, String, String, String, String, i32)> {
        let mut results = Vec::new();
        let query = term.trim();
        if query.is_empty() { return results; }

        if query.starts_with('/') || query.starts_with('~') {
            let path = if query.starts_with('~') { query.replace('~', &std::env::var("HOME").unwrap_or_default()) } else { query.to_string() };
            let path_obj = std::path::Path::new(&path);
            if path_obj.exists() {
                results.push((2, path.clone(), "Open File or Folder".into(), if path_obj.is_dir() { "folder-symbolic".into() } else { "document-open-symbolic".into() }, format!("xdg-open '{}'", path), 200));
            }
        }

        if query.chars().any(|c| c.is_ascii_digit()) && !query.starts_with("> ") {
            if let Ok(val) = eval(query) {
                results.push((1, val.to_string(), format!("Result: {}", val), "accessories-calculator-symbolic".into(), val.to_string(), 150));
            }
        }

        if query.starts_with("> ") {
            if let Some(cmd) = query.strip_prefix("> ") {
                results.push((2, cmd.trim().into(), "Run Command".into(), "utilities-terminal-symbolic".into(), cmd.trim().into(), 110));
            }
            return results;
        }

        let usage = self.usage_cache.read().unwrap();
        let matcher = SkimMatcherV2::default();
        let apps = self.app_cache.read().unwrap();
        let query_lower = query.to_lowercase();
        
        let mut app_hits: Vec<_> = apps.iter().filter_map(|app| {
            let base_score = std::cmp::max(matcher.fuzzy_match(&app.name, query).unwrap_or(0), matcher.fuzzy_match(&app.id, query).unwrap_or(0));
            
            if base_score > 0 {
                let app_name_lower = app.name.to_lowercase();
                let app_id_lower = app.id.to_lowercase();
                
                let mut final_score = base_score as i32;

                // THE FIX: Massive boost if you typed the exact start of the app name
                if app_name_lower.starts_with(&query_lower) {
                    final_score += 100;
                } else if app_name_lower.contains(&query_lower) {
                    final_score += 20; 
                }

                // THE FIX: Usage multiplier ONLY applies if the match score is decent (>= 50)
                if final_score >= 50 {
                    let mut usage_minutes = 0;
                    for (class, seconds) in usage.iter() {
                        if app_id_lower.contains(class) || app_name_lower.contains(class) {
                            usage_minutes = std::cmp::max(usage_minutes, seconds / 60);
                        }
                    }
                    let multiplier = 1.0 + (std::cmp::min(usage_minutes, 200) as f64 / 400.0);
                    final_score = (final_score as f64 * multiplier) as i32;
                }

                Some((app, final_score))
            } else { None }
        }).collect();

        app_hits.sort_by(|a, b| b.1.cmp(&a.1));
        
        for (app, score) in app_hits.into_iter().take(8) {
            results.push((0, app.name.clone(), app.description.clone(), app.icon.clone(), app.id.clone(), score));
        }

        results
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error>> {
    let app_cache = Arc::new(RwLock::new(load_apps()));
    let cache_clone = app_cache.clone();
    std::thread::spawn(move || {
        let ctx = glib::MainContext::default();
        let _guard = ctx.acquire().unwrap();
        let monitor = gio::AppInfoMonitor::get();
        monitor.connect_changed(move |_| { *cache_clone.write().unwrap() = load_apps(); });
        glib::MainLoop::new(Some(&ctx), false).run();
    });

    let usage_cache = Arc::new(RwLock::new(get_usage_scores()));
    let usage_clone = usage_cache.clone();
    tokio::spawn(async move {
        loop {
            tokio::time::sleep(Duration::from_secs(60)).await;
            if let Ok(mut cache) = usage_clone.write() {
                *cache = get_usage_scores();
            }
        }
    });

    let provider = SearchProvider { 
        app_cache, 
        clipboard: Arc::new(ClipboardManager::new()),
        usage_cache,
    };

    let _conn = ConnectionBuilder::session()?.name("com.meismeric.auroralauncher")?.serve_at("/com/meismeric/auroralauncher", provider)?.build().await?;
    
    let mut sigterm = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())?;
    tokio::select! { _ = tokio::signal::ctrl_c() => {}, _ = sigterm.recv() => {} }
    Ok(())
}