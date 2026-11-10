# USB to Bluetooth Keyboard Proxy: Install Guide

For the YD-ESP32-S3 on Arch Linux. Works with bash, zsh or fish.

## 1. Install ESP-IDF (one time)

These commands work in any shell:

```
sudo pacman -S --needed git cmake ninja ccache dfu-util libusb python python-pip
mkdir -p ~/esp
cd ~/esp
git clone -b v5.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
sudo usermod -aG uucp $USER
```

Log out and back in once so the serial port permission applies.

## 2. Make a shortcut (one time)

Pick your shell.

**bash** (add to `~/.bashrc`):
```bash
echo "alias get_idf='. ~/esp/esp-idf/export.sh'" >> ~/.bashrc
source ~/.bashrc
```

**zsh** (add to `~/.zshrc`):
```zsh
echo "alias get_idf='. ~/esp/esp-idf/export.sh'" >> ~/.zshrc
source ~/.zshrc
```

**fish**:
```fish
function get_idf
    . ~/esp/esp-idf/export.fish
end
funcsave get_idf
```

**Other shells (sh, dash, ksh)**: run `. ~/esp/esp-idf/export.sh` directly each time.

## 3. Build and flash

Same in every shell:

```
get_idf
cd path/to/ble_kb_proxy
idf.py set-target esp32s3
idf.py build flash monitor
```

Plug your computer into the **COM** port to flash.

## 4. Hook up the keyboard

1. Plug the keyboard into the **USB** port.
2. Keep the board powered from the **COM** port. The keyboard needs 5V from the board.

## 5. Pair

1. Open Bluetooth settings on your computer or phone.
2. Pair with **Vial BLE Keyboard**.
3. Type. Keys should work.

## Notes

- Run `get_idf` in every new terminal before using `idf.py`.
- Check it worked with `idf.py --version`.
- Exit the serial monitor with `Ctrl+]`.
- Reset all pairings: hold **BOOT** while pressing reset.
- Vial's config app still needs the keyboard plugged into a computer.

## Problems

| Problem | Fix |
|---|---|
| `idf.py: command not found` | You forgot to run `get_idf` in this terminal. |
| fish `export.fish` errors | Fish support is experimental. Run in bash instead: `bash -c '. ~/esp/esp-idf/export.sh && idf.py build flash monitor'` |
| Port not found | Check `ls /dev/ttyACM* /dev/ttyUSB*`. Try the `dialout` group instead of `uucp`. |
| `install.sh` fails on Python | Use an older Python, e.g. `python3.12 ./install.sh esp32s3`. |
| Keyboard has no power | Check for a USB-OTG jumper on the board, or power it with external 5V. |
| Garbled keys | Turn NKRO off on the keyboard. |
