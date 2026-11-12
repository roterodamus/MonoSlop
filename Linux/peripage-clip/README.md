# peripage-clip

Print the Wayland clipboard (text or image) on a Peripage A6 (203 dpi) over Bluetooth.

## Requirements
- `wl-clipboard` (provides `wl-paste`), `fontconfig` (for `fc-match`)
- Rust >= 1.85 to build
- A paired printer

## Build & install
    cargo build --release
    install -Dm755 target/release/peripage-clip ~/.local/bin/peripage-clip

## Pair once
    bluetoothctl
    > scan on          # note the printer's MAC
    > pair AA:BB:CC:DD:EE:FF
    > trust AA:BB:CC:DD:EE:FF

## Configure
    peripage-clip --init      # writes ~/.config/peripage-clip/config.toml
Edit the file and set `mac`.

## Use
    peripage-clip                    # print clipboard
    peripage-clip --preview out.png  # render what would print, no Bluetooth
    peripage-clip --file pic.png     # print a file instead of the clipboard

Sway/Hyprland keybinding example:
    bindsym $mod+p exec peripage-clip
