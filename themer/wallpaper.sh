#!/bin/bash
# shellcheck disable=SC2016,SC2154,SC2034

# --- Configuration ---
WALLPAPER_DIR="$HOME/Pictures/Wallpapers"

# --- swww Transition Options ---
TRANSITION_TYPE="grow"
TRANSITION_STEP="10"
TRANSITION_FPS="60"
TRANSITION_BEZIER="0.7,0.8,2,3"

# --- General File Paths ---
SCRIPT_COLORS_RAW="$HOME/.cache/wallust/scriptable_colors.txt"
LOCK_FILE="/tmp/wallpaper.lock"
NOTIFICATIONS_MUTED=false

# ===================================================================
# --- APPLICATION THEME PATHS ---
# ===================================================================
# ... (All other app paths are fine) ...
HYPR_COLORS_OUTPUT="$HOME/.config/hypr/colors-hyprland-generated.conf"
KITTY_THEME_OUTPUT="$HOME/.config/kitty/theme-wallust-generated.conf"
KITTY_LIGHTEN_FACTOR="1.5"
HELPER_SCRIPT_PATH="$(dirname "$0")/lighten_color.py"
IRONBAR_STYLE_TEMPLATE="$HOME/.config/ironbar/style-wallust-generated.css"
IRONBAR_STYLE_OUTPUT="$HOME/.config/ironbar/style.css"
SWAYNC_STYLE_BASE="$HOME/.config/swaync/style-base.css"
SWAYNC_STYLE_OUTPUT="$HOME/.config/swaync/style.css"
VICINAE_THEME_TEMPLATE="$HOME/.config/vicinae/themes/vicinae-template.json"
VICINAE_THEME_OUTPUT="$HOME/.config/vicinae/themes/wallust-generated.json"

# ===================================================================
# --- AURORA SHELL INTEGRATION PATHS ---
# ===================================================================
AURORA_CONFIG_DIR="$HOME/.config/aurora-shell"

# Template paths (where we read the base CSS from)
UPTIME_STYLE_TEMPLATE="$AURORA_CONFIG_DIR/templates/uptime/archbadge-template.css"
CALENDAR_STYLE_TEMPLATE="$AURORA_CONFIG_DIR/templates/calendar/calendar-template.css"
CHEATSHEET_STYLE_TEMPLATE="$AURORA_CONFIG_DIR/templates/cheatsheet/cheatsheet-template.css"
CC_STYLE_TEMPLATE="$AURORA_CONFIG_DIR/templates/control-center/controlcenter-template.css"
LAUNCHER_STYLE_TEMPLATE="$AURORA_CONFIG_DIR/templates/launcher/launcher-template.css"
MPRIS_PLAYER_STYLE_TEMPLATE="$AURORA_CONFIG_DIR/templates/mpris-player/mpris-player-template.css"
TOPBAR_STYLE_TEMPLATE="$AURORA_CONFIG_DIR/templates/topbar/topbar-template.css"
CACHY_STYLE_TEMPLATE="$AURORA_CONFIG_DIR/templates/cachy-selector/cachy-selector-template.css"
INSIGHT_STYLE_TEMPLATE="$AURORA_CONFIG_DIR/templates/insight/insight-template.css"

# Output paths for Aurora Shell's final CSS files
UPTIME_STYLE_OUTPUT="$AURORA_CONFIG_DIR/templates/uptime/archbadge.css"
CALENDAR_STYLE_OUTPUT="$AURORA_CONFIG_DIR/templates/calendar/calendar.css"
CHEATSHEET_STYLE_OUTPUT="$AURORA_CONFIG_DIR/templates/cheatsheet/cheatsheet.css"
CC_STYLE_OUTPUT="$AURORA_CONFIG_DIR/templates/control-center/controlcenter.css"
LAUNCHER_STYLE_OUTPUT="$AURORA_CONFIG_DIR/templates/launcher/launcher.css"
MPRIS_PLAYER_STYLE_OUTPUT="$AURORA_CONFIG_DIR/templates/mpris-player/mpris-player.css"
TOPBAR_STYLE_OUTPUT="$AURORA_CONFIG_DIR/templates/topbar/topbar.css"
CACHY_STYLE_OUTPUT="$AURORA_CONFIG_DIR/templates/cachy-selector/cachy-selector.css"
INSIGHT_STYLE_OUTPUT="$AURORA_CONFIG_DIR/templates/insight/insight.css" # <-- CORRECTED
# ===================================================================

