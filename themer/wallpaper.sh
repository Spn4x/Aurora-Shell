#!/bin/bash

# --- Configuration ---
WALLPAPER_DIR="$HOME/Pictures/Wallpapers"

# --- swww Transition Options ---
TRANSITION_TYPE="grow"
TRANSITION_STEP=10
TRANSITION_FPS=60

# ===================================================================
# --- FILE PATHS FOR AURORA SHELL INTEGRATION (CORRECTED) ---
# ===================================================================
AURORA_CONFIG_DIR="$HOME/.config/aurora-shell"
SCRIPT_COLORS_RAW="$HOME/.cache/wallust/scriptable_colors.txt"

# --- Output paths for Aurora Shell's final CSS files ---
UPTIME_STYLE_OUTPUT="$AURORA_CONFIG_DIR/templates/uptime/archbadge.css"
CALENDAR_STYLE_OUTPUT="$AURORA_CONFIG_DIR/templates/calendar/calendar.css"
CHEATSHEET_STYLE_OUTPUT="$AURORA_CONFIG_DIR/templates/cheatsheet/cheatsheet.css"
CC_STYLE_OUTPUT="$AURORA_CONFIG_DIR/templates/control-center/controlcenter.css"
LAUNCHER_STYLE_OUTPUT="$AURORA_CONFIG_DIR/templates/launcher/launcher.css"
MPRIS_PLAYER_STYLE_OUTPUT="$AURORA_CONFIG_DIR/templates/mpris-player/mpris-player.css"
TOPBAR_STYLE_OUTPUT="$AURORA_CONFIG_DIR/templates/topbar/topbar.css"
CACHY_STYLE_OUTPUT="$AURORA_CONFIG_DIR/templates/cachy-selector/cachy-selector.css"

# --- Template paths (where we read the base CSS from) ---
UPTIME_STYLE_TEMPLATE="$AURORA_CONFIG_DIR/templates/uptime/archbadge-template.css"
CALENDAR_STYLE_TEMPLATE="$AURORA_CONFIG_DIR/templates/calendar/calendar-template.css"
CHEATSHEET_STYLE_TEMPLATE="$AURORA_CONFIG_DIR/templates/cheatsheet/cheatsheet-template.css"
CC_STYLE_TEMPLATE="$AURORA_CONFIG_DIR/templates/control-center/controlcenter-template.css"
LAUNCHER_STYLE_TEMPLATE="$AURORA_CONFIG_DIR/templates/launcher/launcher-template.css"
MPRIS_PLAYER_STYLE_TEMPLATE="$AURORA_CONFIG_DIR/templates/mpris-player/mpris-player-template.css"
TOPBAR_STYLE_TEMPLATE="$AURORA_CONFIG_DIR/templates/topbar/topbar-template.css"
CACHY_STYLE_TEMPLATE="$AURORA_CONFIG_DIR/templates/cachy-selector/cachy-selector-template.css"
# ===================================================================

# --- Lock File ---
LOCK_FILE="/tmp/wallpaper.lock"

