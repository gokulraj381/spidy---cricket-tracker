# Spidy Cricket Tracker: an ESP32-C3 LED matrix with live cricket scores, animated eyes, scrolling text, a countdown bar and a break nagger

A tiny desk gadget built from an **ESP32-C3** and a **4-module MAX7219 LED matrix** (32 x 8 red dots). It makes its own Wi-Fi network, so you control it from your phone with no app and no router. Over the USB cable it can also nag you to take a break when you have been at the PC too long.

**What it does**

| Feature | What you see |
|---------|--------------|
| 🔤 Scrolling text | Any message you type, at any speed |
| 👀 Animated eyes | Two eyes that look left, look right and blink, in four styles: classic, round, outline and robot |
| ⏳ Countdown timer | Stand the strip upright: it fills up LED by LED and is full exactly when time is up, then flashes |
| 💻 Break monitor | A script on your PC watches your keyboard and mouse. After 15 minutes of work the matrix flashes and scrolls *BITCH TAKE A BREAK* until you walk away for 3 minutes |
| 🏏 Cricket scoreboard | The live score of an international match: score, overs, chase and batters, with an animation for every four, six, wicket, fifty, hundred and the result. The board fetches it by itself over your home Wi-Fi, or a script on your PC sends it over USB |
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
> **Prefer the Arduino IDE?** See [section 11](#11-arduino-ide-instead-of-platformio).

---

## 4. Get the code, build it, flash it

```bash
# 1. Download the project
git clone https://github.com/gokulraj381/spidy---cricket-tracker.git
cd spidy---cricket-tracker

# 2. Build (the first time takes a few minutes while the toolchain downloads)
pio run

# 3. Plug the board into the PC by USB, then flash it
pio run -t upload

# 4. Watch what the board says (press Ctrl+C to leave)
pio device monitor
```

**What success looks like**

- Step 2 ends with `[SUCCESS]` and a line like `Flash: [=======   ] 69% (used 1362357 bytes ...)`.
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

If the self-test looks wrong on the strip, jump to [Troubleshooting the display](#12-troubleshooting).

---

## 6. Use it from your phone

1. On your phone, join the Wi-Fi network **`ESP32C3 LED_MATRIX`**. It has no password.
2. Most phones show a *"Sign in to network"* notice. Tap it and the control page opens. If nothing appears, open a browser and go to **http://192.168.4.1**.
3. The page has seven cards:

| Card | What to do |
|------|------------|
| **ESP32 LED Matrix Control** | Type a message, pick a scroll speed (10 to 500 ms per step), tap *Display Message*. |
| **Eyes** | Pick a style: *Classic*, *Round*, *Outline* or *Robot*. The strip lies flat, connector on the right. The pupils glide left, hold two seconds, glide right, and the eyes blink now and then. The robot style looks left, looks right, returns to the centre and blinks twice in a loop of about five seconds. The style is saved on the board. *Show Text* switches back. Whichever you chose last is the resting screen and comes back after a timer, even after a power cycle. |
| **Break Monitor** | *Test Alert* shows the break message for 10 seconds. *Dismiss* clears a running alert. The real thing is driven from your PC, see section 7. |
| **Cricket Score** | Shows what the scoreboard is displaying. *Demo Score* puts a sample score up for 3 minutes, and the *Four*, *Six*, *Wicket*, *Fifty*, *Hundred* and *Win* buttons preview each animation. The live score comes over Wi-Fi or from your PC, see section 8. |
| **Live Scores over Wi-Fi** | Enter your home Wi-Fi name and password and tap *Save Wi-Fi*. The board joins it and shows live international scores with no PC. Optionally name one team to follow. See section 8. |
| **Countdown Timer** | Stand the strip upright with the connector at the bottom. Set minutes and seconds, tap *Start Timer*. The bar fills from the bottom, one LED at a time, over the whole duration, is completely lit when time is up, then flashes for 10 seconds. The page shows a live countdown. If the bar grows downward on your setup, tick *Fill from the other end*. |
| **Display Setup** | Only needed if the text looks wrong. See [Troubleshooting](#12-troubleshooting). |

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

## 8. Live cricket scores

The matrix follows the first live **international** match it finds, and waits for one if none is on. No account or API key is needed: the score comes from ESPN's public cricket scoreboard. That feed is unofficial and undocumented, so it can change or stop working without notice.

Lay the strip flat with the connector on the right, the same way as for the eyes.

There are two ways to feed it. Use either, or both: while the PC script runs it takes over, and the board picks up again a minute after it stops.

| | Over Wi-Fi (no PC) | PC script over USB |
|---|---|---|
| Needs | Your home Wi-Fi name and password | A PC that stays on, and the USB cable |
| Score, overs, chase, run rate, result | Yes | Yes |
| Wicket animation | Exact | Exact |
| Four and six animations | Read from the jump in the total, so four singles between two checks can look like a four | Exact, from the scorecard's boundary counts |
| Batter pages, fifty and hundred animations | No | Yes |

### Over Wi-Fi, without a PC

1. Join the board's own network `ESP32C3 LED_MATRIX` with your phone and open the page, as in section 6.
2. In the **Live Scores over Wi-Fi** card enter your home Wi-Fi name and password and tap *Save Wi-Fi*. It must be a 2.4 GHz network.
3. Within about 15 seconds the card shows `connected` and the board's address on your home network. From then on, whenever an international match is live, the score appears by itself.

Optional: type a team such as `IND` under *Only this team* and tap *Save* to ignore other matches. Untick *Show live international scores automatically* to switch it off without forgetting the Wi-Fi.

- The settings are saved on the board and survive a power cycle.
- The board keeps its own network as well, so the control page still opens at `http://192.168.4.1`. It is also reachable from your home network at the address shown in the card. When the board joins your Wi-Fi its own network moves to the same channel, so a phone connected to it may drop for a moment.
- The board's network has no password, so the Wi-Fi password you type travels unencrypted from your phone to the board. Set it up at home. To avoid that entirely, set it over the USB cable instead: `w MyNetwork|MyPassword` on the serial console.
- The password is stored on the board in plain form. Anyone with the board and a USB cable could read it out.
- If it stays on `not connected`, check the name and password, move the board closer to the router, and see `WIFI_TX_POWER` in section 14.

### From the PC, over USB

A script on the PC fetches the live score and sends it to the board over the USB cable.

```
python tools/cricket_score.py                   # first live international match
python tools/cricket_score.py --team IND        # only follow India
python tools/cricket_score.py --list            # show today's matches and their ids
python tools/cricket_score.py --match 1550711   # follow one match by id
python tools/cricket_score.py --all             # include domestic and league matches
python tools/cricket_score.py --demo            # no internet: a made-up finish with every animation
```

On Windows you can also double-click `tools\cricket_score.bat`. `Ctrl+C` stops it and the eyes or text come back.

**What the matrix shows.** Pages rotate every few seconds. A page that is too wide for the strip scrolls through instead.

| Page | Example |
|------|---------|
| Score of the batting side, as the stadium-style score page | `IND 187/4` |
| Overs | `OV 18.2` |
| Chase (second innings) | `NEED 24`, then `10 BALLS` |
| Run rate (first innings of an ODI or T20) | `CRR 8.45` |
| The two batters | `KOHLI 72*` |
| The other side's total | `AUS 210/6` |
| Match state and situation | `STUMPS`, `SL TRAIL BY 30 RUNS` |

The score page is laid out like a stadium board instead of a line of text: the team name top left, the runs in tall digits that use the whole height of the strip, and the wickets as a small digit bottom right. Under the team name sit six dots, one per ball bowled this over. The newest dot blinks, the next one flickers, and the row sweeps clear when the over ends.

When runs are scored, `+1`, `+2` and so on takes the place of the team name for a moment, the digits that changed roll over like an odometer, and the brightness kicks up and settles back. A highlight sweeps across the score each time the page comes up.

The other pages come in with one of three transitions in turn: a ripple from the left, a drop from the centre outwards, and a bar that wipes across. Overs, run rate, chase and batter pages carry a small icon (ball, bars, target, bat) where it fits.

**Animations**

| Event | What plays |
|-------|------------|
| Four | The ball races along the ground, a flash, then `FOUR!` |
| Six | The ball sails up and out of the ground, fireworks, then `SIX!` |
| Wicket | The ball hops in and tips the bails off, then `OUT!` drops in letter by letter, each landing with a little hop |
| Fifty, hundred | Sparkles, `50!` or `100!` between dancing bars, then the batter's name |
| Result | A wave along the strip, then the result scrolls through |

**Good to know**

- The PC script checks the score every 15 seconds (`--poll` changes that) and the board every 10 seconds over Wi-Fi, so an animation plays up to that long after the feed updates.
- With the PC script, fours and sixes are counted from the scorecard's boundary totals, so they are exact. If the scorecard cannot be fetched, the script falls back to the jump in the total, which can occasionally mistake four singles for a boundary. Over Wi-Fi the board always uses the jump in the total.
- The board keeps the scoreboard only while scores keep arriving. If the script stops, the PC sleeps or the Wi-Fi drops, the board returns to the eyes or text after 3 minutes.
- A countdown timer and the break alert take over the display and the score comes back afterwards.
- The script holds the USB serial port, so it cannot run at the same time as `break_monitor.py`, and it must be closed before flashing or running `verify_board.py`.
- `--http 192.168.4.1` sends over Wi-Fi instead of USB. That only works if the PC reaches the board's Wi-Fi and the internet at the same time, for example Wi-Fi to the board and Ethernet to the router.

---

## 9. Serial console (no Wi-Fi needed)

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
| `f` | Print the current eyes or cricket frame as ASCII art |
| `q 2\|IND 187/4\|OV 18.2` | Show a cricket score: balls bowled this over (0 to 6, or `-` for no over row), then up to 8 pages separated by `\|` |
| `v 6` | Play a cricket animation: `v 4`, `v 6`, `v W`, `v 50 KOHLI 50*`, `v 100 KOHLI 100*`, `v WIN IND WON BY 5 WICKETS` |
| `Q` | Close the cricket scoreboard |
| `w MyNetwork\|MyPassword` | Join a home Wi-Fi network for live scores. `w -` forgets it, `w` alone prints the Wi-Fi and live-score state |
| `l 1 IND` | Live scores over Wi-Fi on, following only India. `l 1` follows any international, `l 0` switches it off |

The board also logs every timer step with a timestamp, which is how `verify_board.py` checks the timing.

---

## 10. HTTP API

Every control is a plain `GET`, so `curl` or any automation works while you are on the board's Wi-Fi.

| Endpoint | Parameters | Effect |
|----------|------------|--------|
| `/` | – | The control page |
| `/setText` | `text` (1–127 chars), `speed` (10–500 ms) | Set the scrolling message and make text the resting screen |
| `/eyes` | optional `style` (0 classic, 1 round, 2 outline, 3 robot; omitted = the saved style) | Show the eyes and make them the resting screen |
| `/showText` | – | Show the text and make it the resting screen |
| `/startTimer` | `seconds` (1–86400), optional `flip` (`0`/`1`) | Start the countdown bar |
| `/cancelTimer` | – | Stop the timer |
| `/alert` | `text`, optional `seconds` (0 or omitted = until cleared) | Flash, then scroll `text` |
| `/clearAlert` | – | Remove the alert |
| `/cricket` | `d` = `<balls>\|PAGE\|PAGE...`, for example `2\|IND 187/4\|OV 18.2` | Show or update the cricket scoreboard. It closes by itself 3 minutes after the last update |
| `/cricketEvent` | `e` (`4`, `6`, `W`, `50`, `100`, `WIN`), optional `text` | Play an animation on the scoreboard |
| `/cricketStop` | – | Close the scoreboard |
| `/wifi` | `ssid`, optional `pass` (8 to 64 chars) | Join a home network for live scores. An empty `ssid` forgets it. Without `pass` the saved password is kept for the same network |
| `/cricketAuto` | `on` (`0`/`1`), optional `team` | Switch live scores over Wi-Fi on or off, and optionally follow one team only |
| `/selftest` | – | Run the 9-step display test |
| `/setModule` | `type` (0–7) | Set the module wiring variant, saved in flash |
| `/status` | – | JSON with `mode` (`text`/`eyes`/`alert`/`timer`/`done`/`selftest`/`cricket`), `remaining`, `total`, `flip`, `text`, `speed`, `module`, `eyeStyle`, `step`, `idle`, `alert`, `cricket`, `wifi`, `wifiState`, `auto`, `team`, `net` |

Example, a 5-minute timer:

```
curl "http://192.168.4.1/startTimer?seconds=300"
```

---

## 11. Arduino IDE instead of PlatformIO

The same firmware is in [`arduino/ESP32C3_LED_MATRIX/ESP32C3_LED_MATRIX.ino`](arduino/ESP32C3_LED_MATRIX/ESP32C3_LED_MATRIX.ino).

1. In the Arduino IDE open *File → Preferences* and add this to *Additional boards manager URLs*:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
2. *Tools → Board → Boards Manager*, install **esp32 by Espressif Systems**.
3. *Tools → Manage Libraries*, install **MD_Parola**, **MD_MAX72XX**, **ArduinoJson**, **ESP Async WebServer** (by ESP32Async) and **Async TCP** (by ESP32Async).
4. Open the `.ino` file. Under *Tools* choose board **ESP32C3 Dev Module**, set **USB CDC On Boot: Enabled** and **Partition Scheme: Minimal SPIFFS (1.9MB APP with OTA)**. The firmware does not fit the default scheme.
5. Plug in the board, pick its port, press Upload.

The `.ino` is a copy of `src/main.cpp`. If you edit the PlatformIO version, run `python tools/sync_ino.py` to update the copy.

---

## 12. Troubleshooting

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
- Close `break_monitor.py`, `cricket_score.py` and `verify_board.py` before flashing; they hold the port.

---

## 13. Project layout

```
platformio.ini              build settings: ESP32-C3, libraries, USB serial on, 1.9 MB app partition
src/main.cpp                the whole firmware (display, web page, timer, eyes, alert, cricket, self-test)
arduino/ESP32C3_LED_MATRIX/ the same firmware as an Arduino IDE sketch
tools/verify_board.py       one-command check of a flashed board over USB
tools/break_monitor.py      PC-side break nagger (Windows)
tools/break_monitor.bat     double-click launcher for the monitor
tools/cricket_score.py      PC-side live cricket score feed
tools/cricket_score.bat     double-click launcher for the cricket feed
tools/sync_ino.py           copies src/main.cpp to the Arduino sketch
tools/requirements.txt      Python packages for the tools (pyserial)
```

---

## 14. Customising

Everything tunable is a `#define` near the top of [`src/main.cpp`](src/main.cpp):

| Define | Default | Meaning |
|--------|---------|---------|
| `MAX_DEVICES` | 4 | Number of 8x8 modules in the chain |
| `DIN_PIN`, `CLK_PIN`, `CS_PIN` | 0, 1, 2 | Wiring |
| `WIFI_TX_POWER` | 8.5 dBm | Raise for boards with a real antenna |
| `DONE_BLINK_MS` | 10000 | How long the strip flashes when a timer ends |
| `EYE_W`, `EYE_H`, `LOOK_RANGE`, `SWEEP_HOLD_MS`, `BLINK_MS` | 11, 6, 3, 2000, 220 | Eye size, how far the pupils travel, hold time per side, blink length |
| `ALERT_FLASH_MS` | 1200 | Flashing before the alert text starts |
| `CRK_PAGE_MS`, `CRK_MAIN_PAGE_MS`, `CRK_SCROLL_MS` | 3500, 6000, 45 | Cricket: time a page stays up, time the score page stays up, scroll speed in ms per pixel |
| `CRK_STALE_MS` | 180000 | Cricket: close the scoreboard after this long without an update |
| `NET_POLL_MS`, `NET_FIND_MS` | 10000, 60000 | Live scores over Wi-Fi: how often the score is checked during a match, and how often the board looks for a match when none is live |
| `TIMER_DEBUG` | 1 | Set to 0 to stop logging every timer step |
| `ssid` | ESP32C3 LED_MATRIX | Wi-Fi network name |

The message and the timings of the break monitor are command-line options, see section 7.

---

## 15. Credits

- Original scrolling-text project and web page: **Roy Cuadra**.
- Timer, eyes, break monitor, cricket scoreboard, self-test, captive portal and the tools: added on top of it.
- Eye animation style after the **Nico DeskBuddy** project.
- Libraries: [MD_Parola](https://github.com/MajicDesigns/MD_Parola) and [MD_MAX72XX](https://github.com/MajicDesigns/MD_MAX72XX) by MajicDesigns, [ArduinoJson](https://arduinojson.org) by Benoît Blanchon, [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) and [AsyncTCP](https://github.com/ESP32Async/AsyncTCP) by ESP32Async.

This project is for educational and hobby use. Handle electronics safely.
