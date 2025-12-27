use gio::prelude::*;
use fuzzy_matcher::FuzzyMatcher;
use fuzzy_matcher::skim::SkimMatcherV2;
use gio::{Icon, ThemedIcon, FileIcon};
use std::sync::OnceLock;

// Internal struct to cache app data so we don't touch GIO/Disk constantly
struct CachedApp {
    name: String,
    description: String,
    icon: String,
    id: String,
}

// Global Cache
static APP_CACHE: OnceLock<Vec<CachedApp>> = OnceLock::new();

pub struct AppResult {
    pub name: String,
    pub description: String,
    pub icon: String,
    pub id: String,
    pub score: i64,
}

fn get_icon_string(icon: &Icon) -> String {
    if let Some(themed) = icon.downcast_ref::<ThemedIcon>() {
        let names = themed.names();
        if !names.is_empty() {
            return names[0].to_string();
        }
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
        
        let name = app.name().to_string();
        let id = app.id().map(|s| s.to_string()).unwrap_or_default();
        let description = app.description()
            .map(|s| s.to_string())
            .unwrap_or_else(|| "Application".to_string());
            
        let icon = if let Some(gicon) = app.icon() {
            get_icon_string(&gicon)
        } else {
            "application-x-executable".to_string()
        };

        cache.push(CachedApp {
            name,
            description,
            icon,
            id,
        });
    }
    cache
}

pub fn search(query: &str) -> Vec<AppResult> {
    let mut results = Vec::new();
    let matcher = SkimMatcherV2::default();

    // Get cache or initialize it if it's the first run
    let apps = APP_CACHE.get_or_init(load_apps);

    for app in apps {
        let score_name = matcher.fuzzy_match(&app.name, query).unwrap_or(0);
        let score_id = matcher.fuzzy_match(&app.id, query).unwrap_or(0);
        let final_score = std::cmp::max(score_name, score_id);

        if final_score > 0 {
            results.push(AppResult {
                name: app.name.clone(),
                description: app.description.clone(),
                icon: app.icon.clone(),
                id: app.id.clone(),
                score: final_score,
            });
        }
    }
    
    results.sort_by(|a, b| b.score.cmp(&a.score));
    results
}