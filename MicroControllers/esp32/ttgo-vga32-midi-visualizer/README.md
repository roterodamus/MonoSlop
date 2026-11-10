# VGA MIDI Visualizer

An abstract MIDI visualizer for the **TTGO VGA32 v1.4** (ESP32). It turns Bluetooth MIDI into moving shapes and words on a VGA monitor.

- 8 shape layers (MIDI channels 1-8), 32 shapes
- 1 word layer (MIDI channel 9), 128 words
- Background color and preset control on MIDI channel 16
- 32 preset slots saved in flash
- Bluetooth MIDI in two modes: connect from an iPad or computer, or connect to a BLE-MIDI keyboard

---

## Install

### What you need
- TTGO VGA32 v1.4 board
- VGA monitor and cable
- USB cable for power and programming
- Arduino IDE

### Steps

1. **Install the ESP32 board package** in Arduino IDE (Boards Manager, "esp32" by Espressif). Use version **2.0.x**. FabGL does not work with 3.x.
2. **Install the libraries** (Library Manager):
   - `FabGL`
   - `NimBLE-Arduino` version **1.4.x** (2.x will not compile)
3. **Select the board settings:**
   - Board: `ESP32 Dev Module`
   - Partition Scheme: `Huge APP (3MB No OTA)`
4. **Open** `vga_midi_viz.ino` (keep it in a folder named `vga_midi_viz`).
5. **Upload** it to the board, then connect the VGA monitor.

### If something goes wrong

| Problem | Fix |
|---|---|
| Out of memory, or the board resets on boot | Set `#define DOUBLE_BUFFER 0` at the top of the sketch (the picture will flicker a little) |
| Slow frame rate | Lower the **Copies** control (CC 25) on busy channels |
| A control seems to do nothing | Keep `#define DEBUG_CC 1` and open the Serial Monitor at 115200 baud. Every incoming CC is printed there |

---

## Connect

The button on **IO36** switches between two Bluetooth modes. The mode is remembered after power-off.

| Mode | What it does | Dot while not connected |
|---|---|---|
| **Device** (default) | Shows up as **"VGA-MIDI-Viz"** so an iPad or computer can connect to it | Red |
| **Keyboard** | Scans for and connects to BLE-MIDI keyboards (Yamaha, Casio, M-Vave and others) | Yellow |

The dot is in the top-left corner and disappears once connected.

**Device mode:**
- macOS: Audio MIDI Setup, then MIDI Studio, then the Bluetooth icon, then Connect.
- iPad or iPhone: connect from your music app's Bluetooth MIDI settings.
- Windows: use a BLE-MIDI bridge such as MIDIberry.

**Keyboard mode:** turn on the keyboard's Bluetooth MIDI and make sure it is not already connected to another device. Most keyboards only send on MIDI channel 1 and cannot send CCs, so in this mode you will mostly play layer 1.

---

## Play

| MIDI channel | What it controls |
|---|---|
| 1-8 | Shape layers. Each one is independent. |
| 9 | Word layer |
| 16 | Background color and presets |

- **Note pitch:** sets where the shape sits around its orbit and shifts its color.
- **Velocity:** sets size and brightness.
- **Pitch bend:** rotates the layer.
- **CC 120 or 123:** all notes off. The shapes fade out.

### Controls for channels 1-9

Send these CCs on the channel you want to change.

| CC | Function | Notes |
|---|---|---|
| 20 | Shape (words on ch 9) | See the lists below |
| 21 | Shape spin | 64 = stopped, higher = clockwise, lower = counter-clockwise (no effect on words) |
| 22 | Hue | Base color |
| 23 | Size | |
| 24 | Release | How long shapes take to fade, about 0.1 to 4 s |
| 25 | Copies | 1 to 12 mirrored copies |
| 26 | Orbit radius | Distance from the anchor point |
| 27 | Style | Low = outline, middle = filled, high = filled with outline |
| 28 | Hue spread | Color shift across pitch and copies |
| 29 | Expand | Shapes grow as they age |
| 30 | Position X | |
| 31 | Position Y | |
| 85 | Orbit angle mode | Low = by pitch, middle = auto-rotate, high = both |
| 86 | Orbit rotate speed | 64 = stopped |

### Shapes (CC 20 on channels 1-8)

Each shape covers 4 values. Use the first number in the range.

| Value | Shape | Value | Shape | Value | Shape | Value | Shape |
|---|---|---|---|---|---|---|---|
| 0 | Circle | 32 | Heart | 64 | Gear | 96 | Music note |
| 4 | Triangle | 36 | Crescent | 68 | Crown | 100 | Wrench |
| 8 | Square | 40 | Line | 72 | Shield | 104 | Envelope |
| 12 | Diamond | 44 | Sparkle | 76 | Hourglass | 108 | Acid smiley |
| 16 | Pentagon | 48 | Spade | 80 | Lightning bolt | 112 | Infinity |
| 20 | Hexagon | 52 | Club | 84 | Arrow | 116 | Dollar |
| 24 | Star | 56 | Chevron | 88 | Flower | 120 | Euro |
| 28 | Cross | 60 | Ring | 92 | Bell | 124 | Yen |

### Words (channel 9)

CC 20 picks the word directly: value 0 is ACID, 1 is DANCE, 2 is FREEDOM, and so on up to 127. The full list of 128 words is in the sketch under `WORDS`.

- Words step through four font sizes as the size grows.
- Style: low = plain text, middle = solid box with inverted text, high = dim box with bright text.
- Spin has no effect, because text cannot rotate. Orbit controls still move the words.

### Background (channel 16)

| CC | Function |
|---|---|
| 20 | Background hue |
| 21 | Background saturation |
| 22 | Background brightness (0 = black, the default) |

---

## Presets

You can save the whole look (all 9 channels plus the background) to one of 32 slots. Slots are kept when the power is off.

1. **Pick a slot:** send a **Program Change 0-31** on any channel.
2. **Save:** send **CC 24** on channel 16 with a value of 64 or higher.
3. **Load:** send **CC 23** on channel 16 with a value of 64 or higher.

**Slot bar:** send **CC 25** on channel 16. A value of 64 or higher shows a row of 32 slots at the bottom of the screen for 2 seconds after any slot action. A value below 64 hides it (the default).

| Color | Meaning |
|---|---|
| Cyan | Selected slot |
| Green | Just loaded |
| Magenta | Just saved |
| Red | Tried to load an empty slot |
| Gray | Slot holds a preset |
| Dark outline | Empty slot |

---

## Notes

- The slot bar setting is not saved in presets and resets to hidden at power-up.
- Connecting to a keyboard can pause the picture for a few seconds while it connects.
- Do not hold the IO36 button while powering on.
- If the button triggers randomly, add a 10k pull-up resistor from IO36 to 3.3V.
