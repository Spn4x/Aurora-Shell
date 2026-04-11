use std::ffi::CStr;
use std::fs::{self};
use std::os::raw::c_char;
use std::path::{PathBuf};
use std::process::Command;
use std::env;

struct Color { r: u8, g: u8, b: u8 }

impl Color {
    fn from_hex(hex: &str) -> Option<Self> {
        let hex = hex.trim_start_matches('#');
        if hex.len() != 6 { return None; }
        let r = u8::from_str_radix(&hex[0..2], 16).ok()?;
        let g = u8::from_str_radix(&hex[2..4], 16).ok()?;
        let b = u8::from_str_radix(&hex[4..6], 16).ok()?;
        Some(Self { r, g, b })
    }

    fn to_hex(&self) -> String {
        format!("#{:02x}{:02x}{:02x}", self.r, self.g, self.b)
    }

    fn luminance(&self) -> f64 {
        0.299 * (self.r as f64) + 0.587 * (self.g as f64) + 0.114 * (self.b as f64)
    }

    fn mix(&self, other: &Color, factor: f64) -> Color {
        let mix = |c1: u8, c2: u8| -> u8 {
            ((c1 as f64 * (1.0 - factor)) + (c2 as f64 * factor)).clamp(0.0, 255.0) as u8
        };
        Color {
            r: mix(self.r, other.r),
            g: mix(self.g, other.g),
            b: mix(self.b, other.b),
        }
    }

    fn lighten(&self, factor: f64) -> Color {
        let op = |c: u8| -> u8 { ((c as f64 * factor).clamp(0.0, 255.0)) as u8 };
        Color { r: op(self.r), g: op(self.g), b: op(self.b) }
    }
}

struct Theme {
    bg: String,
    fg: String,
    cursor: String,
    colors: Vec<String>,
}

fn expand_path(path_str: &str) -> PathBuf {
    if path_str.starts_with("~/") || path_str.starts_with("$HOME/") {
        if let Ok(home) = env::var("HOME") {
            let offset = if path_str.starts_with('~') { 2 } else { 6 };
            return PathBuf::from(home).join(&path_str[offset..]);
        }
    }
    PathBuf::from(path_str)
}

pub fn run_theme_process(wallpaper_path_ptr: *const c_char) -> Result<(), Box<dyn std::error::Error>> {
    if wallpaper_path_ptr.is_null() { return Err("Null path".into()); }
    let c_str = unsafe { CStr::from_ptr(wallpaper_path_ptr) };
    let wallpaper_path = c_str.to_str()?;

    // 1. Run Wallust
    let status = Command::new("wallust")
        .args(&["run", "--backend", "wal", "--quiet", wallpaper_path])
        .status()?;

    if !status.success() {
        return Err("Wallust execution failed".into());
    }

    // 2. Parse Colors
    let cache_path = expand_path("~/.cache/wallust/scriptable_colors.txt");
    let content = fs::read_to_string(cache_path)?;
    
    let mut theme = Theme {
        bg: "#000000".into(), fg: "#ffffff".into(), cursor: "#ffffff".into(),
        colors: vec!["#000000".into(); 16],
    };

    for line in content.lines() {
        if let Some((key, val)) = line.split_once('=') {
            let key = key.trim();
            let val = val.trim().trim_matches(|c| c == '\'' || c == '"');
            match key {
                "background" => theme.bg = val.into(),
                "foreground" => theme.fg = val.into(),
                "cursor" => theme.cursor = val.into(),
                k if k.starts_with("color") => {
                    if let Ok(idx) = k[5..].parse::<usize>() {
                        if idx < 16 { theme.colors[idx] = val.into(); }
                    }
                }
                _ => {}
            }
        }
    }

    // 3. Smart Accent
    let bg_col = Color::from_hex(&theme.bg).unwrap_or(Color { r:0, g:0, b:0 });
    let accent_col = Color::from_hex(&theme.colors[4]).unwrap_or(Color { r:255, g:255, b:255 });
    let white = Color { r:255, g:255, b:255 };
    let black = Color { r:0, g:0, b:0 };

    let smart_accent = if bg_col.luminance() < 128.0 {
        accent_col.mix(&white, 0.4) 
    } else {
        accent_col.mix(&black, 0.3) 
    };

    // 4. Generate CSS
    let template_path = expand_path("~/.config/aurora-shell/templates/aurora-colors-template.css");
    let dest_path = expand_path("~/.config/aurora-shell/aurora-colors.css");
    
    let mut css_content = if template_path.exists() {
        fs::read_to_string(template_path)?
    } else {
        String::new()
    };

    let replacements = vec![
        ("{{background}}", &theme.bg), ("{{foreground}}", &theme.fg),
        ("{{cursor}}", &theme.cursor), ("{{accent}}", &theme.colors[4]),
        ("%%BACKGROUND%%", &theme.bg), ("%%FOREGROUND%%", &theme.fg),
        ("%%COLOR0%%", &theme.colors[0]), ("%%COLOR4%%", &theme.colors[4]),
    ];

    for (k, v) in replacements {
        css_content = css_content.replace(k, v);
    }
    
    for i in 0..16 {
        css_content = css_content.replace(&format!("{{{{color{}}}}}", i), &theme.colors[i]);
    }

    let smart_defs = format!(
        "\n@define-color accent_color {};\n@define-color smart_accent {};\n@define-color bg_color {};\n@define-color fg_color {};\n",
        theme.colors[4], smart_accent.to_hex(), theme.bg, theme.fg
    );
    css_content.push_str(&smart_defs);
    
    if let Some(parent) = dest_path.parent() { fs::create_dir_all(parent)?; }
    fs::write(dest_path, css_content)?;

    // 5. Hyprland Config
    let hypr_path = expand_path("~/.config/hypr/colors-hyprland-generated.conf");
    let c4_raw = theme.colors[4].trim_start_matches('#');
    let c6_raw = theme.colors[6].trim_start_matches('#');
    let c0_raw = theme.colors[0].trim_start_matches('#');
    
    let hypr_conf = format!(
        "$wallust_background = {}\n$wallust_foreground = {}\n$wallust_color4 = {}\ngeneral {{\n    col.active_border = rgba({}ff) rgba({}ff) 45deg\n    col.inactive_border = rgba({}aa)\n}}\n",
        theme.bg, theme.fg, theme.colors[4], c4_raw, c6_raw, c0_raw
    );
    fs::write(hypr_path, hypr_conf).ok();

    // 6. Kitty Config
    let kitty_path = expand_path("~/.config/kitty/theme-wallust-generated.conf");
    let mut kitty_conf = format!("foreground {}\nbackground {}\ncursor {}\n", 
        Color::from_hex(&theme.fg).map(|c| c.lighten(1.5).to_hex()).unwrap_or(theme.fg.clone()), 
        theme.bg, 
        Color::from_hex(&theme.cursor).map(|c| c.lighten(1.5).to_hex()).unwrap_or(theme.cursor.clone())
    );
    
    for i in 0..16 {
        let lc = Color::from_hex(&theme.colors[i]).map(|c| c.lighten(1.5).to_hex()).unwrap_or(theme.colors[i].clone());
        kitty_conf.push_str(&format!("color{} {}\n", i, lc));
    }
    fs::write(kitty_path, kitty_conf).ok();

    // 7. Reload
    Command::new("hyprctl").arg("reload").spawn().ok();
    Command::new("pkill").args(&["-SIGUSR1", "kitty"]).spawn().ok();

    Ok(())
}