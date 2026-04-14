use serde::{Deserialize, Serialize};
use sha2::{Sha256, Digest};
use std::fs;
use std::path::PathBuf;
use std::sync::Arc;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::process::Command;
use tokio::sync::RwLock;
use tokio::time::{sleep, Duration};

const MAX_HISTORY: usize = 10;

#[derive(Serialize, Deserialize, Clone)]
pub struct ClipItem {
    pub id: String,
    pub is_image: bool,
    pub content: String,
    pub timestamp: u64,
}

pub struct ClipboardManager {
    items: Arc<RwLock<Vec<ClipItem>>>,
    cache_dir: PathBuf,
}

impl ClipboardManager {
    pub fn new() -> Self {
        let home = std::env::var("HOME").unwrap_or_else(|_| "/tmp".into());
        let cache_dir = PathBuf::from(home).join(".cache/aurora-shell/clipboard");
        
        // Forcefully wipe old cache on startup to prevent buggy states from previous sessions
        let _ = fs::remove_dir_all(&cache_dir);
        fs::create_dir_all(&cache_dir).ok();

        // Always start fresh
        let items = Arc::new(RwLock::new(Vec::new()));
        
        let manager = Self {
            items: items.clone(),
            cache_dir: cache_dir.clone(),
        };

        // Split text and images into two separate, reliable watchers
        manager.start_text_watcher(items.clone());
        manager.start_image_watcher(items.clone());

        manager
    }

    async fn save_history(cache_dir: &PathBuf, items: &[ClipItem]) {
        let history_file = cache_dir.join("history.json");
        if let Ok(data) = serde_json::to_string(items) {
            let _ = fs::write(history_file, data);
        }
    }

    async fn add_item(items: Arc<RwLock<Vec<ClipItem>>>, cache_dir: PathBuf, item: ClipItem) {
        let mut list = items.write().await;
        list.retain(|x| x.content != item.content);
        list.insert(0, item);
        
        // Strictly enforce history limit
        while list.len() > MAX_HISTORY {
            if let Some(removed) = list.pop() {
                if removed.is_image {
                    let _ = fs::remove_file(PathBuf::from(&removed.content));
                }
            }
        }
        Self::save_history(&cache_dir, &list).await;
    }

    // Method to wipe everything
    pub async fn clear(&self) {
        let mut list = self.items.write().await;
        for item in list.iter() {
            if item.is_image {
                let _ = fs::remove_file(&item.content);
            }
        }
        list.clear();
        Self::save_history(&self.cache_dir, &list).await;
    }

    fn start_text_watcher(&self, items: Arc<RwLock<Vec<ClipItem>>>) {
        let cache_dir = self.cache_dir.clone();
        tokio::spawn(async move {
            let mut child = Command::new("wl-paste")
                .args(&["-t", "text/plain", "--watch", "echo", "T"])
                .stdout(std::process::Stdio::piped())
                .kill_on_drop(true)
                .spawn()
                .expect("Failed to start text watcher");

            let stdout = child.stdout.take().unwrap();
            let mut reader = BufReader::new(stdout);
            let mut line = String::new();
            let mut last_text = String::new();

            while reader.read_line(&mut line).await.unwrap_or(0) > 0 {
                line.clear();
                if let Ok(output) = Command::new("wl-paste").arg("-n").arg("-t").arg("text/plain").output().await {
                    let text = String::from_utf8_lossy(&output.stdout).to_string();
                    
                    if !text.trim().is_empty() && text != last_text {
                        last_text = text.clone();
                        
                        let id = format!("{:x}", Sha256::digest(text.as_bytes()));
                        let item = ClipItem {
                            id,
                            is_image: false,
                            content: text.clone(),
                            timestamp: std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_secs(),
                        };
                        Self::add_item(items.clone(), cache_dir.clone(), item).await;

                        let text_clone = text.clone();
                        tokio::spawn(async move {
                            sleep(Duration::from_millis(50)).await; // Let original app settle
                            if let Ok(mut wl_copy) = Command::new("wl-copy")
                                .arg("--foreground") // Force it to hold ownership
                                .stdin(std::process::Stdio::piped())
                                .spawn() 
                            {
                                if let Some(mut stdin) = wl_copy.stdin.take() {
                                    let _ = stdin.write_all(text_clone.as_bytes()).await;
                                }
                                let _ = wl_copy.wait().await; // Await completion to prevent zombies
                            }
                        });
                    }
                }
            }
        });
    }

