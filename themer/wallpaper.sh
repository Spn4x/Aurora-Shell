#!/bin/bash
#
# wallpaper.sh - A comprehensive wallpaper and theming script for Hyprland.
# Optimized to use a global color file architecture.
#

set -o pipefail

# ===================================================================
# --- SCRIPT CONFIGURATION ---
# ===================================================================
readonly WALLPAPER_DIR_DEFAULT="$HOME/Pictures/Wallpapers"
WALLPAPER_DIR=""
readonly LOCK_FILE="/tmp/wallpaper.lock"

# --- SWWW TRANSITION OPTIONS ---
readonly TRANSITION_TYPE="grow"
readonly TRANSITION_STEP="10"
readonly TRANSITION_FPS="60"
readonly TRANSITION_BEZIER="0.7,0.8,2,3"

# --- FILE PATHS ---
readonly AURORA_CONFIG_FILE="$HOME/.config/aurora-shell/config.json"
readonly SCRIPT_COLORS_RAW="$HOME/.cache/wallust/scriptable_colors.txt"
readonly HYPR_COLORS_OUTPUT="$HOME/.config/hypr/colors-hyprland-generated.conf"
readonly KITTY_THEME_OUTPUT="$HOME/.config/kitty/theme-wallust-generated.conf"
readonly IRONBAR_STYLE_TEMPLATE="$HOME/.config/ironbar/style-wallust-generated.css"
readonly IRONBAR_STYLE_OUTPUT="$HOME/.config/ironbar/style.css"
readonly SWAYNC_STYLE_BASE="$HOME/.config/swaync/style-base.css"
readonly SWAYNC_STYLE_OUTPUT="$HOME/.config/swaync/style.css"
readonly VICINAE_THEME_TEMPLATE="$HOME/.config/vicinae/themes/vicinae-template.json"
readonly VICINAE_THEME_OUTPUT="$HOME/.config/vicinae/themes/wallust-generated.json"
readonly HELPER_SCRIPT_PATH="$(dirname "$0")/lighten_color.py"
readonly KITTY_LIGHTEN_FACTOR="1.5"

# --- AURORA SHELL CONFIGURATION ---
readonly AURORA_CONFIG_DIR="$HOME/.config/aurora-shell"

# NEW: Only one global template is needed for Aurora Shell
readonly GLOBAL_COLOR_TEMPLATE="$AURORA_CONFIG_DIR/templates/aurora-colors-template.css"
readonly GLOBAL_COLOR_OUTPUT="$AURORA_CONFIG_DIR/aurora-colors.css"

# --- GLOBAL STATE ---
NOTIFICATIONS_MUTED=false

# ===================================================================
# --- HELPER FUNCTIONS ---
# ===================================================================

log_info() { echo "INFO: $*"; }
log_warn() { echo "WARN: $*" >&2; }
log_error() { echo "ERROR: $*" >&2; }

