# Aurora-Shell

![Desktop Showcase](pics/allwidgets.png)
###### *Song: Euthanasia - Will Wood*
---

### An arch user's descent into AI exploitation... And apparently Desktop Shell Development.

A fully automated and cohesive desktop experience built on Hyprland, made possible using C/GTK4 - And AI exploitation.

---

### Key Features

* **Dynamic Theming:** Leveraging Wallust, a set of color palletes are generated from the wallpaper and applied to the set of default widgets the shell provides.
*   **Control Center:** Manages Wi-Fi, Bluetooth, audio sinks, brightness, and volume.
*   **Hyper-Calendar:** A full calendar with CRUD (Create, Read, Update, Delete) functionality for managing events and schedules.
*   **Side-MPRIS-Player:** A media player widget that displays metadata and features perfectly **synced lyrics** for music playing in any MPRIS-compatible player (including browsers).
* **App-Launcher:** Launches apps and does math I guess.

---

### Usage

All widgets are controlled through the main `aurora-shell` executable using the `--toggle` flag. This design makes it easy to integrate with any workflow or window manager.

### Integrating with Hyprland

The most common way to use Aurora Shell is by adding entries to your `hyprland.conf`.

1.  **Launching Widgets on Startup:**
    Add an `exec-once` rule for any widget you want to be visible when you log in, like the topbar.

2.  **Creating Keybindings:**
    Use `bind` rules to assign a keyboard shortcut to toggle each widget.

Here is a sample configuration to add to your `~/.config/hyprland/hyprland.conf`:

```ini
# ~/.config/hyprland/hyprland.conf

# --- Aurora Shell Integration ---

# Automatically start the topbar when Hyprland loads
exec-once = aurora-shell --toggle topbar

# Keybindings to toggle individual widgets
# (Change $mainMod and keys to your preference)

# Launcher ($mainMod + D)
bind = $mainMod, D, exec, aurora-shell --toggle launcher

# Control Center ($mainMod + C)
bind = $mainMod, C, exec, aurora-shell --toggle control-center

# Calendar ($mainMod + A)
bind = $mainMod, A, exec, aurora-shell --toggle calendar

# Cheatsheet ($mainMod + K)
bind = $mainMod, K, exec, aurora-shell --toggle cheatsheet

# MPRIS Media Player ($mainMod + M)
bind = $mainMod, M, exec, aurora-shell --toggle mpris-player

# Themer / Wallpaper Changer
bind = $mainMod, T, exec, aurora-shell --toggle themer

# Uptime / System Info
bind = $mainMod, U, exec, aurora-shell --toggle uptime
```

### Command Reference

Here is a complete list of the default widget toggle commands:

| Widget | Command |
| :--- | :--- |
| **Topbar** | `aurora-shell --toggle topbar` |
| **Launcher** | `aurora-shell --toggle launcher` |
| **Control Center** | `aurora-shell --toggle control-center` |
| **Calendar** | `aurora-shell --toggle calendar` |
| **Cheatsheet** | `aurora-shell --toggle cheatsheet` |
| **MPRIS Player** | `aurora-shell --toggle mpris-player` |
| **Themer** | `aurora-shell --toggle themer` |
| **Uptime** | `aurora-shell --toggle uptime` |

## Dependencies

Before building, you must install the necessary development packages and libraries.

### Core Build Tools

You will need `meson`, `ninja`, and a C compiler like `gcc`.

### Project Libraries

Aurora Shell and its widgets rely on several libraries. The table below lists the required Meson dependency names and the corresponding package names for popular distributions.

| Dependency Name           | Arch Linux          | Debian / Ubuntu          | Fedora                     |
| :------------------------ | :------------------ | :----------------------- | :------------------------- |
| `gtk4`                    | `gtk4`              | `libgtk-4-dev`           | `gtk4-devel`               |
| `libadwaita-1`            | `libadwaita`        | `libadwaita-1-dev`       | `libadwaita-devel`         |
| `gtk4-layer-shell-0`      | `gtk-layer-shell`   | `libgtk-layer-shell-0-dev` | `gtk-layer-shell-devel`    |
| `json-glib-1.0`           | `json-glib`         | `libjson-glib-dev`       | `json-glib-devel`          |
| `gio-2.0` / `gio-unix-2.0`| `glib2`             | `libglib2.0-dev`         | `glib2-devel`              |
| `libsoup-3.0`             | `libsoup3`          | `libsoup-3.0-dev`        | `libsoup3-devel`           |
| `dl` / `m`                | `glibc` (base)      | `libc6-dev` (base)       | `glibc-devel` (base)       |

### Runtime Dependencies

Some widgets require external tools to be installed on your system to function correctly.

*   **MPRIS Player:** Requires `playerctl` for media control.
*   **Control Center:**
    *   **Audio:** Requires `pulseaudio` or `pipewire-pulse`.
    *   **Network:** Requires `network-manager`.
    *   **Bluetooth:** Requires `bluez` and `bluez-tools`.

    ---

### Install Commands

**On Arch Linux:**
```bash
sudo pacman -S meson ninja gcc gtk4 libadwaita gtk-layer-shell json-glib glib2 libsoup3 playerctl networkmanager bluez bluez-utils
```

**On Debian / Ubuntu:**
```bash
sudo apt install meson ninja build-essential libgtk-4-dev libadwaita-1-dev libgtk-layer-shell-0-dev libjson-glib-dev libglib2.0-dev libsoup-3.0-dev playerctl network-manager bluez
```

**On Fedora:**
```bash
sudo dnf install meson ninja gcc gtk4-devel libadwaita-devel gtk-layer-shell-devel json-glib-devel glib2-devel libsoup3-devel playerctl NetworkManager bluez
```

## Installation

1.  **Clone the repository and navigate into it:**
    ```bash
    git clone https://github.com/Spn4x/Aurora-Shell.git
    cd aurora-shell
    ```

2.  **Set up the build directory using Meson:**
    ```bash
    meson setup build
    ```

3.  **Compile the source code:**
    ```bash
    ninja -C build
    ```

4.  **Install the application system-wide:**
    ```bash
    sudo meson install -C build
    ```

## Uninstallation

To completely remove Aurora Shell from your system, follow these steps.

1.  **Navigate to the build directory** within the project folder:
    ```bash
    cd /path/to/aurora-shell/build
    ```

2.  **Run the Ninja uninstall script:**
    This removes the binaries and other files managed by the installer.
    ```bash
    sudo ninja uninstall
    ```

3.  **Manually remove the shared data directory:**
    ⚠️ This command permanently deletes the directory and its contents.
    ```bash
    sudo rm -rf /usr/local/share/aurora-shell
    ```

---

    ## License

    This project is licensed under the [GgplV3 License](LICENSE). Feel free to use and modify the code as you see fit.
