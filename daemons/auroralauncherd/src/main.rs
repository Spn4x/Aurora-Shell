use evalexpr::eval;
use fuzzy_matcher::skim::SkimMatcherV2;
use fuzzy_matcher::FuzzyMatcher;
use gio::prelude::*;
use std::sync::{Arc, RwLock};
use std::error::Error;
use zbus::{ConnectionBuilder, dbus_interface};
use gio::{Icon, ThemedIcon, FileIcon};

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
    if let Some(s) = IconExt::to_string(icon) {
        return s.to_string();
    }
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

struct SearchProvider {
    app_cache: Arc<RwLock<Vec<CachedApp>>>,
}

#[dbus_interface(name = "com.meismeric.auroralauncher.Search")]
impl SearchProvider {
    /// Returns: Vec<(Type, Title, Description, Icon, ExecData, Score)>
    /// Type: 0=App, 1=Calc, 2=Command
    async fn query(&self, term: String) -> Vec<(u32, String, String, String, String, i32)> {
        let mut results = Vec::new();
        let query = term.trim();

        if query.is_empty() { return results; }

        let is_command = query.starts_with("> ");

        // 1. Calculator (Only active in Default Mode)
        if !is_command && query.chars().any(|c| c.is_ascii_digit()) {
            if let Ok(val) = eval(query) {
                let result_str = val.to_string();
                if result_str != query {
                    results.push((
                        1, 
                        result_str.clone(), 
                        format!("Result: {}", result_str), 
                        "accessories-calculator-symbolic".to_string(), 
                        result_str, 
                        120
                    ));
                }
            }
        }

        // 2. Command (Only active in Command Mode)
        if is_command {
            if let Some(cmd) = query.strip_prefix("> ") {
                if !cmd.trim().is_empty() {
                    results.push((
                        2, 
                        cmd.trim().to_string(), 
                        "Run command".to_string(), 
                        "utilities-terminal-symbolic".to_string(), 
                        cmd.trim().to_string(), 
                        110
                    ));
                }
            }
        }

        // 3. Apps (Only active in Default Mode)
        if !is_command {
            let matcher = SkimMatcherV2::default();
            let apps = self.app_cache.read().unwrap();
            
            let mut app_hits: Vec<_> = apps.iter().filter_map(|app| {
                let score_name = matcher.fuzzy_match(&app.name, query).unwrap_or(0);
                let score_id = matcher.fuzzy_match(&app.id, query).unwrap_or(0);
                let score = std::cmp::max(score_name, score_id);
                if score > 0 { Some((app, score)) } else { None }
            }).collect();

            app_hits.sort_by(|a, b| b.1.cmp(&a.1));

            for (app, score) in app_hits.into_iter().take(15) {
                results.push((
                    0,
                    app.name.clone(),
                    app.description.clone(),
                    app.icon.clone(),
                    app.id.clone(),
                    score as i32
                ));
            }
        }

        results
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error>> {
    // Load initial cache
    let app_cache = Arc::new(RwLock::new(load_apps()));
    let cache_clone = app_cache.clone();

    // Spawn a GLib main loop in a background thread to watch for app installations/removals
    std::thread::spawn(move || {
        let ctx = glib::MainContext::default();
        let _guard = ctx.acquire().unwrap();
        
        let monitor = gio::AppInfoMonitor::get();
        monitor.connect_changed(move |_| {
            println!("auroralauncherd: App database changed, refreshing cache...");
            *cache_clone.write().unwrap() = load_apps();
        });

        let loop_ = glib::MainLoop::new(Some(&ctx), false);
        loop_.run();
    });

    let provider = SearchProvider { app_cache };

    let _conn = ConnectionBuilder::session()?
        .name("com.meismeric.auroralauncher")?
        .serve_at("/com/meismeric/auroralauncher", provider)?
        .build()
        .await?;

    println!("auroralauncherd: D-Bus search provider running.");

    // Keep the daemon running
    std::future::pending::<()>().await;
    Ok(())
}