send_notification() {
    if [[ "$NOTIFICATIONS_MUTED" == "false" ]]; then
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

get_wallpaper_dir() {
    if [[ ! -f "$AURORA_CONFIG_FILE" ]]; then
        echo "$WALLPAPER_DIR_DEFAULT"
        return
    fi
    local config_dir
    config_dir=$(jq -r '.[] | select(.type == "themer-config") | .wallpaper_dir' "$AURORA_CONFIG_FILE")
    if [[ -z "$config_dir" || "$config_dir" == "null" ]]; then
        echo "$WALLPAPER_DIR_DEFAULT"
        return
    fi
    eval echo "$config_dir"
}

check_dependencies() {
    for cmd in swww wallust cachy-selector notify-send jq; do
        if ! command -v "$cmd" &>/dev/null; then
            log_error "Required command '$cmd' not found in PATH. Please install it."
            exit 1
        fi
    done
}

# ===================================================================
# --- THEMING LOGIC ---
# ===================================================================

set_wallpaper_and_generate_colors() {
    local wallpaper_path="$1"
    log_info "Setting wallpaper: $wallpaper_path"
    
    local random_pos
    random_pos="$(awk -v seed=$RANDOM 'BEGIN{srand(seed); printf "%.2f,%.2f\n", rand(), rand()}')"
    
    swww img "$wallpaper_path" \
        --transition-type "$TRANSITION_TYPE" \
        --transition-fps "$TRANSITION_FPS" \
        --transition-step "$TRANSITION_STEP" \
        --transition-pos "$random_pos" \
        ${TRANSITION_BEZIER:+--transition-bezier "$TRANSITION_BEZIER"}

    log_info "Generating color palette with wallust..."
    mkdir -p "$(dirname "$SCRIPT_COLORS_RAW")"
    wallust run --backend wal --quiet "$wallpaper_path"
    if [[ $? -ne 0 ]] || [[ ! -f "$SCRIPT_COLORS_RAW" ]]; then
        log_error "Wallust failed to generate colors."
        send_notification -u critical "Themer Error" "Failed to generate color palette."
        return 1
    fi
    return 0
}

source_colors() {
    set +u
    source "$SCRIPT_COLORS_RAW"
    set -u
    if [[ -z "$background" || -z "$color4" ]]; then
        log_error "Failed to source color variables."
        return 1
    fi
    return 0
}

theme_generic_template() {
    local template_file="$1"
    local output_file="$2"
    local component_name="$3"

    if [[ ! -f "$template_file" ]]; then
        log_warn "Template for $component_name not found at '$template_file'. Skipping."
        return
    fi

    mkdir -p "$(dirname "$output_file")"
    local tmp_file
    tmp_file=$(mktemp)
    
    set +u
    sed -e "s/{{background}}/$background/g" \
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
        "$template_file" > "$tmp_file"
    set -u
    
    if ! cmp -s "$tmp_file" "$output_file"; then
        mv "$tmp_file" "$output_file"
    else
        rm "$tmp_file"
    fi
}

# --- NEW FUNCTION: Only updates the global color file ---
theme_aurora_shell() {
    log_info "Updating global Aurora colors..."
    theme_generic_template "$GLOBAL_COLOR_TEMPLATE" "$GLOBAL_COLOR_OUTPUT" "Global Colors"
}

theme_system_apps() {
    log_info "Theming system applications..."
    set +u
    # Hyprland
    mkdir -p "$(dirname "$HYPR_COLORS_OUTPUT")"
    local active_border_col1_hex=${color4#\#}
    local active_border_col2_hex=${color6#\#}
    local inactive_border_col_hex=${color0#\#}
    cat > "$HYPR_COLORS_OUTPUT" << EOF
# Hyprland colors generated by wallpaper.sh
\$wallust_background = $background
\$wallust_foreground = $foreground
\$wallust_color0 = $color0; \$wallust_color1 = $color1;
\$wallust_color4 = $color4; \$wallust_color6 = $color6;
general {
    col.active_border = rgba(${active_border_col1_hex}ff) rgba(${active_border_col2_hex}ff) 45deg
    col.inactive_border = rgba(${inactive_border_col_hex}aa)
}
EOF

    # Kitty
    if [[ -x "$HELPER_SCRIPT_PATH" ]]; then
        mkdir -p "$(dirname "$KITTY_THEME_OUTPUT")"
        {
            echo "# Kitty colors generated by wallpaper.sh"
            echo "foreground          $("$HELPER_SCRIPT_PATH" "$foreground" "$KITTY_LIGHTEN_FACTOR")"
            echo "cursor              $("$HELPER_SCRIPT_PATH" "$cursor" "$KITTY_LIGHTEN_FACTOR")"
            for i in {0..15}; do
                eval "color_val=\$color$i"
                echo "color$i              \$(\"$HELPER_SCRIPT_PATH\" \"$color_val\" \"$KITTY_LIGHTEN_FACTOR\")"
            done
        } > "$KITTY_THEME_OUTPUT"
    fi

    # SwayNC
    if [[ -f "$SWAYNC_STYLE_BASE" ]]; then
        mkdir -p "$(dirname "$SWAYNC_STYLE_OUTPUT")"
        local tmp_swaync_css
        tmp_swaync_css=$(mktemp)
        sed -e "s|%%BACKGROUND%%|$background|g" \
            -e "s|%%FOREGROUND%%|$foreground|g" \
            -e "s|%%COLOR0%%|$color0|g" \
            -e "s|%%COLOR4%%|$color4|g" \
            -e "s|%%BACKGROUND_RGBA_60%%|$(hex_to_rgba "$background" "0.6")|g" \
            -e "s|%%COLOR0_RGBA_50%%|$(hex_to_rgba "$color0" "0.5")|g" \
            "$SWAYNC_STYLE_BASE" > "$tmp_swaync_css"
        mv "$tmp_swaync_css" "$SWAYNC_STYLE_OUTPUT"
    fi
    set -u

    theme_generic_template "$IRONBAR_STYLE_TEMPLATE" "$IRONBAR_STYLE_OUTPUT" "Ironbar"
    theme_generic_template "$VICINAE_THEME_TEMPLATE" "$VICINAE_THEME_OUTPUT" "Vicinae"
}

reload_services() {
    log_info "Reloading applications..."
    hyprctl reload &
    if pgrep -x "swaync" > /dev/null; then swaync-client -rs; fi
}

apply_theme_and_reload() {
    local selected_wallpaper="$1"
    if [[ -z "$selected_wallpaper" || ! -f "$selected_wallpaper" ]]; then
        log_error "Invalid wallpaper path."
        return 1
    fi

    send_notification -u low "🎨 Generating Palette..."
    if ! set_wallpaper_and_generate_colors "$selected_wallpaper"; then return 1; fi
    
    source_colors
    send_notification -u low "🖌️ Applying Themes..."
    theme_aurora_shell
    theme_system_apps
    reload_services
    send_notification -i "$selected_wallpaper" "✅ Theme Applied" "Your new desktop theme is now active!"
}

main() {
    check_dependencies
    WALLPAPER_DIR=$(get_wallpaper_dir)
    if [[ -e "$LOCK_FILE" ]]; then exit 1; fi
    touch "$LOCK_FILE"
    trap 'rm -f "$LOCK_FILE"' EXIT

    if [[ -n "$1" && -f "$1" ]]; then
        apply_theme_and_reload "$1"
        exit 0
    fi

    case "$1" in
        --startup)
            if ! swww-daemon >/dev/null 2>&1; then swww init >/dev/null 2>&1; fi
            exit 0
            ;;
        --next|--prev)
            local wall_files
            mapfile -t wall_files < <(find "$WALLPAPER_DIR" -type f \( -iname '*.png' -o -iname '*.jpeg' -o -iname '*.jpg' -o -iname '*.gif' -o -iname '*.webp' \) | sort)
            if [[ ${#wall_files[@]} -eq 0 ]]; then exit 1; fi
            local current_wallpaper
            current_wallpaper=$(swww query | head -n 1 | sed 's/.*: //')
            local current_index=-1
            for i in "${!wall_files[@]}"; do
                if [[ "${wall_files[$i]}" == "$current_wallpaper" ]]; then current_index=$i; break; fi
            done
            [[ $current_index -eq -1 ]] && current_index=0
            local new_index
            local count=${#wall_files[@]}
            if [[ "$1" == "--next" ]]; then new_index=$(( (current_index + 1) % count )); else new_index=$(( (current_index - 1 + count) % count )); fi
            apply_theme_and_reload "${wall_files[$new_index]}"
            ;;
        --mute)
            NOTIFICATIONS_MUTED=true
            shift
            main "$@"
            ;;
        ""|*)
            local wall_files
            wall_files=$(find "$WALLPAPER_DIR" -type f \( -iname '*.png' -o -iname '*.jpeg' -o -iname '*.jpg' -o -iname '*.gif' -o -iname '*.webp' \) | sort)
            if [[ -z "$wall_files" ]]; then exit 1; fi
            local selected_wallpaper
            selected_wallpaper=$(echo "$wall_files" | cachy-selector "cachy-selector")
            if [[ -n "$selected_wallpaper" ]]; then apply_theme_and_reload "$selected_wallpaper"; fi
            ;;
    esac
}

main "$@"