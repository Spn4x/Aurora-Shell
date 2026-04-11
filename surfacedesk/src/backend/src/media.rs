use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::env;
use sha2::{Sha256, Digest};

fn get_cache_dir() -> PathBuf {
    let home = env::var("HOME").unwrap_or_else(|_| "/tmp".into());
    let path = Path::new(&home).join(".cache/surfacedesk/art");
    fs::create_dir_all(&path).ok();
    path
}

pub fn get_cached_art_path(url: &str) -> Option<String> {
    let mut hasher = Sha256::new();
    hasher.update(url);
    let result = hasher.finalize();
    let hash_hex = hex::encode(result);
    
    let path = get_cache_dir().join(&hash_hex);
    
    // Always return the computed path so C knows where it *should* be
    Some(path.to_string_lossy().to_string())
}

pub fn download_art(url: &str, dest_path: &str) -> bool {
    let status = Command::new("curl")
        .args(&["-s", "-L", "-o", dest_path, url])
        .status();
        
    match status {
        Ok(s) => s.success(),
        Err(_) => false,
    }
}