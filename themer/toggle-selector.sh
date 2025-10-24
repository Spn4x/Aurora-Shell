#!/bin/bash
#
# toggle-selector.sh - Smartly toggles the cachy-selector UI AND applies
# the theme if a wallpaper is chosen.
#

readonly PROCESS_NAME="cachy-selector"
readonly WALLPAPER_DIR="$HOME/Pictures/Wallpapers"

# Check if the process is running.
if pgrep -x "$PROCESS_NAME" > /dev/null; then
    # If yes, just kill it. This is the "Escape" or "toggle off" case.
    pkill -x "$PROCESS_NAME"
else
    # If no, launch it and CAPTURE the selected wallpaper path.
    # The 'nohup' and redirection is removed because we now NEED the output.
    
    selected_wallpaper=$(find "$WALLPAPER_DIR" -type f \( -iname '*.png' -o -iname '*.jpeg' -o -iname '*.jpg' -o -iname '*.gif' -o -iname '*.webp' \) | sort | "$PROCESS_NAME" "cachy-selector")
    
    # Check if the user actually selected a wallpaper (the output is not empty).
    if [[ -n "$selected_wallpaper" ]]; then
        # If a wallpaper was selected, run the main theming script with it.
        # We run this in the background to not block the shell.
        wallpaper.sh "$selected_wallpaper" &
    fi
fi