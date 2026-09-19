# ESP32-C3 LED Matrix: scrolling text, animated eyes, countdown bar and a break nagger

A tiny desk gadget built from an **ESP32-C3** and a **4-module MAX7219 LED matrix** (32 x 8 red dots). It makes its own Wi-Fi network, so you control it from your phone with no app and no router. Over the USB cable it can also nag you to take a break when you have been at the PC too long.

**What it does**

| Feature | What you see |
|---------|--------------|
| 🔤 Text | Any message you type. Short words sit still and centred, longer text scrolls at the speed you choose |
| 👀 Animated eyes | Two eyes that look left, look right and blink |
| ⏳ Countdown timer | Stand the strip upright: it fills up LED by LED and is full exactly when time is up, then flashes |
| 💻 Break monitor | A script on your PC watches your keyboard and mouse. After 15 minutes of work the matrix flashes and scrolls *BITCH TAKE A BREAK* until you walk away for 3 minutes |
| 🛠 Self-test | Lights every LED, then each module, a row, a column and a pixel, so wiring problems are obvious |
| 📱 Phone page | Everything is controlled from a page at `http://192.168.4.1` that opens by itself when you join the Wi-Fi |

No soldering skills beyond plugging in five jumper wires. Total cost is around 10 USD.

---

## 1. What you need

| Part | Notes |
|------|-------|
| **ESP32-C3 board** | Any board with the chip's own USB port works. Tested with the small *ESP32-C3 SuperMini*. The official DevKitM-1 also works. |
| **MAX7219 4-in-1 LED matrix** | The common *FC-16* module: four 8x8 blocks in a row, one 5-pin input header. Search for "MAX7219 4 in 1 dot matrix". |
| **5 female-to-female jumper wires** | |
| **USB cable** | Data cable, not charge-only. It powers everything and carries the break-monitor messages. |
| **A PC** | Windows, macOS or Linux for flashing. The break monitor itself is Windows-only. |

---

## 2. Wiring

Five wires. Use the matrix header marked **DIN** (the input). The other end of the strip is marked **DOUT** and stays empty.

The table is in the **same order as the pins on the header**, top to bottom. Notice that **CS comes before CLK**. Swapping those two is the most common mistake and makes the modules light up fully one after another.

| Matrix header pin | ESP32-C3 pin |
|-------------------|--------------|
| VCC | **5V** (see note) |
| GND | GND |
| DIN | GPIO **0** |
| CS  | GPIO **2** |
| CLK | GPIO **1** |

```
  ESP32-C3                       MAX7219 4-in-1 (input header)
  ┌──────────┐                   ┌──────┬──────┬──────┬──────┐
  │       5V ├───────────────────┤ VCC  │      │      │      │
  │      GND ├───────────────────┤ GND  │      │      │      │
  │   GPIO 0 ├───────────────────┤ DIN  │      │      │      │
  │   GPIO 2 ├───────────────────┤ CS   │      │      │      │
  │   GPIO 1 ├───────────────────┤ CLK  │      │      │      │
  │      USB ├── to the PC       └──────┴──────┴──────┴──────┘
  └──────────┘                   (module 1 is the one next to the header)
```

**VCC note.** The MAX7219 is a 5 V chip whose inputs officially want 3.5 V for a logic high, while the ESP32-C3 outputs 3.3 V. Nearly all FC-16 modules accept this. If the display shows garbage with the wiring confirmed correct, move VCC to the **3.3V** pin: a little dimmer, but the signals are then well inside spec.

**Pins can be changed** at the top of [`src/main.cpp`](src/main.cpp) (`DIN_PIN`, `CLK_PIN`, `CS_PIN`).

---

## 3. Install the software on your PC

You need **Python 3** and **PlatformIO**. PlatformIO downloads the compiler and all libraries by itself the first time you build, so there is nothing else to install.

### Windows

1. Install Python from <https://www.python.org/downloads/>. On the first installer screen tick **"Add python.exe to PATH"**.
2. Open **PowerShell** (press the Windows key, type `powershell`, press Enter) and run:

```powershell
pip install platformio pyserial
```

### macOS

```bash
brew install python          # if you do not have Python 3 yet
pip3 install platformio pyserial
```

### Linux (Debian, Ubuntu, Mint)

```bash
sudo apt install python3 python3-pip
pip3 install platformio pyserial
sudo usermod -a -G dialout $USER     # lets you use the USB port; log out and in again afterwards
```

Check that it worked:

```
pio --version
```

