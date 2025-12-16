use gio::prelude::*;
use fuzzy_matcher::FuzzyMatcher;
use fuzzy_matcher::skim::SkimMatcherV2;
use gio::{Icon, ThemedIcon, FileIcon};
use glib::Cast; 

pub struct AppResult {
    pub name: String,
    pub description: String,
    pub icon: String,
    pub id: String,
    pub score: i64,
}

fn get_icon_string(icon: &Icon) -> String {
    // 1. Try ThemedIcon (e.g. "firefox")
    if let Some(themed) = icon.downcast_ref::<ThemedIcon>() {
        let names = themed.names();
        if !names.is_empty() {
            return names[0].to_string();
        }
    }
    
    // 2. Try FileIcon (e.g. "/usr/share/pixmaps/firefox.png")
    if let Some(file_icon) = icon.downcast_ref::<FileIcon>() {
        if let Some(path) = file_icon.file().path() {
            return path.to_string_lossy().to_string();
        }
    }

    // 3. Fallback to GIO string representation
    if let Some(s) = IconExt::to_string(icon) {
        return s.to_string();
    }

    "application-x-executable".to_string()
}

pub fn search(query: &str) -> Vec<AppResult> {
    let mut results = Vec::new();
    let matcher = SkimMatcherV2::default();
    let apps = gio::AppInfo::all();

    for app in apps {
        if !app.should_show() { continue; }
        
        let name = app.name().to_string();
        let id = app.id().map(|s| s.to_string()).unwrap_or_default();
        
        let score_name = matcher.fuzzy_match(&name, query).unwrap_or(0);
        let score_id = matcher.fuzzy_match(&id, query).unwrap_or(0);
        let final_score = std::cmp::max(score_name, score_id);

        if final_score > 0 {
            let description = app.description()
                .map(|s| s.to_string())
                .unwrap_or_else(|| "Application".to_string());
                
            let icon_str = if let Some(gicon) = app.icon() {
                get_icon_string(&gicon)
            } else {
                "application-x-executable".to_string()
            };

            results.push(AppResult {
                name,
                description,
                icon: icon_str,
                id,
                score: final_score,
            });
        }
    }
    
    results.sort_by(|a, b| b.score.cmp(&a.score));
    results
}