    fn start_image_watcher(&self, items: Arc<RwLock<Vec<ClipItem>>>) {
        let cache_dir = self.cache_dir.clone();
        tokio::spawn(async move {
            let mut child = Command::new("wl-paste")
                .args(&["-t", "image/png", "--watch", "echo", "I"])
                .stdout(std::process::Stdio::piped())
                .kill_on_drop(true)
                .spawn()
                .expect("Failed to start image watcher");

            let stdout = child.stdout.take().unwrap();
            let mut reader = BufReader::new(stdout);
            let mut line = String::new();
            let mut last_img_hash = String::new();

            while reader.read_line(&mut line).await.unwrap_or(0) > 0 {
                line.clear();
                if let Ok(output) = Command::new("wl-paste").arg("-t").arg("image/png").output().await {
                    let bytes = output.stdout;
                    if !bytes.is_empty() {
                        let hash = format!("{:x}", Sha256::digest(&bytes));
                        
                        if hash != last_img_hash {
                            last_img_hash = hash.clone();
                            
                            let img_path = cache_dir.join(format!("{}.png", hash));
                            let _ = fs::write(&img_path, &bytes);

                            let item = ClipItem {
                                id: hash,
                                is_image: true,
                                content: img_path.to_string_lossy().to_string(),
                                timestamp: std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_secs(),
                            };
                            Self::add_item(items.clone(), cache_dir.clone(), item).await;

                            let bytes_clone = bytes.clone();
                            tokio::spawn(async move {
                                sleep(Duration::from_millis(50)).await;
                                if let Ok(mut wl_copy) = Command::new("wl-copy")
                                    .arg("-t").arg("image/png")
                                    .arg("--foreground") // Force it to hold ownership
                                    .stdin(std::process::Stdio::piped())
                                    .spawn() 
                                {
                                    if let Some(mut stdin) = wl_copy.stdin.take() {
                                        let _ = stdin.write_all(&bytes_clone).await;
                                    }
                                    let _ = wl_copy.wait().await; // Await completion to prevent zombies
                                }
                            });
                        }
                    }
                }
            }
        });
    }

    pub async fn query(&self, term: &str) -> Vec<(u32, String, String, String, String, i32)> {
        let list = self.items.read().await;
        let mut results = Vec::new();
        let term = term.to_lowercase();

        for item in list.iter() {
            let mut score = 100;
            if !term.is_empty() {
                if item.is_image || !item.content.to_lowercase().contains(&term) { continue; }
                score = 110; 
            }

            let (title, icon, payload) = if item.is_image {
                ("Image".to_string(), item.content.clone(), format!("IMG:{}", item.content))
            } else {
                let mut t = item.content.replace("\n", " ");
                if t.len() > 60 { t.truncate(60); t.push_str("..."); }
                (t, "edit-paste-symbolic".to_string(), format!("TXT:{}", item.content))
            };

            let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_secs();
            let diff_mins = (now - item.timestamp) / 60;
            let desc = if diff_mins < 1 { "Just now".to_string() } else if diff_mins < 60 { format!("{}m ago", diff_mins) } else { format!("{}h ago", diff_mins / 60) };

            results.push((3, title, desc, icon, payload, score));
        }
        results
    }

    pub async fn set_item(&self, payload: &str) {
        if payload.starts_with("IMG:") {
            let path = &payload[4..];
            if let Ok(bytes) = fs::read(path) {
                tokio::spawn(async move {
                    if let Ok(mut wl_copy) = Command::new("wl-copy")
                        .arg("-t").arg("image/png")
                        .arg("--foreground")
                        .stdin(std::process::Stdio::piped())
                        .spawn() 
                    {
                        if let Some(mut stdin) = wl_copy.stdin.take() {
                            let _ = stdin.write_all(&bytes).await;
                        }
                        let _ = wl_copy.wait().await;
                    }
                });
            }
        } else if payload.starts_with("TXT:") {
            let text = payload[4..].to_string();
            tokio::spawn(async move {
                if let Ok(mut wl_copy) = Command::new("wl-copy")
                    .arg("--foreground")
                    .stdin(std::process::Stdio::piped())
                    .spawn() 
                {
                    if let Some(mut stdin) = wl_copy.stdin.take() {
                        let _ = stdin.write_all(text.as_bytes()).await;
                    }
                    let _ = wl_copy.wait().await;
                }
            });
        }
    }

    pub async fn delete_item(&self, payload: &str) {
        let mut list = self.items.write().await;
        list.retain(|x| {
            let match_str = if x.is_image { format!("IMG:{}", x.content) } else { format!("TXT:{}", x.content) };
            if match_str == payload {
                if x.is_image { let _ = fs::remove_file(&x.content); }
                false
            } else { true }
        });
        Self::save_history(&self.cache_dir, &list).await;
    }
}