# --- Theme Application Function ---
apply_theme_and_reload() {
    local SELECTED_NEW_WALLPAPER_PATH="$1"

    # 1. Set Wallpaper and Generate Palette
    if [ -z "$SELECTED_NEW_WALLPAPER_PATH" ] || [ ! -f "$SELECTED_NEW_WALLPAPER_PATH" ]; then
        notify-send -u critical "Themer Error" "Invalid wallpaper path."
        return 1
    fi
    local random_pos="$(awk -v seed=$RANDOM 'BEGIN{srand(seed); printf "%.2f,%.2f\n", rand(), rand()}')"
    swww img "$SELECTED_NEW_WALLPAPER_PATH" --transition-type "$TRANSITION_TYPE" --transition-fps "$TRANSITION_FPS" --transition-step "$TRANSITION_STEP" --transition-pos "$random_pos"
    wallust run "$SELECTED_NEW_WALLPAPER_PATH"
    
    # 2. Source the new colors
    if [ ! -f "$SCRIPT_COLORS_RAW" ]; then
        notify-send -u critical "Themer Error" "Wallust did not generate colors."
        return 1
    fi
    set +u
    source "$SCRIPT_COLORS_RAW"
    set -u
    notify-send -u low "🎨 Applying Theme to Aurora Shell..."

    # 3. Helper function to theme a widget's CSS
    theme_aurora_widget() {
        local template_file="$1"
        local output_file="$2"
        local widget_name="$3"

        if [ ! -f "$template_file" ]; then
            echo "Warning: Template for $widget_name not found at '$template_file'. Skipping."
            return
        fi

        echo "Generating CSS for $widget_name..."
        local tmp_css=$(mktemp)
        cp "$template_file" "$tmp_css"
        
        # Replace all placeholders
        sed -i "s/{{color0}}/$color0/g" "$tmp_css"
        sed -i "s/{{color1}}/$color1/g" "$tmp_css"
        sed -i "s/{{color2}}/$color2/g" "$tmp_css"
        sed -i "s/{{color3}}/$color3/g" "$tmp_css"
        sed -i "s/{{color4}}/$color4/g" "$tmp_css"
        sed -i "s/{{color5}}/$color5/g" "$tmp_css"
        sed -i "s/{{color6}}/$color6/g" "$tmp_css"
        sed -i "s/{{color7}}/$color7/g" "$tmp_css"
        sed -i "s/{{color8}}/$color8/g" "$tmp_css"
        sed -i "s/{{color9}}/$color9/g" "$tmp_css"
        sed -i "s/{{color10}}/$color10/g" "$tmp_css"
        sed -i "s/{{color11}}/$color11/g" "$tmp_css"
        sed -i "s/{{color12}}/$color12/g" "$tmp_css"
        sed -i "s/{{color13}}/$color13/g" "$tmp_css"
        sed -i "s/{{color14}}/$color14/g" "$tmp_css"
        sed -i "s/{{color15}}/$color15/g" "$tmp_css"
        sed -i "s/{{background}}/$background/g" "$tmp_css"
        sed -i "s/{{foreground}}/$foreground/g" "$tmp_css"
        sed -i "s/{{accent}}/$color4/g" "$tmp_css"
        sed -i "s/{{surface}}/$color0/g" "$tmp_css"

        mv "$tmp_css" "$output_file"
    }

    # 4. Theme all Aurora widgets that have templates
    theme_aurora_widget "$UPTIME_STYLE_TEMPLATE" "$UPTIME_STYLE_OUTPUT" "Uptime"
    theme_aurora_widget "$CALENDAR_STYLE_TEMPLATE" "$CALENDAR_STYLE_OUTPUT" "Calendar"
    theme_aurora_widget "$CHEATSHEET_STYLE_TEMPLATE" "$CHEATSHEET_STYLE_OUTPUT" "Cheatsheet"
    theme_aurora_widget "$CC_STYLE_TEMPLATE" "$CC_STYLE_OUTPUT" "Control Center"
    theme_aurora_widget "$LAUNCHER_STYLE_TEMPLATE" "$LAUNCHER_STYLE_OUTPUT" "Launcher"
    theme_aurora_widget "$MPRIS_PLAYER_STYLE_TEMPLATE" "$MPRIS_PLAYER_STYLE_OUTPUT" "MPRIS Player"
    theme_aurora_widget "$TOPBAR_STYLE_TEMPLATE" "$TOPBAR_STYLE_OUTPUT" "Topbar"
    theme_aurora_widget "$CACHY_STYLE_TEMPLATE" "$CACHY_STYLE_OUTPUT" "Cachy Selector"

    notify-send -i "$SELECTED_NEW_WALLPAPER_PATH" "✅ Aurora Theme Applied"
    return 0
}

# --- Function to ensure swww-daemon is running ---
ensure_swww_daemon() {
    if ! pgrep -x "swww-daemon" > /dev/null; then
        swww init >/dev/null 2>&1 && sleep 0.5
        if ! pgrep -x "swww-daemon" > /dev/null; then return 1; fi
    fi
    return 0
}

# ===================================================================
# --- SCRIPT EXECUTION STARTS HERE ---
# ===================================================================
trap 'rm -f "$LOCK_FILE"' EXIT
if [ -e "$LOCK_FILE" ]; then exit 1; else touch "$LOCK_FILE"; fi

if ! ensure_swww_daemon; then
    notify-send -u critical "Themer Error" "Failed to start swww-daemon."
    exit 1
fi

if [ ! -d "$WALLPAPER_DIR" ]; then
    notify-send -u critical "Themer Error" "Wallpaper directory '$WALLPAPER_DIR' not found."
    exit 1
fi

WALLPAPER_FILES=$(find "$WALLPAPER_DIR" -type f \( -iname '*.png' -o -iname '*.jpeg' -o -iname '*.jpg' -o -iname '*.gif' -o -iname '*.webp' \) | sort)

if [ -z "$WALLPAPER_FILES" ]; then
    notify-send "Themer Error" "No images found in $WALLPAPER_DIR."
    exit 1
fi

# --- Interactive Mode ---
echo "Interactive mode: Launching cachy-selector..."
SELECTOR_PATH="/usr/local/bin/cachy-selector"

if [ ! -x "$SELECTOR_PATH" ]; then
    notify-send -u critical "Themer Error" "cachy-selector not found at $SELECTOR_PATH"
    exit 1
fi

# The selector is now called with the correct config name "cachy-selector"
SELECTED_NEW_WALLPAPER_PATH=$(echo "$WALLPAPER_FILES" | "$SELECTOR_PATH" "cachy-selector")

if [ -z "$SELECTED_NEW_WALLPAPER_PATH" ]; then
    echo "No wallpaper selected. Exiting."
    exit 0
fi

apply_theme_and_reload "$SELECTED_NEW_WALLPAPER_PATH"
exit $?