# --- Helper Functions ---

send_notification() {
    if [ "$NOTIFICATIONS_MUTED" = "false" ]; then
        notify-send "$@"
    fi
}

hex_to_rgba() {
    local hex=${1#\#}
    local alpha=$2
    local r=$((16#${hex:0:2}))
    local g=$((16#${hex:2:2}))
    local b=$((16#${hex:4:2}))
    echo "rgba($r, $g, $b, $alpha)"
}

theme_generic_css() {
    local template_file="$1"
    local output_file="$2"
    local component_name="$3"

    if [ ! -f "$template_file" ]; then
        echo "Warning: Template for $component_name not found at '$template_file'. Skipping."
        return
    fi

    echo "Generating CSS/JSON for $component_name..."
    local tmp_file
    tmp_file=$(mktemp)
    cp "$template_file" "$tmp_file"

    # Replace all placeholders consistently
    sed -i \
        -e "s/{{background}}/$background/g" \
        -e "s/{{foreground}}/$foreground/g" \
        -e "s/{{cursor}}/$cursor/g" \
        -e "s/{{color0}}/$color0/g" -e "s/{{color1}}/$color1/g" \
        -e "s/{{color2}}/$color2/g" -e "s/{{color3}}/$color3/g" \
        -e "s/{{color4}}/$color4/g" -e "s/{{color5}}/$color5/g" \
        -e "s/{{color6}}/$color6/g" -e "s/{{color7}}/$color7/g" \
        -e "s/{{color8}}/$color8/g" -e "s/{{color9}}/$color9/g" \
        -e "s/{{color10}}/$color10/g" -e "s/{{color11}}/$color11/g" \
        -e "s/{{color12}}/$color12/g" -e "s/{{color13}}/$color13/g" \
        -e "s/{{color14}}/$color14/g" -e "s/{{color15}}/$color15/g" \
        -e "s/{{accent}}/$color4/g" \
        -e "s/{{surface}}/$color0/g" \
        "$tmp_file"

    mv "$tmp_file" "$output_file"
}

# --- Main Theming and Reloading Function ---
apply_theme_and_reload() {
    local SELECTED_NEW_WALLPAPER_PATH="$1"

    if [ -z "$SELECTED_NEW_WALLPAPER_PATH" ] || [ ! -f "$SELECTED_NEW_WALLPAPER_PATH" ]; then
        send_notification -u critical "Wallpaper Script Error" "Invalid wallpaper path provided."
        return 1
    fi

    # 1. Set Wallpaper and Generate Color Palette
    send_notification -u low "🎨 Generating Palette" "Setting wallpaper and extracting colors..."
    local random_pos
    random_pos="$(awk -v seed=$RANDOM 'BEGIN{srand(seed); printf "%.2f,%.2f\n", rand(), rand()}')"
    swww img "$SELECTED_NEW_WALLPAPER_PATH" \
        --transition-type "$TRANSITION_TYPE" --transition-fps "$TRANSITION_FPS" \
        --transition-step "$TRANSITION_STEP" --transition-pos "$random_pos" \
        ${TRANSITION_BEZIER:+--transition-bezier "$TRANSITION_BEZIER"}

    wallust run --backend wal "$SELECTED_NEW_WALLPAPER_PATH"
    if [ $? -ne 0 ]; then send_notification -u critical "Wallust Error"; return 1; fi

    # 2. Source the new colors
    if [ ! -f "$SCRIPT_COLORS_RAW" ]; then
        send_notification -u critical "Themer Error" "Wallust did not generate colors."
        return 1
    fi
    set +u
    # shellcheck source=/dev/null
    source "$SCRIPT_COLORS_RAW"
    set -u

    send_notification -u low "🖌️ Applying Themes..."

    # 3. Theme Aurora Shell Widgets
    theme_generic_css "$UPTIME_STYLE_TEMPLATE" "$UPTIME_STYLE_OUTPUT" "Uptime"
    theme_generic_css "$CALENDAR_STYLE_TEMPLATE" "$CALENDAR_STYLE_OUTPUT" "Calendar"
    theme_generic_css "$CHEATSHEET_STYLE_TEMPLATE" "$CHEATSHEET_STYLE_OUTPUT" "Cheatsheet"
    theme_generic_css "$CC_STYLE_TEMPLATE" "$CC_STYLE_OUTPUT" "Control Center"
    theme_generic_css "$LAUNCHER_STYLE_TEMPLATE" "$LAUNCHER_STYLE_OUTPUT" "Launcher"
    theme_generic_css "$MPRIS_PLAYER_STYLE_TEMPLATE" "$MPRIS_PLAYER_STYLE_OUTPUT" "MPRIS Player"
    theme_generic_css "$TOPBAR_STYLE_TEMPLATE" "$TOPBAR_STYLE_OUTPUT" "Topbar"
    theme_generic_css "$CACHY_STYLE_TEMPLATE" "$CACHY_STYLE_OUTPUT" "Cachy Selector"
    theme_generic_css "$INSIGHT_STYLE_TEMPLATE" "$INSIGHT_STYLE_OUTPUT" "Insight" # <-- ADDED

    # 4. Theme Other Applications
    # ... (Rest of script is fine, no changes needed below this line) ...
    # --- Hyprland ---
    echo "Generating Hyprland colors..."
    local active_border_col1_hex=${color4#\#}
    local active_border_col2_hex=${color6#\#}
    local inactive_border_col_hex=${color0#\#}
    cat > "$HYPR_COLORS_OUTPUT" << EOF
# Hyprland colors generated by wallpaper.sh
\$wallust_background = $background
\$wallust_foreground = $foreground
\$wallust_cursor = $cursor
\$wallust_color0 = $color0; \$wallust_color1 = $color1; \$wallust_color2 = $color2; \$wallust_color3 = $color3
\$wallust_color4 = $color4; \$wallust_color5 = $color5; \$wallust_color6 = $color6; \$wallust_color7 = $color7
\$wallust_color8 = $color8; \$wallust_color9 = $color9; \$wallust_color10 = $color10; \$wallust_color11 = $color11
\$wallust_color12 = $color12; \$wallust_color13 = $color13; \$wallust_color14 = $color14; \$wallust_color15 = $color15
general {
    col.active_border = rgba(${active_border_col1_hex}ff) rgba(${active_border_col2_hex}ff) 45deg
    col.inactive_border = rgba(${inactive_border_col_hex}aa)
}
EOF

    # --- Kitty (Lighter Variant) ---
    if [ -x "$HELPER_SCRIPT_PATH" ]; then
        echo "Generating lighter Kitty theme..."
        cat > "$KITTY_THEME_OUTPUT" << EOF
# Kitty colors generated by wallpaper.sh (Lighter Variant)
foreground          $("$HELPER_SCRIPT_PATH" "$foreground" "$KITTY_LIGHTEN_FACTOR")
cursor              $("$HELPER_SCRIPT_PATH" "$cursor" "$KITTY_LIGHTEN_FACTOR")
color8              $("$HELPER_SCRIPT_PATH" "$color8" "$KITTY_LIGHTEN_FACTOR")
color1              $("$HELPER_SCRIPT_PATH" "$color1" "$KITTY_LIGHTEN_FACTOR")
color9              $("$HELPER_SCRIPT_PATH" "$color9" "$KITTY_LIGHTEN_FACTOR")
color2              $("$HELPER_SCRIPT_PATH" "$color2" "$KITTY_LIGHTEN_FACTOR")
color10             $("$HELPER_SCRIPT_PATH" "$color10" "$KITTY_LIGHTEN_FACTOR")
color3              $("$HELPER_SCRIPT_PATH" "$color3" "$KITTY_LIGHTEN_FACTOR")
color11             $("$HELPER_SCRIPT_PATH" "$color11" "$KITTY_LIGHTEN_FACTOR")
color4              $("$HELPER_SCRIPT_PATH" "$color4" "$KITTY_LIGHTEN_FACTOR")
color12             $("$HELPER_SCRIPT_PATH" "$color12" "$KITTY_LIGHTEN_FACTOR")
color5              $("$HELPER_SCRIPT_PATH" "$color5" "$KITTY_LIGHTEN_FACTOR")
color13             $("$HELPER_SCRIPT_PATH" "$color13" "$KITTY_LIGHTEN_FACTOR")
color6              $("$HELPER_SCRIPT_PATH" "$color6" "$KITTY_LIGHTEN_FACTOR")
color14             $("$HELPER_SCRIPT_PATH" "$color14" "$KITTY_LIGHTEN_FACTOR")
color7              $("$HELPER_SCRIPT_PATH" "$color7" "$KITTY_LIGHTEN_FACTOR")
color15             $("$HELPER_SCRIPT_PATH" "$color15" "$KITTY_LIGHTEN_FACTOR")
EOF
    fi

    # --- Ironbar and Vicinae (use the generic theming function) ---
    theme_generic_css "$IRONBAR_STYLE_TEMPLATE" "$IRONBAR_STYLE_OUTPUT" "Ironbar"
    theme_generic_css "$VICINAE_THEME_TEMPLATE" "$VICINAE_THEME_OUTPUT" "Vicinae"

    # --- SwayNC (special handling for RGBA values) ---
    if [ -f "$SWAYNC_STYLE_BASE" ]; then
        echo "Generating SwayNC CSS..."
        local tmp_swaync_css
        tmp_swaync_css=$(mktemp)
        cp "$SWAYNC_STYLE_BASE" "$tmp_swaync_css"
        sed -i "s|%%BACKGROUND%%|$background|g" "$tmp_swaync_css"
        sed -i "s|%%FOREGROUND%%|$foreground|g" "$tmp_swaync_css"
        sed -i "s|%%COLOR0%%|$color0|g" "$tmp_swaync_css"
        sed -i "s|%%COLOR1%%|$color1|g" "$tmp_swaync_css"
        sed -i "s|%%COLOR4%%|$color4|g" "$tmp_swaync_css"
        sed -i "s|%%COLOR7%%|$color7|g" "$tmp_swaync_css"
        sed -i "s|%%COLOR15%%|$color15|g" "$tmp_swaync_css"
        sed -i "s|%%BACKGROUND_RGBA_30%%|$(hex_to_rgba "$background" "0.3")|g" "$tmp_swaync_css"
        sed -i "s|%%BACKGROUND_RGBA_60%%|$(hex_to_rgba "$background" "0.6")|g" "$tmp_swaync_css"
        sed -i "s|%%BACKGROUND_RGBA_90%%|$(hex_to_rgba "$background" "0.9")|g" "$tmp_swaync_css"
        sed -i "s|%%COLOR0_RGBA_30%%|$(hex_to_rgba "$color0" "0.3")|g" "$tmp_swaync_css"
        sed -i "s|%%COLOR0_RGBA_50%%|$(hex_to_rgba "$color0" "0.5")|g" "$tmp_swaync_css"
        sed -i "s|%%COLOR1_RGBA_50%%|$(hex_to_rgba "$color1" "0.5")|g" "$tmp_swaync_css"
        sed -i "s|%%COLOR4_RGBA_50%%|$(hex_to_rgba "$color4" "0.5")|g" "$tmp_swaync_css"
        sed -i "s|%%COLOR7_RGBA_50%%|$(hex_to_rgba "$color7" "0.5")|g" "$tmp_swaync_css"
        sed -i "s|%%COLOR8_RGBA_50%%|$(hex_to_rgba "$color8" "0.5")|g" "$tmp_swaync_css"
        sed -i "s|%%COLOR12_RGBA_50%%|$(hex_to_rgba "$color12" "0.5")|g" "$tmp_swaync_css"
        sed -i "s|%%COLOR13_RGBA_50%%|$(hex_to_rgba "$color13" "0.5")|g" "$tmp_swaync_css"
        sed -i "s|%%COLOR14_RGBA_50%%|$(hex_to_rgba "$color14" "0.5")|g" "$tmp_swaync_css"
        mv "$tmp_swaync_css" "$SWAYNC_STYLE_OUTPUT"
    fi

    # 5. Reload Services
    echo "Reloading applications..."
    hyprctl reload > /dev/null 2>&1 &
    if pgrep -x "swaync" > /dev/null; then swaync-client -rs > /dev/null 2>&1; echo "Reloaded SwayNC style."; fi

    send_notification -i "$SELECTED_NEW_WALLPAPER_PATH" "✅ Theme Applied" "Your new desktop theme is now active!"
    return 0
}

# --- Function to ensure swww-daemon is running ---
ensure_swww_daemon() {
    if ! pgrep -x "swww-daemon" > /dev/null; then
        echo "swww-daemon not running, attempting to initialize..."
        swww init >/dev/null 2>&1 && sleep 0.5
        if ! pgrep -x "swww-daemon" > /dev/null; then return 1; fi
        echo "swww-daemon initialized."
    fi
    return 0
}

# ===================================================================
# --- SCRIPT EXECUTION STARTS HERE ---
# ===================================================================

for arg in "$@"; do
    if [[ "$arg" == "--mute" ]]; then
        NOTIFICATIONS_MUTED=true
        echo "Notifications are muted."
    fi
done
trap 'rm -f "$LOCK_FILE"' EXIT
if [ -e "$LOCK_FILE" ]; then
    echo "Script is already running. Exiting."
    exit 1
else
    touch "$LOCK_FILE"
fi
if [[ "$1" == "--startup" ]]; then
    echo "Startup mode: Ensuring swww-daemon is running."
    if ! ensure_swww_daemon; then
        echo "Error: Failed to start swww-daemon on startup." >&2; exit 1;
    fi
    echo "swww-daemon is running. Startup script finished."; exit 0;
fi
if ! ensure_swww_daemon; then
    send_notification -u critical "SWWW Error" "Failed to start swww-daemon."; exit 1;
fi
if [ ! -d "$WALLPAPER_DIR" ]; then
    send_notification -u critical "Error" "Directory '$WALLPAPER_DIR' not found."; exit 1;
fi
WALLPAPER_FILES=$(find "$WALLPAPER_DIR" -type f \( -iname '*.png' -o -iname '*.jpeg' -o -iname '*.jpg' -o -iname '*.gif' -o -iname '*.webp' \) | sort)
if [ -z "$WALLPAPER_FILES" ]; then
    send_notification "Error" "No image files found in $WALLPAPER_DIR."; exit 1;
fi

if [[ " $@ " =~ " --next " ]] || [[ " $@ " =~ " --prev " ]]; then
    echo "Cycling mode..."
    mapfile -t wallpaper_array < <(echo "$WALLPAPER_FILES")
    count=${#wallpaper_array[@]}
    current_wallpaper=$(swww query | head -n 1 | sed 's/.*: //')
    current_index=-1
    for i in "${!wallpaper_array[@]}"; do
        if [[ "${wallpaper_array[$i]}" == "$current_wallpaper" ]]; then current_index=$i; break; fi
    done
    if [[ $current_index -eq -1 ]]; then current_index=0; fi
    if [[ " $@ " =~ " --next " ]]; then
        new_index=$(( (current_index + 1) % count ))
    else # --prev
        new_index=$(( (current_index - 1 + count) % count ))
    fi
    SELECTED_NEW_WALLPAPER_PATH="${wallpaper_array[$new_index]}"
    apply_theme_and_reload "$SELECTED_NEW_WALLPAPER_PATH"
    exit $?
fi

echo "Interactive mode: Launching cachy-selector..."
SELECTOR_EXECUTABLE="cachy-selector" 
if ! command -v "$SELECTOR_EXECUTABLE" &> /dev/null; then
    send_notification -u critical "Error" "'$SELECTOR_EXECUTABLE' not found in PATH."
    exit 1
fi
SELECTED_NEW_WALLPAPER_PATH=$(echo "$WALLPAPER_FILES" | "$SELECTOR_EXECUTABLE" "cachy-selector")
if [ -z "$SELECTED_NEW_WALLPAPER_PATH" ]; then
    echo "No wallpaper selected. Exiting."
    exit 0
fi
apply_theme_and_reload "$SELECTED_NEW_WALLPAPER_PATH"
exit $?