#!/bin/bash
#
# toggle-selector.sh - Smartly toggles the cachy-selector UI AND applies
# the theme if a wallpaper is chosen.
#
# This script is now aware of the Aurora Shell configuration file.
#

readonly PROCESS_NAME="cachy-selector"
readonly AURORA_CONFIG_FILE="$HOME/.config/aurora-shell/config.json"
readonly WALLPAPER_DIR_DEFAULT="$HOME/Pictures/Wallpapers"

# Function to get the wallpaper directory from the main config file.
get_wallpaper_dir() {
    if [[ ! -f "$AURORA_CONFIG_FILE" ]] || ! command -v jq &>/dev/null; then
        echo "$WALLPAPER_DIR_DEFAULT"
        return
    fi

    local config_dir
    config_dir=$(jq -r '.[] | select(.type == "themer-config") | .wallpaper_dir' "$AURORA_CONFIG_FILE")

    if [[ -z "$config_dir" || "$config_dir" == "null" ]]; then
        echo "$WALLPAPER_DIR_DEFAULT"
    else
        # Safely expand the tilde (~)
        eval echo "$config_dir"
    fi
}

# Check if the process is running.
if pgrep -x "$PROCESS_NAME" > /dev/null; then
    # If yes, just kill it. This is the "Escape" or "toggle off" case.
    pkill -x "$PROCESS_NAME"
else
    # If no, determine the correct wallpaper directory and launch the selector.
    WALLPAPER_DIR=$(get_wallpaper_dir)

    if [[ ! -d "$WALLPAPER_DIR" ]]; then
        notify-send -u critical "Aurora Themer Error" "Wallpaper directory not found: '$WALLPAPER_DIR'"
        exit 1
    fi
    
    selected_wallpaper=$(find "$WALLPAPER_DIR" -type f \( -iname '*.png' -o -iname '*.jpeg' -o -iname '*.jpg' -o -iname '*.gif' -o -iname '*.webp' \) | sort | "$PROCESS_NAME" "cachy-selector")
    
    # Check if the user actually selected a wallpaper (the output is not empty).
    if [[ -n "$selected_wallpaper" ]]; then
        # If a wallpaper was selected, run the main theming script with it.
        # We run this in the background to not block the shell.
        wallpaper.sh "$selected_wallpaper" &
    fi
fi