You should see something like `PlatformIO Core, version 6.x`. If the command is not found, close and reopen the terminal.

> **Prefer a graphical editor?** Install [VS Code](https://code.visualstudio.com/) and its **PlatformIO IDE** extension, then open this folder. The ✓ (build) and → (upload) buttons in the bottom bar do the same as the commands below.
>
> **Prefer the Arduino IDE?** See [section 10](#10-arduino-ide-instead-of-platformio).

---

## 4. Get the code, build it, flash it

```bash
# 1. Download the project
git clone https://github.com/<your-github-username>/ESP32C3-LED-MATRIX.git
cd ESP32C3-LED-MATRIX

# 2. Build (the first time takes a few minutes while the toolchain downloads)
pio run

# 3. Plug the board into the PC by USB, then flash it
pio run -t upload

# 4. Watch what the board says (press Ctrl+C to leave)
pio device monitor
```

**What success looks like**

- Step 2 ends with `[SUCCESS]` and a line like `Flash: [=======   ] 65% (used 856788 bytes ...)`.
- Step 3 ends with `Hash of data verified.` then `[SUCCESS]`.
- Step 4 shows:

```
Boot. Reset reason: power-on
Module type 4: DR1CR0RR0 (FC-16)
Mode: text
WiFi: access point up
AP started. SSID: ESP32C3 LED_MATRIX
Open http://192.168.4.1
```

- The strip scrolls **WELCOME!** from right to left.

**If the upload fails with "Could not open COMx" or "ClearCommError"**, just run step 3 again, up to three times. The ESP32-C3's built-in USB port briefly disconnects when the flasher resets the chip, and on Windows the first attempt sometimes lands in that gap. Nothing is wrong with the board.

**If PlatformIO cannot find the port**, add it explicitly: `pio run -t upload --upload-port COM7` (Windows), `--upload-port /dev/ttyACM0` (Linux), `--upload-port /dev/cu.usbmodem1101` (macOS). `pio device list` shows the ports.

---

## 5. Check that your board works

One command runs the firmware's own tests over the USB cable and tells you PASS or FAIL:

```bash
python tools/verify_board.py
```

It asks the board for its status, runs the 9-step display self-test (watch the strip: every LED, then each module, a row, a column, a pixel), and runs a 10-second countdown while checking that all 256 bar steps arrive with exact timing. It ends with:

```
RESULT: PASS - the board is working
```

If the self-test looks wrong on the strip, jump to [Troubleshooting the display](#11-troubleshooting).

---

## 6. Use it from your phone

1. On your phone, join the Wi-Fi network **`ESP32C3 LED_MATRIX`**. It has no password.
2. Most phones show a *"Sign in to network"* notice. Tap it and the control page opens. If nothing appears, open a browser and go to **http://192.168.4.1**.
3. The page has five cards:

| Card | What to do |
|------|------------|
| **ESP32 LED Matrix Control** | Type a message, pick a scroll speed (10 to 500 ms per step), tap *Display Message*. Short text that fits on the strip, about five characters such as `BUSY`, is shown still and centred; longer text scrolls. |
| **Eyes** | Tap *Show Eyes*. The strip lies flat, connector on the right. The pupils glide left, hold two seconds, glide right, and the eyes blink now and then. *Show Text* switches back. Whichever you chose last is the resting screen and comes back after a timer, even after a power cycle. |
| **Break Monitor** | *Test Alert* shows the break message for 10 seconds. *Dismiss* clears a running alert. The real thing is driven from your PC, see section 7. |
| **Countdown Timer** | Stand the strip upright with the connector at the bottom. Set minutes and seconds, tap *Start Timer*. The bar fills from the bottom, one LED at a time, over the whole duration, is completely lit when time is up, then flashes for 10 seconds. The page shows a live countdown. If the bar grows downward on your setup, tick *Fill from the other end*. |
| **Display Setup** | Only needed if the text looks wrong. See [Troubleshooting](#11-troubleshooting). |

The network has no internet. Phones may warn about that or offer to switch back to mobile data; choose *stay connected* while you use the page.

---

## 7. The PC break monitor

The board cannot see your PC, so a small script on the PC does the watching. It reads the Windows idle timer (time since your last key press or mouse move) and talks to the board over the USB cable, so no Wi-Fi is involved.

- After **15 minutes** of continuous work it sends an alert: the strip flashes, then scrolls **BITCH TAKE A BREAK**.
- Pauses shorter than **3 minutes** still count as working.
- If you keep working through the alert, the strip flashes again every 2 minutes.
- After you have been away for 3 minutes the alert clears, the eyes or text come back, and the count starts again.

```
python tools/break_monitor.py                        # 15 min work, 3 min break
python tools/break_monitor.py --work 45 --break 5
python tools/break_monitor.py --message "TAKE A BREAK"
python tools/break_monitor.py --http 192.168.4.1     # over Wi-Fi instead of USB
```

On Windows you can also double-click `tools\break_monitor.bat`. Leave the window open; it shows a live line such as `working 07:12 / 15:00`. `Ctrl+C` stops it and clears any alert.

**Start it with Windows:** press Win+R, type `shell:startup`, press Enter, and put a shortcut to `break_monitor.bat` in the folder that opens.

**Note:** while the script runs it holds the USB serial port, so close it before flashing new firmware or running `verify_board.py`.

---

## 8. Serial console (no Wi-Fi needed)

With the USB cable connected, `pio device monitor` (or any serial terminal at 115200 baud) shows the board's log and accepts commands. Type the command and press Enter.

| Command | Effect |
|---------|--------|
| `s` | Print the current state |
| `x HELLO WORLD` | Set the scrolling text |
| `e` | Show the eyes |
| `t 30` | Start a 30-second timer (`t 30 f` fills from the other end) |
| `c` | Cancel the timer, back to the resting screen |
| `a TAKE A BREAK` | Show an alert until cleared |
| `k` | Clear the alert |
| `d` | Run the display self-test |
| `m` | List module types; `m 2` switches to type 2 |
| `f` | Print the current eye frame as ASCII art |

The board also logs every timer step with a timestamp, which is how `verify_board.py` checks the timing.

---

## 9. HTTP API

Every control is a plain `GET`, so `curl` or any automation works while you are on the board's Wi-Fi.

| Endpoint | Parameters | Effect |
|----------|------------|--------|
| `/` | – | The control page |
| `/setText` | `text` (1–127 chars), `speed` (10–500 ms) | Set the scrolling message and make text the resting screen |
| `/eyes` | – | Show the eyes and make them the resting screen |
| `/showText` | – | Show the text and make it the resting screen |
| `/startTimer` | `seconds` (1–86400), optional `flip` (`0`/`1`) | Start the countdown bar |
| `/cancelTimer` | – | Stop the timer |
| `/alert` | `text`, optional `seconds` (0 or omitted = until cleared) | Flash, then scroll `text` |
| `/clearAlert` | – | Remove the alert |
| `/selftest` | – | Run the 9-step display test |
| `/setModule` | `type` (0–7) | Set the module wiring variant, saved in flash |
| `/status` | – | JSON with `mode` (`text`/`eyes`/`alert`/`timer`/`done`/`selftest`), `remaining`, `total`, `flip`, `text`, `speed`, `module`, `step`, `idle`, `alert` |

Example, a 5-minute timer:

```
curl "http://192.168.4.1/startTimer?seconds=300"
```

---

## 10. Arduino IDE instead of PlatformIO

The same firmware is in [`arduino/ESP32C3_LED_MATRIX/ESP32C3_LED_MATRIX.ino`](arduino/ESP32C3_LED_MATRIX/ESP32C3_LED_MATRIX.ino).

1. In the Arduino IDE open *File → Preferences* and add this to *Additional boards manager URLs*:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
2. *Tools → Board → Boards Manager*, install **esp32 by Espressif Systems**.
3. *Tools → Manage Libraries*, install **MD_Parola**, **MD_MAX72XX**, **ESP Async WebServer** (by ESP32Async) and **Async TCP** (by ESP32Async).
4. Open the `.ino` file. Under *Tools* choose board **ESP32C3 Dev Module** and set **USB CDC On Boot: Enabled**.
5. Plug in the board, pick its port, press Upload.

The `.ino` is a copy of `src/main.cpp`. If you edit the PlatformIO version, run `python tools/sync_ino.py` to update the copy.

---

## 11. Troubleshooting

### The display
Run the self-test first (`python tools/verify_board.py`, or the *Display Setup* card, or `d` on the serial console). The scrolling **WELCOME!** at power-up must look right before anything else can.

**Modules light up fully one after another, then all go dark, repeating**
- The **CS and CLK wires are swapped**. Header order is VCC, GND, DIN, CS, CLK.
- The wires are on the **DOUT** (output) header instead of DIN.
- A loose jumper on DIN, CS or CLK.

**Garbled, mirrored or transposed pixels, or the timer bar draws along the strip's edge**
- Wrong module type. Modules differ in how the MAX7219 chip maps to rows and columns. On the phone page, *Display Setup*, run the self-test and change *Module type* until every step looks as described there. The choice is saved on the board. FC-16 is the default, Generic the next most common.
- The eight types are named DR/CR/RR. In the self-test: four short lines across the strip in the *Row* step means DR is wrong, a line on the wrong long edge means RR is wrong, a line one module in instead of at the connector end in the *Column* step means CR is wrong.

**Blank display**
- No common ground, VCC not connected, or a charge-only USB cable.

**Self-test steps and what each must look like**
1. All LEDs on. A dark LED is a wiring or module fault.
2. All LEDs off.
3. to 6. Each module alone, in order, starting with the one nearest the connector.
7. Row: one straight line along the whole long edge of the strip.
8. Column: one 8-LED line across the strip, right at the connector end.
9. Pixel: a single LED in the corner at the connector end.

### Wi-Fi
The serial log (`pio device monitor`) prints the reset reason at boot, whether the access point started, and every client that joins or leaves.

**The network is not visible**
- It is **2.4 GHz only**. Wait 5 seconds after power-up.
- Small boards with a PCB antenna (SuperMini and similar) are unreliable at full transmit power. The firmware already lowers it (`WIFI_TX_POWER` in the code). Boards with a proper antenna can raise it.
- `Reset reason: BROWNOUT` in the log means the power supply is too weak. Use a short good cable and a 1 A charger. Power the matrix from 5V, never from the 3.3V pin.

**Visible but cannot join, or drops**
- No internet on this network. Choose *stay connected* and turn off *auto-switch to mobile data* while using it.
- Four clients maximum.
- `client joined` quickly followed by `client left` in the log points to the power or antenna issues above.

### Flashing
- *Could not open COMx* or *ClearCommError*: run the upload again, up to three times. See section 4.
- *No serial port at all*: charge-only cable, or the board is in a boot loop. Try another cable.
- Close `break_monitor.py` and `verify_board.py` before flashing; they hold the port.

---

## 12. Project layout

```
platformio.ini              build settings: ESP32-C3, libraries, USB serial on
src/main.cpp                the whole firmware (display, web page, timer, eyes, alert, self-test)
arduino/ESP32C3_LED_MATRIX/ the same firmware as an Arduino IDE sketch
tools/verify_board.py       one-command check of a flashed board over USB
tools/break_monitor.py      PC-side break nagger (Windows)
tools/break_monitor.bat     double-click launcher for the monitor
tools/sync_ino.py           copies src/main.cpp to the Arduino sketch
tools/requirements.txt      Python packages for the tools (pyserial)
```

---

## 13. Customising

Everything tunable is a `#define` near the top of [`src/main.cpp`](src/main.cpp):

| Define | Default | Meaning |
|--------|---------|---------|
| `MAX_DEVICES` | 4 | Number of 8x8 modules in the chain |
| `DIN_PIN`, `CLK_PIN`, `CS_PIN` | 0, 1, 2 | Wiring |
| `WIFI_TX_POWER` | 8.5 dBm | Raise for boards with a real antenna |
| `DONE_BLINK_MS` | 10000 | How long the strip flashes when a timer ends |
| `EYE_W`, `EYE_H`, `LOOK_RANGE`, `SWEEP_HOLD_MS`, `BLINK_MS` | 11, 6, 3, 2000, 220 | Eye size, how far the pupils travel, hold time per side, blink length |
| `ALERT_FLASH_MS` | 1200 | Flashing before the alert text starts |
| `TIMER_DEBUG` | 1 | Set to 0 to stop logging every timer step |
| `ssid` | ESP32C3 LED_MATRIX | Wi-Fi network name |

The message and the timings of the break monitor are command-line options, see section 7.

---

## 14. Credits

- Original scrolling-text project and web page: **Roy Cuadra**.
- Timer, eyes, break monitor, self-test, captive portal and the tools: added on top of it.
- Eye animation style after the **Nico DeskBuddy** project.
- Libraries: [MD_Parola](https://github.com/MajicDesigns/MD_Parola) and [MD_MAX72XX](https://github.com/MajicDesigns/MD_MAX72XX) by MajicDesigns, [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) and [AsyncTCP](https://github.com/ESP32Async/AsyncTCP) by ESP32Async.

This project is for educational and hobby use. Handle electronics safely.
