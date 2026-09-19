#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <MD_MAX72xx.h>
#include <MD_Parola.h>
#include <SPI.h>
#include <esp_system.h>

// ─────────────────────────────────────────────────────────────────────────────
//  MAX7219 LED matrix configuration
// ─────────────────────────────────────────────────────────────────────────────
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4
#define DIN_PIN 0
#define CLK_PIN 1
#define CS_PIN 2

// Limits and tuning
#define TEXT_MAX_LEN      127        // max characters of display text
#define SPEED_MIN         10         // ms per scroll frame
#define SPEED_MAX         500
#define TIMER_MAX_SEC     86400UL    // 24 hours
#define DONE_BLINK_MS     10000UL    // flash the whole matrix this long when the timer ends
#define DONE_BLINK_PERIOD 250UL      // ms per on/off half-cycle while flashing
#define TIMER_DEBUG       1          // 1 = log every bar step with a timestamp on the serial monitor
// Module wiring variant, index 0..7 into MODULE_NAMES below (4 = FC-16). Can be
// changed at runtime from the web page or serial console and is saved in flash.
#define MODULE_TYPE_DEFAULT 4

MD_Parola P = MD_Parola(HARDWARE_TYPE, DIN_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
MD_MAX72XX *mx = nullptr;            // raw driver, used for direct pixel drawing in timer mode

// Timer bar geometry. The 32x8 strip is held upright, so it becomes 8 LEDs
// wide and 32 LEDs tall. Each hardware column is one row of the bar, and each
// of the 8 bits in that column is one LED across that row.
const uint16_t BAR_ROWS   = MAX_DEVICES * 8;   // 32 rows, bottom to top
const uint16_t BAR_LEVELS = BAR_ROWS * 8;      // 256 LEDs in total

// ─────────────────────────────────────────────────────────────────────────────
//  Wi-Fi access point
// ─────────────────────────────────────────────────────────────────────────────
const char *ssid = "ESP32C3 LED_MATRIX";
#define WIFI_CHANNEL     1
#define WIFI_MAX_CLIENTS 4
#define DNS_PORT         53
// Transmit power. Small boards with a PCB-trace antenna (ESP32-C3 SuperMini and
// similar) are often invisible or refuse connections at full power because the
// antenna is mismatched. 8.5 dBm is the widely used fix and still covers a room.
// Boards with a proper or external antenna can go up to WIFI_POWER_19_5dBm.
#define WIFI_TX_POWER    WIFI_POWER_8_5dBm

AsyncWebServer server(80);
DNSServer      dnsServer;   // answers every hostname with our IP (captive portal)

// ─────────────────────────────────────────────────────────────────────────────
//  Display state. Owned by loop(); the web task only reads it for /status.
// ─────────────────────────────────────────────────────────────────────────────
enum DisplayMode : uint8_t { MODE_TEXT, MODE_TIMER, MODE_DONE, MODE_SELFTEST, MODE_EYES, MODE_ALERT };

volatile DisplayMode mode = MODE_TEXT;
char displayText[TEXT_MAX_LEN + 1] = "WELCOME!";
int  scrollSpeed = 50;
bool textStatic  = false;   // true when the text fits on the strip and is shown still, centred

unsigned long timerStartMs    = 0;
unsigned long timerDurationMs = 0;
bool          timerFlip       = false;   // true = bar grows from the other end of the strip
int           lastLevel       = -1;
unsigned long doneStartMs     = 0;
bool          doneLit         = false;

// Module type (how the MAX7219 digits/segments map to rows/columns) and the
// display self-test. Both exist to sort out wiring and module variants.
Preferences prefs;
uint8_t     moduleTypeIdx = MODULE_TYPE_DEFAULT;
static const char *const MODULE_NAMES[8] = {
  "DR0CR0RR0", "DR0CR0RR1", "DR0CR1RR0 (Generic)", "DR0CR1RR1",
  "DR1CR0RR0 (FC-16)", "DR1CR0RR1", "DR1CR1RR0 (Parola)", "DR1CR1RR1 (ICStation)"
};

struct SelfStep { uint16_t ms; const char *what; };
static const SelfStep SELF_STEPS[] = {
  {3000, "ALL ON - every LED lit; a dark LED is a wiring or module fault"},
  {1000, "ALL OFF"},
  {1500, "Module 1 only - the one nearest the connector"},
  {1500, "Module 2 only"},
  {1500, "Module 3 only"},
  {1500, "Module 4 only - farthest from the connector"},
  {3000, "ROW - one straight line along the whole long edge of the strip"},
  {3000, "COLUMN - one 8-LED line across the strip, right at the connector end"},
  {2000, "PIXEL - a single LED in the corner at the connector end"},
};
static const uint8_t SELF_STEP_COUNT = sizeof(SELF_STEPS) / sizeof(SELF_STEPS[0]);
uint8_t       selfStep      = 0;
unsigned long selfStepStart = 0;

// ─────────────────────────────────────────────────────────────────────────────
//  Animated eyes (after the Nico DeskBuddy eyes, scaled to 32x8).
//  Logical coordinates: x 0..31 left to right, y 0..7 top to bottom.
//  Hardware column = 31 - x (column 0 is at the connector end), bit y = row.
// ─────────────────────────────────────────────────────────────────────────────
#define EYES_FRAME_MS   25        // 40 fps
#define EYE_GAP_LEFT    13        // first column of the gap between the eyes
#define EYE_GAP_RIGHT   19        // first column of the right eye
#define EYE_W           11
#define EYE_H           6
#define LOOK_RANGE      3         // pupil travel each side of centre, in pixels
#define SWEEP_HOLD_MS   2000      // how long the eyes hold each side before sweeping back
#define BLINK_MS        220
#define EYE_K           0.20f     // eye box spring stiffness / damping
#define EYE_D           0.60f
#define PUPIL_K         0.15f
#define PUPIL_D         0.60f

struct Eye {
  float x, y, w, h,   tx, ty, tw, th,   vx, vy, vw, vh;   // box, its target and velocity
  float px, py,       tpx, tpy,         vpx, vpy;         // pupil offset from the eye centre

  void init(float _x, float _y, float _w, float _h) {
    x = tx = _x; y = ty = _y; w = tw = _w; h = th = _h;
    vx = vy = vw = vh = 0;
    px = py = tpx = tpy = vpx = vpy = 0;
  }
  void update() {
    vx = (vx + (tx - x) * EYE_K) * EYE_D;  x += vx;
    vy = (vy + (ty - y) * EYE_K) * EYE_D;  y += vy;
    vw = (vw + (tw - w) * EYE_K) * EYE_D;  w += vw;
    vh = (vh + (th - h) * EYE_K) * EYE_D;  h += vh;
    vpx = (vpx + (tpx - px) * PUPIL_K) * PUPIL_D;  px += vpx;
    vpy = (vpy + (tpy - py) * PUPIL_K) * PUPIL_D;  py += vpy;
  }
};

Eye           eyeL, eyeR;
float         lookX          = LOOK_RANGE;   // where the pupils point: +LOOK_RANGE right, -LOOK_RANGE left
bool          blinking       = false;
unsigned long blinkStartMs   = 0;
unsigned long nextBlinkMs    = 0;
unsigned long nextSweepMs    = 0;
unsigned long lastEyeFrameMs = 0;
uint8_t       eyesFb[32];                    // frame being built
uint8_t       eyesLastFb[32];                // frame on the display
bool          eyesDirty      = true;         // force a push
uint8_t       idleMode       = 0;            // 0 = text, 1 = eyes (saved in flash)

// ─────────────────────────────────────────────────────────────────────────────
//  Break alert: a message that flashes, then scrolls until cleared. Sent by
//  tools/break_monitor.py on the PC when you have worked too long.
// ─────────────────────────────────────────────────────────────────────────────
#define ALERT_FLASH_MS     1200UL   // full-matrix flashing before the text starts
#define ALERT_FLASH_PERIOD 150UL
char          alertText[TEXT_MAX_LEN + 1] = "";
unsigned long alertStartMs = 0;
unsigned long alertMs      = 0;      // 0 = stays until cleared
uint8_t       alertPhase   = 0;      // 0 flashing, 1 scrolling
bool          alertFlashOn = false;
bool          alertQueued  = false;  // arrived during a timer; shown when the timer ends

// ─────────────────────────────────────────────────────────────────────────────
//  Commands from the web server. Handlers run on the AsyncTCP task, not in
//  loop(), so they never touch the display. They queue one command here and
//  loop() applies it on its next pass.
// ─────────────────────────────────────────────────────────────────────────────
enum Command : uint8_t { CMD_NONE, CMD_SET_TEXT, CMD_START_TIMER, CMD_CANCEL_TIMER, CMD_SELFTEST, CMD_SET_MODULE,
                         CMD_SHOW_EYES, CMD_SHOW_TEXT, CMD_ALERT, CMD_CLEAR_ALERT };

portMUX_TYPE  cmdMux         = portMUX_INITIALIZER_UNLOCKED;
Command       pendingCmd     = CMD_NONE;
char          pendingText[TEXT_MAX_LEN + 1] = "";
int           pendingSpeed   = 50;
unsigned long pendingTimerMs = 0;
bool          pendingFlip    = false;
uint8_t       pendingModule  = MODULE_TYPE_DEFAULT;
unsigned long pendingAlertMs = 0;

// ─────────────────────────────────────────────────────────────────────────────
//  Web page (kept in flash, served without copying to RAM)
// ─────────────────────────────────────────────────────────────────────────────
static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 LED Matrix Control</title>
  <style>
    * {
      margin: 0;
      padding: 0;
      box-sizing: border-box;
    }

    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, Cantarell, sans-serif;
      background: #0a0a0a;
      color: #ffffff;
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      position: relative;
      overflow-x: hidden;
    }

    /* Animated background */
    body::before {
      content: '';
      position: fixed;
      top: 0;
      left: 0;
      width: 100%;
      height: 100%;
      background: radial-gradient(circle at 25% 25%, #1a1a2e 0%, transparent 50%),
                  radial-gradient(circle at 75% 75%, #16213e 0%, transparent 50%),
                  linear-gradient(135deg, #0f0f0f 0%, #1a1a1a 100%);
      z-index: -2;
    }

    /* Subtle grid pattern */
    body::after {
      content: '';
      position: fixed;
      top: 0;
      left: 0;
      width: 100%;
      height: 100%;
      background-image:
        linear-gradient(rgba(255,255,255,0.02) 1px, transparent 1px),
        linear-gradient(90deg, rgba(255,255,255,0.02) 1px, transparent 1px);
      background-size: 20px 20px;
      z-index: -1;
    }

    main {
      flex: 1;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 2rem 1rem;
    }

    .cards {
      display: flex;
      flex-direction: column;
      gap: 1.5rem;
      width: 100%;
      max-width: 480px;
    }

    .container {
      background: rgba(255, 255, 255, 0.03);
      backdrop-filter: blur(20px);
      border: 1px solid rgba(255, 255, 255, 0.1);
      padding: 3rem 2rem;
      border-radius: 24px;
      width: 100%;
      box-shadow:
        0 25px 50px -12px rgba(0, 0, 0, 0.25),
        0 0 0 1px rgba(255, 255, 255, 0.05),
        inset 0 1px 0 rgba(255, 255, 255, 0.1);
      position: relative;
    }

    .container::before {
      content: '';
      position: absolute;
      top: 0;
      left: 0;
      right: 0;
      height: 1px;
      background: linear-gradient(90deg, transparent, rgba(255, 255, 255, 0.2), transparent);
    }

    h1 {
      font-size: 1.875rem;
      font-weight: 700;
      margin-bottom: 2rem;
      background: linear-gradient(135deg, #ffffff, #a0a0a0);
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
      background-clip: text;
      text-align: center;
      letter-spacing: -0.025em;
    }

    .hint {
      color: #6b7280;
      font-size: 0.875rem;
      text-align: center;
      line-height: 1.5;
      margin: -1.25rem 0 1.75rem;
    }

    .form-group {
      margin-bottom: 1.5rem;
      position: relative;
    }

    .form-group:last-child {
      margin-bottom: 0;
    }

    .row {
      display: flex;
      gap: 1rem;
      margin-bottom: 1.5rem;
    }

    .row .form-group {
      flex: 1;
      margin-bottom: 0;
    }

    label {
      display: block;
      margin-bottom: 0.5rem;
      font-size: 0.875rem;
      font-weight: 500;
      color: #a0a0a0;
      text-transform: uppercase;
      letter-spacing: 0.05em;
    }

    input[type="text"],
    input[type="number"] {
      width: 100%;
      padding: 1rem 1.25rem;
      background: rgba(255, 255, 255, 0.05);
      border: 1px solid rgba(255, 255, 255, 0.1);
      border-radius: 12px;
      color: #ffffff;
      font-size: 1rem;
      font-weight: 400;
      transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);
      outline: none;
    }

    input[type="text"]:focus,
    input[type="number"]:focus {
      border-color: rgba(99, 102, 241, 0.5);
      box-shadow: 0 0 0 3px rgba(99, 102, 241, 0.1);
      background: rgba(255, 255, 255, 0.08);
    }

    input[type="text"]::placeholder,
    input[type="number"]::placeholder {
      color: #6b7280;
    }

    .check {
      display: flex;
      align-items: center;
      gap: 0.65rem;
      text-transform: none;
      letter-spacing: 0;
      font-size: 0.875rem;
      color: #a0a0a0;
      margin-bottom: 1.5rem;
      cursor: pointer;
      user-select: none;
    }

    .check input {
      width: 1.1rem;
      height: 1.1rem;
      accent-color: #6366f1;
      cursor: pointer;
    }

    select {
      width: 100%;
      padding: 1rem 1.25rem;
      background: rgba(255, 255, 255, 0.05);
      border: 1px solid rgba(255, 255, 255, 0.1);
      border-radius: 12px;
      color: #ffffff;
      font-size: 1rem;
      outline: none;
      -webkit-appearance: none;
      appearance: none;
      cursor: pointer;
    }

    select:focus {
      border-color: rgba(99, 102, 241, 0.5);
      box-shadow: 0 0 0 3px rgba(99, 102, 241, 0.1);
    }

    select option {
      background: #1a1a2e;
      color: #ffffff;
    }

    .steps {
      margin: 1.5rem 0 1rem 1.25rem;
      color: #a0a0a0;
      font-size: 0.85rem;
      line-height: 1.7;
    }

    .steps li.active {
      color: #ffffff;
      font-weight: 600;
    }

    .timer-readout {
      text-align: center;
      margin-bottom: 1.5rem;
    }

    .time {
      font-size: 2.75rem;
      font-weight: 700;
      font-variant-numeric: tabular-nums;
      letter-spacing: 0.05em;
    }

    .bar {
      height: 10px;
      background: rgba(255, 255, 255, 0.08);
      border-radius: 5px;
      overflow: hidden;
      margin: 0.75rem 0 0.5rem;
    }

    .bar-fill {
      height: 100%;
      width: 0;
      background: linear-gradient(90deg, #6366f1, #8b5cf6);
      transition: width 0.5s linear;
    }

    .timer-state {
      font-size: 0.8rem;
      color: #6b7280;
    }

    .btn-row {
      display: flex;
      gap: 0.75rem;
    }

    .btn-row .submit-btn {
      flex: 1;
    }

    .submit-btn {
      width: 100%;
      padding: 1rem 1.5rem;
      background: linear-gradient(135deg, #6366f1, #8b5cf6);
      border: none;
      border-radius: 12px;
      color: #ffffff;
      font-size: 1rem;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);
      position: relative;
      overflow: hidden;
      text-transform: uppercase;
      letter-spacing: 0.025em;
    }

    .submit-btn::before {
      content: '';
      position: absolute;
      top: 0;
      left: -100%;
      width: 100%;
      height: 100%;
      background: linear-gradient(90deg, transparent, rgba(255, 255, 255, 0.2), transparent);
      transition: left 0.5s;
    }

    .submit-btn:hover {
      transform: translateY(-2px);
      box-shadow: 0 20px 25px -5px rgba(99, 102, 241, 0.3);
      background: linear-gradient(135deg, #5856ec, #7c3aed);
    }

    .submit-btn:hover::before {
      left: 100%;
    }

    .submit-btn:active {
      transform: translateY(0);
    }

    .submit-btn:disabled {
      opacity: 0.5;
      cursor: not-allowed;
      transform: none;
      box-shadow: none;
    }

    .submit-btn.secondary {
      background: rgba(255, 255, 255, 0.08);
      border: 1px solid rgba(255, 255, 255, 0.15);
    }

    .submit-btn.secondary:hover {
      background: rgba(255, 255, 255, 0.14);
      box-shadow: none;
    }

    /* Status indicator */
    .status {
      margin-top: 1.5rem;
      padding: 1rem;
      border-radius: 8px;
      font-size: 0.875rem;
      font-weight: 500;
      text-align: center;
      opacity: 0;
      transform: translateY(10px);
      transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);
    }

    .status.show {
      opacity: 1;
      transform: translateY(0);
    }

    .status.success {
      background: rgba(34, 197, 94, 0.1);
      color: #22c55e;
      border: 1px solid rgba(34, 197, 94, 0.2);
    }

    .status.error {
      background: rgba(239, 68, 68, 0.1);
      color: #ef4444;
      border: 1px solid rgba(239, 68, 68, 0.2);
    }

    /* Footer */
    footer {
      padding: 2rem 1rem;
      text-align: center;
      border-top: 1px solid rgba(255, 255, 255, 0.1);
      background: rgba(0, 0, 0, 0.3);
      backdrop-filter: blur(10px);
    }

    .footer-content {
      max-width: 1200px;
      margin: 0 auto;
    }

    .footer-text {
      font-size: 0.875rem;
      color: #6b7280;
      margin-bottom: 0.5rem;
    }

    .footer-credit {
      font-size: 0.875rem;
      color: #a0a0a0;
      font-weight: 500;
    }

    .footer-credit .highlight {
      background: linear-gradient(135deg, #6366f1, #8b5cf6);
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
      background-clip: text;
      font-weight: 600;
    }

    /* Mobile responsiveness */
    @media (max-width: 640px) {
      .container {
        padding: 2rem 1.5rem;
        border-radius: 16px;
      }

      h1 {
        font-size: 1.5rem;
        margin-bottom: 1.5rem;
      }

      .hint {
        margin-top: -0.75rem;
      }

      input[type="text"],
      input[type="number"] {
        padding: 0.875rem 1rem;
        font-size: 0.95rem;
      }

      .submit-btn {
        padding: 0.875rem 1.25rem;
        font-size: 0.95rem;
      }

      .time {
        font-size: 2.25rem;
      }

      footer {
        padding: 1.5rem 1rem;
      }

      .footer-text,
      .footer-credit {
        font-size: 0.8rem;
      }
    }

    @media (max-width: 480px) {
      main {
        padding: 1rem 0.5rem;
      }

      .container {
        padding: 1.5rem 1rem;
      }

      h1 {
        font-size: 1.25rem;
      }
    }

    /* Animations */
    @keyframes fadeInUp {
      from {
        opacity: 0;
        transform: translateY(30px);
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }

    .container {
      animation: fadeInUp 0.8s cubic-bezier(0.4, 0, 0.2, 1);
    }
  </style>
</head>
<body>
  <main>
    <div class="cards">

      <div class="container">
        <h1>ESP32 LED Matrix Control</h1>
        <form id="matrixForm">
          <div class="form-group">
            <label for="text">Display Text</label>
            <input type="text" id="text" placeholder="Enter your message" maxlength="127" required>
          </div>

          <div class="form-group">
            <label for="speed">Scroll Speed (ms)</label>
            <input type="number" id="speed" placeholder="50" min="10" max="500" value="50" required>
          </div>

          <div class="form-group">
            <button type="submit" class="submit-btn" id="submitBtn">
              Display Message
            </button>
          </div>

          <div class="status" id="status"></div>
        </form>
      </div>

      <div class="container">
        <h1>Eyes</h1>
        <p class="hint">
          Animated eyes that look left, look right and blink. Whatever you choose here,
          eyes or text, becomes the resting screen and comes back after a timer.
        </p>
        <div class="btn-row">
          <button type="button" class="submit-btn" id="eyesBtn">Show Eyes</button>
          <button type="button" class="submit-btn secondary" id="textBtn">Show Text</button>
        </div>
        <div class="timer-state" id="eyesState" style="margin-top: 1rem; text-align: center;"></div>
        <div class="status" id="eyesStatus"></div>
      </div>

      <div class="container">
        <h1>Break Monitor</h1>
        <p class="hint">
          Run tools/break_monitor.py on your PC. After 15 minutes of work it makes the matrix
          flash and scroll a message until you have stepped away for 3 minutes. Both times are
          settable on the command line.
        </p>
        <div class="timer-state" id="alertState" style="text-align: center; margin-bottom: 1.5rem;">No alert</div>
        <div class="btn-row">
          <button type="button" class="submit-btn" id="testAlertBtn">Test Alert (10 s)</button>
          <button type="button" class="submit-btn secondary" id="dismissBtn" disabled>Dismiss</button>
        </div>
        <div class="status" id="alertStatus"></div>
      </div>

      <div class="container">
        <h1>Countdown Timer</h1>
        <p class="hint">
          Stand the matrix upright with the connector at the bottom. The bar fills from the bottom
          and is fully lit when time is up, then the whole matrix flashes for a few seconds.
        </p>
        <form id="timerForm">
          <div class="row">
            <div class="form-group">
              <label for="minutes">Minutes</label>
              <input type="number" id="minutes" min="0" max="1440" value="5" required>
            </div>
            <div class="form-group">
              <label for="seconds">Seconds</label>
              <input type="number" id="seconds" min="0" max="59" value="0" required>
            </div>
          </div>

          <label class="check">
            <input type="checkbox" id="flip">
            Fill from the other end (tick this if the bar grows downward)
          </label>

          <div class="timer-readout">
            <div class="time" id="timeLeft">--:--</div>
            <div class="bar"><div class="bar-fill" id="barFill"></div></div>
            <div class="timer-state" id="timerState">No timer running</div>
          </div>

          <div class="btn-row">
            <button type="submit" class="submit-btn" id="startBtn">Start Timer</button>
            <button type="button" class="submit-btn secondary" id="cancelBtn" disabled>Cancel</button>
          </div>

          <div class="status" id="timerStatus"></div>
        </form>
      </div>

      <div class="container">
        <h1>Display Setup</h1>
        <p class="hint">
          Only needed when the text does not look right. Run the self-test, compare each step with
          the list, and change the module type until every step looks as described. The choice is
          saved on the board.
        </p>
        <div class="form-group">
          <label for="module">Module type</label>
          <select id="module">
            <option value="4">FC-16 (DR1 CR0 RR0) - default</option>
            <option value="2">Generic (DR0 CR1 RR0)</option>
            <option value="6">Parola (DR1 CR1 RR0)</option>
            <option value="7">ICStation (DR1 CR1 RR1)</option>
            <option value="0">DR0 CR0 RR0</option>
            <option value="1">DR0 CR0 RR1</option>
            <option value="3">DR0 CR1 RR1</option>
            <option value="5">DR1 CR0 RR1</option>
          </select>
        </div>
        <div class="btn-row">
          <button type="button" class="submit-btn" id="selftestBtn">Run Self-Test</button>
        </div>
        <ol class="steps" id="steps">
          <li>All LEDs on. Any dark LED is a wiring or module fault.</li>
          <li>All LEDs off.</li>
          <li>Module 1 only: the one nearest the connector.</li>
          <li>Module 2 only.</li>
          <li>Module 3 only.</li>
          <li>Module 4 only: farthest from the connector.</li>
          <li>Row: one straight line along the whole long edge of the strip.</li>
          <li>Column: one 8-LED line across the strip, right at the connector end.</li>
          <li>Pixel: a single LED in the corner at the connector end.</li>
        </ol>
        <p class="hint" style="margin: 0 0 1.5rem;">
          Row step shows four short lines across the strip: DR is wrong. Row step line is on the
          wrong long edge: RR is wrong. Column step line sits one module in instead of at the
          very end: CR is wrong.
        </p>
        <div class="status" id="setupStatus"></div>
      </div>

    </div>
  </main>

  <footer>
    <div class="footer-content">
      <div class="footer-text">ESP32 LED Matrix Control Interface</div>
      <div class="footer-credit">Created by <span class="highlight">Roy Cuadra</span></div>
    </div>
  </footer>

  <script>
    const $ = (id) => document.getElementById(id);

    const form      = $('matrixForm');
    const submitBtn = $('submitBtn');
    const timerForm = $('timerForm');
    const startBtn  = $('startBtn');
    const cancelBtn = $('cancelBtn');
    const moduleSel   = $('module');
    const selftestBtn = $('selftestBtn');
    const eyesBtn     = $('eyesBtn');
    const textBtn     = $('textBtn');
    const testAlertBtn = $('testAlertBtn');
    const dismissBtn   = $('dismissBtn');

    let currentMode = 'text';
    let pollHandle  = null;

    function showStatus(el, message, type = 'success') {
      el.textContent = message;
      el.className = `status ${type} show`;
      clearTimeout(el._hide);
      el._hide = setTimeout(() => el.classList.remove('show'), 3000);
    }

    function fmtTime(total) {
      const h = Math.floor(total / 3600);
      const m = Math.floor((total % 3600) / 60);
      const s = total % 60;
      const p = (n) => String(n).padStart(2, '0');
      return h > 0 ? `${h}:${p(m)}:${p(s)}` : `${p(m)}:${p(s)}`;
    }

    function render(st) {
      currentMode = st.mode;
      cancelBtn.disabled = (st.mode === 'text' || st.mode === 'eyes');
      $('eyesState').textContent = (st.mode === 'eyes') ? 'Eyes are showing now'
        : 'Resting screen: ' + (st.idle === 'eyes' ? 'eyes' : 'scrolling text');
      $('alertState').textContent = (st.mode === 'alert') ? 'Showing: ' + st.alert
        : (st.alert ? 'Queued until the timer ends: ' + st.alert : 'No alert');
      dismissBtn.disabled = !st.alert;

      if (st.mode === 'timer') {
        const pct = st.total ? Math.round(100 * (st.total - st.remaining) / st.total) : 0;
        $('timeLeft').textContent = fmtTime(st.remaining);
        $('barFill').style.width = pct + '%';
        $('timerState').textContent = 'Running - ' + pct + '% of the matrix lit';
      } else if (st.mode === 'done') {
        $('timeLeft').textContent = '00:00';
        $('barFill').style.width = '100%';
        $('timerState').textContent = "Time's up! The matrix is flashing.";
      } else {
        $('timeLeft').textContent = '--:--';
        $('barFill').style.width = '0%';
        $('timerState').textContent = (st.mode === 'selftest') ? 'Display self-test running' : 'No timer running';
      }

      document.querySelectorAll('#steps li').forEach((li, i) => {
        li.classList.toggle('active', st.mode === 'selftest' && i === st.step);
      });

      if (st.mode !== 'text' && st.mode !== 'eyes') startPolling(); else stopPolling();
    }

    async function refresh(populateFields = false) {
      try {
        const r = await fetch('/status', { cache: 'no-store' });
        if (!r.ok) throw new Error('status ' + r.status);
        const st = await r.json();
        if (populateFields) {
          $('text').value  = st.text;
          $('speed').value = st.speed;
          $('flip').checked = st.flip;
          moduleSel.value  = String(st.module);
        }
        render(st);
      } catch (err) {
        console.error('Status fetch failed:', err);
      }
    }

    function startPolling() {
      if (!pollHandle) pollHandle = setInterval(() => refresh(false), 1000);
    }

    function stopPolling() {
      if (pollHandle) { clearInterval(pollHandle); pollHandle = null; }
    }

    // ── Text form ──────────────────────────────────────────────────────────
    form.addEventListener('submit', async (e) => {
      e.preventDefault();
      const status = $('status');
      const text  = $('text').value.trim();
      const speed = $('speed').value;

      if (!text) {
        showStatus(status, 'Please enter some text to display', 'error');
        return;
      }

      submitBtn.disabled = true;
      submitBtn.textContent = 'Updating...';

      try {
        const response = await fetch('/setText?text=' + encodeURIComponent(text) + '&speed=' + encodeURIComponent(speed));
        if (!response.ok) throw new Error(await response.text());
        if (currentMode === 'text') {
          showStatus(status, 'Text and scroll speed updated successfully!', 'success');
        } else {
          showStatus(status, 'Saved. The text will show once the timer ends.', 'success');
        }
      } catch (error) {
        console.error('Error:', error);
        showStatus(status, 'Failed to update display. Please try again.', 'error');
      } finally {
        submitBtn.disabled = false;
        submitBtn.textContent = 'Display Message';
      }
    });

    // ── Timer form ─────────────────────────────────────────────────────────
    timerForm.addEventListener('submit', async (e) => {
      e.preventDefault();
      const status  = $('timerStatus');
      const minutes = parseInt($('minutes').value, 10) || 0;
      const seconds = parseInt($('seconds').value, 10) || 0;
      const total   = minutes * 60 + seconds;

      if (total < 1) {
        showStatus(status, 'Set at least 1 second', 'error');
        return;
      }
      if (total > 86400) {
        showStatus(status, 'Maximum timer is 24 hours', 'error');
        return;
      }

      startBtn.disabled = true;
      startBtn.textContent = 'Starting...';

      try {
        const flip = $('flip').checked ? 1 : 0;
        const response = await fetch('/startTimer?seconds=' + total + '&flip=' + flip);
        if (!response.ok) throw new Error(await response.text());
        showStatus(status, 'Timer started for ' + fmtTime(total), 'success');
        await refresh(false);
      } catch (error) {
        console.error('Error:', error);
        showStatus(status, 'Failed to start timer. Please try again.', 'error');
      } finally {
        startBtn.disabled = false;
        startBtn.textContent = 'Start Timer';
      }
    });

    cancelBtn.addEventListener('click', async () => {
      const status = $('timerStatus');
      cancelBtn.disabled = true;
      try {
        const response = await fetch('/cancelTimer');
        if (!response.ok) throw new Error(await response.text());
        showStatus(status, 'Timer cancelled. Back to scrolling text.', 'success');
        await refresh(false);
      } catch (error) {
        console.error('Error:', error);
        showStatus(status, 'Failed to cancel timer. Please try again.', 'error');
        cancelBtn.disabled = false;
      }
    });

    // ── Eyes ───────────────────────────────────────────────────────────────
    async function simpleCall(url, statusEl, okMessage, failMessage) {
      try {
        const response = await fetch(url);
        if (!response.ok) throw new Error(await response.text());
        showStatus(statusEl, okMessage, 'success');
        await refresh(false);
      } catch (error) {
        console.error('Error:', error);
        showStatus(statusEl, failMessage, 'error');
      }
    }

    eyesBtn.addEventListener('click', () =>
      simpleCall('/eyes', $('eyesStatus'), 'Eyes are on the matrix', 'Failed to show eyes'));

    textBtn.addEventListener('click', () =>
      simpleCall('/showText', $('eyesStatus'), 'Scrolling text is back', 'Failed to show text'));

    // ── Break monitor ──────────────────────────────────────────────────────
    testAlertBtn.addEventListener('click', () =>
      simpleCall('/alert?text=' + encodeURIComponent('BITCH TAKE A BREAK') + '&seconds=10', $('alertStatus'),
                 'Test alert on the matrix for 10 seconds', 'Failed to send alert'));

    dismissBtn.addEventListener('click', () =>
      simpleCall('/clearAlert', $('alertStatus'), 'Alert dismissed', 'Failed to dismiss'));

    // ── Display setup ──────────────────────────────────────────────────────
    moduleSel.addEventListener('change', async () => {
      const status = $('setupStatus');
      try {
        const response = await fetch('/setModule?type=' + encodeURIComponent(moduleSel.value));
        if (!response.ok) throw new Error(await response.text());
        showStatus(status, 'Module type applied and saved on the board', 'success');
        await refresh(false);
      } catch (error) {
        console.error('Error:', error);
        showStatus(status, 'Failed to change module type', 'error');
      }
    });

    selftestBtn.addEventListener('click', async () => {
      const status = $('setupStatus');
      selftestBtn.disabled = true;
      try {
        const response = await fetch('/selftest');
        if (!response.ok) throw new Error(await response.text());
        showStatus(status, 'Self-test running for about 18 seconds. Watch the strip.', 'success');
        await refresh(false);
      } catch (error) {
        console.error('Error:', error);
        showStatus(status, 'Failed to start self-test', 'error');
      } finally {
        selftestBtn.disabled = false;
      }
    });

    // Interactive feedback for inputs
    document.querySelectorAll('input[type="text"], input[type="number"]').forEach(input => {
      input.addEventListener('focus', (e) => {
        e.target.parentElement.style.transform = 'scale(1.02)';
      });
      input.addEventListener('blur', (e) => {
        e.target.parentElement.style.transform = 'scale(1)';
      });
    });

    // Load the current state so the page reflects what the matrix is doing
    refresh(true);
  </script>
</body>
</html>
)rawliteral";

// ─────────────────────────────────────────────────────────────────────────────
//  Display helpers (called from loop() only)
// ─────────────────────────────────────────────────────────────────────────────

// Show the text: still and centred if it fits on the strip, scrolling otherwise.
static void startTextMode() {
  mode = MODE_TEXT;
  P.displayClear();
  textStatic = P.getTextColumns(displayText) <= mx->getColumnCount();
  if (textStatic) P.displayText(displayText, PA_CENTER, scrollSpeed, 0, PA_PRINT, PA_NO_EFFECT);
  else            P.displayText(displayText, PA_CENTER, scrollSpeed, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  Serial.printf("Mode: text (%s)\n", textStatic ? "still, centred" : "scrolling");
}

// Light every LED on or off. Used for the end-of-timer flash.
static void fillAll(bool on) {
  mx->control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);
  for (uint16_t c = 0; c < BAR_ROWS; c++) {
    mx->setColumn(c, on ? 0xFF : 0x00);
  }
  mx->control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);   // one flush for the whole strip
}

// Light `level` LEDs (0..BAR_LEVELS) of the upright bar, starting from the
// bottom. Whole rows fill first; the row in progress fills LED by LED, which
// gives 256 visible steps instead of 32.
static void drawBar(uint16_t level) {
  if (level > BAR_LEVELS) level = BAR_LEVELS;
  const uint16_t fullRows = level / 8;
  const uint8_t  partial  = level % 8;
#if TIMER_DEBUG
  Serial.printf("[%lu ms] bar %u/%u  (elapsed %lu of %lu ms)\n",
                millis(), level, BAR_LEVELS, millis() - timerStartMs, timerDurationMs);
#endif

  mx->control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);
  for (uint16_t r = 0; r < BAR_ROWS; r++) {
    uint8_t value;
    if (r < fullRows) {
      value = 0xFF;
    } else if (r == fullRows) {
      // Partial row: fill bits from one side. Flipping the strip 180° also
      // mirrors the row, so the bit direction flips together with the row order.
      value = timerFlip ? (uint8_t)(0xFF << (8 - partial)) : (uint8_t)(0xFF >> (8 - partial));
    } else {
      value = 0x00;
    }
    const uint16_t col = timerFlip ? (BAR_ROWS - 1 - r) : r;
    mx->setColumn(col, value);
  }
  mx->control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);
}

static void startTimer(unsigned long durationMs, bool flip) {
  mode            = MODE_TIMER;
  timerDurationMs = durationMs;
  timerFlip       = flip;
  timerStartMs    = millis();
  lastLevel       = -1;          // force the first draw
  P.displayClear();
  if (prefs.getBool("flip", false) != flip) prefs.putBool("flip", flip);   // remember orientation
  Serial.printf("Mode: timer, %lu s, flip=%d\n", durationMs / 1000UL, flip ? 1 : 0);
}

static const char *moduleTypeName(uint8_t idx) {
  return (idx < 8) ? MODULE_NAMES[idx] : "?";
}

// Draw one self-test pattern. Everything goes through the same buffer path the
// text and the timer use, so it exercises wiring, chain order and the mapping.
static void selfTestDraw(uint8_t step) {
  mx->control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);
  mx->clear();
  switch (step) {
    case 0:
      for (uint16_t c = 0; c < BAR_ROWS; c++) mx->setColumn(c, 0xFF);
      break;
    case 1:
      break;
    case 2: case 3: case 4: case 5: {
      const uint16_t first = (uint16_t)(step - 2) * 8;
      for (uint16_t c = first; c < first + 8; c++) mx->setColumn(c, 0xFF);
      break;
    }
    case 6:
      mx->setRow(0, 0xFF);
      break;
    case 7:
      mx->setColumn(0, 0xFF);
      break;
    case 8:
      mx->setPoint(0, 0, true);
      break;
    default:
      break;
  }
  mx->control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);
  Serial.printf("Self-test %u/%u: %s\n", step + 1, SELF_STEP_COUNT, SELF_STEPS[step].what);
}

static void startSelfTest() {
  mode          = MODE_SELFTEST;
  selfStep      = 0;
  selfStepStart = millis();
  P.displayClear();
  Serial.println("Mode: self-test");
  selfTestDraw(0);
}

// Switch the row/column mapping at runtime and redraw whatever is showing.
static void applyModuleType(uint8_t idx, bool save) {
  if (idx > 7) idx = MODULE_TYPE_DEFAULT;
  moduleTypeIdx = idx;
  mx->setModuleType((MD_MAX72XX::moduleType_t)(MD_MAX72XX::DR0CR0RR0_HW + idx));
  if (save) prefs.putUChar("modtype", idx);
  Serial.printf("Module type %u: %s\n", idx, moduleTypeName(idx));
  switch (mode) {
    case MODE_TEXT:     startTextMode(); break;
    case MODE_TIMER:    P.displayClear(); lastLevel = -1; break;
    case MODE_DONE:     fillAll(doneLit); break;
    case MODE_SELFTEST: selfTestDraw(selfStep); break;
    case MODE_EYES:     P.displayClear(); eyesDirty = true; break;
    case MODE_ALERT:    P.displayClear(); alertPhase = 0; alertStartMs = millis(); alertFlashOn = true; fillAll(true); break;
    default:            break;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Eyes engine
// ─────────────────────────────────────────────────────────────────────────────
static inline void fbSet(int x, int y)   { if (x >= 0 && x < 32 && y >= 0 && y < 8) eyesFb[x] |=  (uint8_t)(1 << y); }
static inline void fbClear(int x, int y) { if (x >= 0 && x < 32 && y >= 0 && y < 8) eyesFb[x] &= (uint8_t)~(1 << y); }

static void renderEye(const Eye &e, float blinkPhase) {
  const int w     = max(1, (int)roundf(e.w));
  const int hFull = max(1, (int)roundf(e.h));
  const int h     = (blinkPhase > 0.0f) ? max(1, (int)roundf(hFull * (1.0f - blinkPhase))) : hFull;
  const int x0    = (int)roundf(e.x);
  const int y0    = (int)roundf(e.y + (hFull - h) / 2.0f);   // lids close toward the middle

  // Eye box with the corners knocked off
  for (int x = x0; x < x0 + w; x++) {
    for (int y = y0; y < y0 + h; y++) {
      const bool corner = (w >= 4 && h >= 3) && (x == x0 || x == x0 + w - 1) && (y == y0 || y == y0 + h - 1);
      if (!corner) fbSet(x, y);
    }
  }

  // Pupil (dark), kept one pixel inside the box
  if (h >= 3 && w >= 4) {
    const int pw = max(2, (int)roundf(w / 2.5f));
    const int ph = max(1, (int)roundf(h / 2.5f));
    int px0 = x0 + w / 2 + (int)roundf(e.px) - pw / 2;
    int py0 = y0 + h / 2 + (int)roundf(e.py) - ph / 2;
    px0 = constrain(px0, x0 + 1, x0 + w - 1 - pw);
    py0 = constrain(py0, y0 + 1, y0 + h - 1 - ph);
    for (int x = px0; x < px0 + pw; x++)
      for (int y = py0; y < py0 + ph; y++) fbClear(x, y);
  }
}

// Both eye boxes drift one pixel in the direction of the glance.
static void eyesApplyTargets() {
  const float dx = lookX * 0.3f;
  eyeL.tw = EYE_W; eyeL.th = EYE_H; eyeL.tx = EYE_GAP_LEFT - EYE_W + dx; eyeL.ty = (8.0f - EYE_H) / 2.0f;
  eyeR.tw = EYE_W; eyeR.th = EYE_H; eyeR.tx = EYE_GAP_RIGHT + dx;        eyeR.ty = (8.0f - EYE_H) / 2.0f;
}

static void eyesPushFrame() {
  mx->control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);
  for (int x = 0; x < 32; x++) mx->setColumn(31 - x, eyesFb[x]);
  mx->control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);
  memcpy(eyesLastFb, eyesFb, sizeof(eyesFb));
  eyesDirty = false;
}

static void eyesTick(unsigned long now) {
  // Blink: quick close and open, every 2..6 s
  float blinkPhase = 0.0f;
  if (!blinking && now >= nextBlinkMs) {
    blinking     = true;
    blinkStartMs = now;
    nextBlinkMs  = now + random(2000, 6000);
  }
  if (blinking) {
    const float t = (float)(now - blinkStartMs) / (float)BLINK_MS;
    if (t >= 1.0f) blinking = false;
    else blinkPhase = 1.0f - fabsf(2.0f * t - 1.0f);
  }

  // Sweep: glide to one side, hold, glide to the other side, repeat
  if (!blinking && now >= nextSweepMs) {
    lookX = -lookX;
    eyeL.tpx = eyeR.tpx = lookX;
    eyeL.tpy = eyeR.tpy = 0;
    nextSweepMs = now + SWEEP_HOLD_MS;
  }

  eyesApplyTargets();
  eyeL.update();
  eyeR.update();

  memset(eyesFb, 0, sizeof(eyesFb));
  renderEye(eyeL, blinkPhase);
  renderEye(eyeR, blinkPhase);

  if (eyesDirty || memcmp(eyesFb, eyesLastFb, sizeof(eyesFb)) != 0) eyesPushFrame();
}

static void startEyes() {
  mode = MODE_EYES;
  P.displayClear();
  eyesDirty      = true;
  lastEyeFrameMs = 0;
  Serial.println("Mode: eyes");
}

// Flash the whole matrix, then scroll alertText until cleared (or alertMs runs out).
static void startAlert() {
  mode         = MODE_ALERT;
  alertQueued  = false;
  alertStartMs = millis();
  alertPhase   = 0;
  alertFlashOn = true;
  P.displayClear();
  fillAll(true);
  Serial.printf("Mode: alert \"%s\"%s\n", alertText, alertMs ? "" : " (until cleared)");
}

// Show an alert now if the display is free, otherwise once the timer is done.
static void requestAlert(const char *text, unsigned long durationMs) {
  strncpy(alertText, text, TEXT_MAX_LEN);
  alertText[TEXT_MAX_LEN] = '\0';
  alertMs = durationMs;
  if (mode == MODE_TEXT || mode == MODE_EYES || mode == MODE_ALERT) startAlert();
  else alertQueued = true;
}

// Whatever the user chose as the resting screen: text or eyes. A queued alert wins.
static void startIdle() {
  if (alertQueued)        startAlert();
  else if (idleMode == 1) startEyes();
  else                    startTextMode();
}

static void clearAlert() {
  alertQueued = false;
  if (mode == MODE_ALERT) {
    Serial.println("Alert cleared");
    mode = MODE_TEXT;      // so startIdle() does not re-queue
    startIdle();
  }
}

static void setIdleMode(uint8_t m) {
  if (idleMode != m) {
    idleMode = m;
    prefs.putUChar("idle", m);
  }
}

// Print the frame on the display as ASCII art (serial 'f'), for checking the
// animation without looking at the LEDs.
static void eyesDumpFrame() {
  for (int y = 0; y < 8; y++) {
    char line[34];
    for (int x = 0; x < 32; x++) line[x] = ((eyesLastFb[x] >> y) & 1) ? '#' : '.';
    line[32] = '\0';
    Serial.println(line);
  }
  Serial.printf("look=%d blinking=%d\n", (int)lookX, blinking ? 1 : 0);
}

// Pull one queued command from the web task and apply it.
static void applyPendingCommand() {
  Command       cmd;
  char          text[TEXT_MAX_LEN + 1];
  int           speed;
  unsigned long ms;
  bool          flip;
  uint8_t       module;
  unsigned long alertDur;

  portENTER_CRITICAL(&cmdMux);
  cmd        = pendingCmd;
  pendingCmd = CMD_NONE;
  memcpy(text, pendingText, sizeof(text));
  speed    = pendingSpeed;
  ms       = pendingTimerMs;
  flip     = pendingFlip;
  module   = pendingModule;
  alertDur = pendingAlertMs;
  portEXIT_CRITICAL(&cmdMux);

  switch (cmd) {
    case CMD_SET_TEXT:
      memcpy(displayText, text, sizeof(displayText));
      scrollSpeed = speed;
      setIdleMode(0);
      // While a timer or self-test runs the new text is kept and shown when it ends.
      if (mode == MODE_TEXT || mode == MODE_EYES) startTextMode();
      break;

    case CMD_START_TIMER:
      startTimer(ms, flip);
      break;

    case CMD_CANCEL_TIMER:
      startIdle();
      break;

    case CMD_SHOW_EYES:
      setIdleMode(1);
      if (mode == MODE_TEXT) startEyes();
      break;

    case CMD_SHOW_TEXT:
      setIdleMode(0);
      if (mode == MODE_EYES) startTextMode();
      break;

    case CMD_ALERT:
      requestAlert(text, alertDur);
      break;

    case CMD_CLEAR_ALERT:
      clearAlert();
      break;

    case CMD_SELFTEST:
      startSelfTest();
      break;

    case CMD_SET_MODULE:
      applyModuleType(module, true);
      break;

    case CMD_NONE:
    default:
      break;
  }
}

// Serial console: lets the display be driven from the USB serial monitor
// without Wi-Fi. Commands (end with Enter):
//   t<seconds>[f]   start a timer, e.g. "t 30" or "t 300 f" (f = fill from the other end)
//   c               cancel timer / back to text
//   x<text>         set the scrolling text, e.g. "x HELLO"
//   s               print current state
static void handleSerialConsole() {
  static char    line[TEXT_MAX_LEN + 8];
  static uint8_t len = 0;

  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (len < sizeof(line) - 1) line[len++] = c;
      continue;
    }
    line[len] = '\0';
    len = 0;

    char *p = line;
    while (*p == ' ') p++;
    if (!*p) continue;

    switch (p[0]) {
      case 't': {
        long secs = atol(p + 1);
        if (secs < 1) secs = 60;
        if ((unsigned long)secs > TIMER_MAX_SEC) secs = TIMER_MAX_SEC;
        startTimer((unsigned long)secs * 1000UL, strchr(p + 1, 'f') != nullptr);
        break;
      }
      case 'c':
        startTextMode();
        break;
      case 'x': {
        const char *t = p + 1;
        while (*t == ' ') t++;
        if (*t) {
          strncpy(displayText, t, TEXT_MAX_LEN);
          displayText[TEXT_MAX_LEN] = '\0';
          setIdleMode(0);
          if (mode == MODE_TEXT || mode == MODE_EYES) startTextMode();
          else Serial.println("Text saved; shows when the timer ends");
        }
        break;
      }
      case 's':
        Serial.printf("mode=%d millis=%lu timerStart=%lu duration=%lu lastLevel=%d text=\"%s\" speed=%d module=%u (%s)\n",
                      (int)mode, millis(), timerStartMs, timerDurationMs, lastLevel, displayText, scrollSpeed,
                      moduleTypeIdx, moduleTypeName(moduleTypeIdx));
        break;
      case 'd':
        startSelfTest();
        break;
      case 'e':
        setIdleMode(1);
        if (mode == MODE_TEXT) startEyes();
        break;
      case 'a': {                      // a TEXT  -> alert until cleared
        const char *t = p + 1;
        while (*t == ' ') t++;
        if (*t) requestAlert(t, 0);
        break;
      }
      case 'k':
        clearAlert();
        break;
      case 'f':
        eyesDumpFrame();
        break;
      case 'm': {
        const char *q = p + 1;
        while (*q == ' ') q++;
        if (*q < '0' || *q > '7' || q[1] != '\0') {
          Serial.println("Module types:");
          for (uint8_t i = 0; i < 8; i++)
            Serial.printf("  m %u = %s%s\n", i, MODULE_NAMES[i], (i == moduleTypeIdx) ? "   <- current" : "");
        } else {
          applyModuleType((uint8_t)(*q - '0'), true);
        }
        break;
      }
      default:
        Serial.println("Commands: t<sec>[f] start timer | c cancel | x<text> set text | e eyes | a<text> alert until cleared | k clear alert | f dump eye frame | s status | d self-test | m<0-7> module type");
        break;
    }
  }
}

// Escape a C string for embedding in a JSON string literal.
static String jsonEscape(const char *s) {
  String out;
  out.reserve(strlen(s) + 8);
  for (; *s; s++) {
    const char c = *s;
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(uint8_t)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// Human-readable reset cause. "BROWNOUT" on the serial monitor means the power
// supply or USB cable cannot keep up when Wi-Fi and the LEDs draw current together.
static const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external reset pin";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "PANIC (firmware crash)";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (power supply too weak)";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "unknown";
  }
}

// Log access-point events so connection problems can be seen on the serial monitor.
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_START:
      Serial.println("WiFi: access point up");
      break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
      Serial.println("WiFi: access point stopped");
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
      const uint8_t *m = info.wifi_ap_staconnected.mac;
      Serial.printf("WiFi: client joined  %02X:%02X:%02X:%02X:%02X:%02X\n", m[0], m[1], m[2], m[3], m[4], m[5]);
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: {
      const uint8_t *m = info.wifi_ap_stadisconnected.mac;
      Serial.printf("WiFi: client left    %02X:%02X:%02X:%02X:%02X:%02X\n", m[0], m[1], m[2], m[3], m[4], m[5]);
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      Serial.printf("WiFi: client got IP  %s\n", IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
      break;
    default:
      break;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.printf("\nBoot. Reset reason: %s\n", resetReasonName(esp_reset_reason()));

  // Saved settings
  prefs.begin("ledmatrix", false);
  moduleTypeIdx = prefs.getUChar("modtype", MODULE_TYPE_DEFAULT);
  if (moduleTypeIdx > 7) moduleTypeIdx = MODULE_TYPE_DEFAULT;
  timerFlip = prefs.getBool("flip", false);
  idleMode  = prefs.getUChar("idle", 0) ? 1 : 0;

  // LED matrix
  P.begin();
  P.setIntensity(5);
  mx = P.getGraphicObject();
  eyeL.init(EYE_GAP_LEFT - EYE_W, (8 - EYE_H) / 2, EYE_W, EYE_H);
  eyeR.init(EYE_GAP_RIGHT,        (8 - EYE_H) / 2, EYE_W, EYE_H);
  applyModuleType(moduleTypeIdx, false);   // sets the mapping and draws the text
  startIdle();                              // switch to eyes if that was the last choice

  // Access point
  WiFi.persistent(false);
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_AP);
  const bool apOk = WiFi.softAP(ssid, nullptr, WIFI_CHANNEL, 0, WIFI_MAX_CLIENTS);
  WiFi.setTxPower(WIFI_TX_POWER);   // must come after softAP; see WIFI_TX_POWER note
  WiFi.setSleep(false);             // no modem power-save: keeps beacons and replies prompt
  Serial.printf("AP %s. SSID: %s\n", apOk ? "started" : "FAILED", ssid);
  Serial.printf("Channel %d, TX power code %d, MAC %s\n", WIFI_CHANNEL, (int)WiFi.getTxPower(), WiFi.softAPmacAddress().c_str());
  Serial.print("Open http://");
  Serial.println(WiFi.softAPIP());

  // Captive-portal DNS: every hostname resolves to us. Phones and laptops then
  // detect a "sign-in" network and open the page by themselves.
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  // Web page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", (const uint8_t *)INDEX_HTML, sizeof(INDEX_HTML) - 1);
  });

  // Update text and scroll speed
  server.on("/setText", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("text") || !request->hasParam("speed")) {
      request->send(400, "text/plain", "Missing text or speed parameter");
      return;
    }
    String text = request->getParam("text")->value();
    text.trim();
    if (text.length() == 0) {
      request->send(400, "text/plain", "Text is empty");
      return;
    }
    int speed = request->getParam("speed")->value().toInt();
    speed = constrain(speed, SPEED_MIN, SPEED_MAX);

    portENTER_CRITICAL(&cmdMux);
    strncpy(pendingText, text.c_str(), TEXT_MAX_LEN);
    pendingText[TEXT_MAX_LEN] = '\0';
    pendingSpeed = speed;
    pendingCmd   = CMD_SET_TEXT;
    portEXIT_CRITICAL(&cmdMux);

    request->send(200, "text/plain", "Updated");
  });

  // Start a countdown: /startTimer?seconds=300&flip=0
  server.on("/startTimer", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("seconds")) {
      request->send(400, "text/plain", "Missing seconds parameter");
      return;
    }
    const long secs = request->getParam("seconds")->value().toInt();
    if (secs < 1 || (unsigned long)secs > TIMER_MAX_SEC) {
      request->send(400, "text/plain", "Duration must be between 1 and 86400 seconds");
      return;
    }
    const bool flip = request->hasParam("flip") && request->getParam("flip")->value() == "1";

    portENTER_CRITICAL(&cmdMux);
    pendingTimerMs = (unsigned long)secs * 1000UL;
    pendingFlip    = flip;
    pendingCmd     = CMD_START_TIMER;
    portEXIT_CRITICAL(&cmdMux);

    request->send(200, "text/plain", "Timer started");
  });

  // Stop the countdown (or the end-of-timer flash) and go back to text
  server.on("/cancelTimer", HTTP_GET, [](AsyncWebServerRequest *request) {
    portENTER_CRITICAL(&cmdMux);
    pendingCmd = CMD_CANCEL_TIMER;
    portEXIT_CRITICAL(&cmdMux);
    request->send(200, "text/plain", "Timer cancelled");
  });

  // Animated eyes as the resting screen
  server.on("/eyes", HTTP_GET, [](AsyncWebServerRequest *request) {
    portENTER_CRITICAL(&cmdMux);
    pendingCmd = CMD_SHOW_EYES;
    portEXIT_CRITICAL(&cmdMux);
    request->send(200, "text/plain", "Eyes shown");
  });

  // Break alert: /alert?text=...&seconds=N  (seconds omitted or 0 = until cleared)
  server.on("/alert", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("text")) {
      request->send(400, "text/plain", "Missing text parameter");
      return;
    }
    String text = request->getParam("text")->value();
    text.trim();
    if (text.length() == 0) {
      request->send(400, "text/plain", "Text is empty");
      return;
    }
    long secs = request->hasParam("seconds") ? request->getParam("seconds")->value().toInt() : 0;
    if (secs < 0) secs = 0;
    if ((unsigned long)secs > TIMER_MAX_SEC) secs = TIMER_MAX_SEC;

    portENTER_CRITICAL(&cmdMux);
    strncpy(pendingText, text.c_str(), TEXT_MAX_LEN);
    pendingText[TEXT_MAX_LEN] = '\0';
    pendingAlertMs = (unsigned long)secs * 1000UL;
    pendingCmd     = CMD_ALERT;
    portEXIT_CRITICAL(&cmdMux);

    request->send(200, "text/plain", "Alert shown");
  });

  server.on("/clearAlert", HTTP_GET, [](AsyncWebServerRequest *request) {
    portENTER_CRITICAL(&cmdMux);
    pendingCmd = CMD_CLEAR_ALERT;
    portEXIT_CRITICAL(&cmdMux);
    request->send(200, "text/plain", "Alert cleared");
  });

  // Scrolling text as the resting screen
  server.on("/showText", HTTP_GET, [](AsyncWebServerRequest *request) {
    portENTER_CRITICAL(&cmdMux);
    pendingCmd = CMD_SHOW_TEXT;
    portEXIT_CRITICAL(&cmdMux);
    request->send(200, "text/plain", "Text shown");
  });

  // Display self-test sequence (about 18 s), then back to the resting screen
  server.on("/selftest", HTTP_GET, [](AsyncWebServerRequest *request) {
    portENTER_CRITICAL(&cmdMux);
    pendingCmd = CMD_SELFTEST;
    portEXIT_CRITICAL(&cmdMux);
    request->send(200, "text/plain", "Self-test started");
  });

  // Module wiring variant: /setModule?type=0..7 (saved in flash)
  server.on("/setModule", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("type")) {
      request->send(400, "text/plain", "Missing type parameter");
      return;
    }
    const long t = request->getParam("type")->value().toInt();
    if (t < 0 || t > 7) {
      request->send(400, "text/plain", "type must be 0..7");
      return;
    }
    portENTER_CRITICAL(&cmdMux);
    pendingModule = (uint8_t)t;
    pendingCmd    = CMD_SET_MODULE;
    portEXIT_CRITICAL(&cmdMux);
    request->send(200, "text/plain", "Module type set");
  });

  // Current state as JSON, polled by the page
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    const DisplayMode   m       = mode;
    const unsigned long totalMs = timerDurationMs;
    unsigned long remainingMs   = 0;
    if (m == MODE_TIMER) {
      const unsigned long elapsed = millis() - timerStartMs;
      remainingMs = (elapsed >= totalMs) ? 0 : (totalMs - elapsed);
    }

    String json;
    json.reserve(256);
    json += "{\"mode\":\"";
    json += (m == MODE_TEXT) ? "text" : (m == MODE_TIMER) ? "timer" : (m == MODE_DONE) ? "done"
          : (m == MODE_EYES) ? "eyes" : (m == MODE_ALERT) ? "alert" : "selftest";
    json += "\",\"remaining\":";
    json += String((remainingMs + 999UL) / 1000UL);   // round up so it never shows 0 early
    json += ",\"total\":";
    json += String(totalMs / 1000UL);
    json += ",\"flip\":";
    json += timerFlip ? "true" : "false";
    json += ",\"text\":\"";
    json += jsonEscape(displayText);
    json += "\",\"speed\":";
    json += String(scrollSpeed);
    json += ",\"module\":";
    json += String(moduleTypeIdx);
    json += ",\"step\":";
    json += String((m == MODE_SELFTEST) ? selfStep : 0);
    json += ",\"idle\":\"";
    json += idleMode ? "eyes" : "text";
    json += "\",\"alert\":\"";
    json += (m == MODE_ALERT || alertQueued) ? jsonEscape(alertText) : "";
    json += "\"}";

    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  // Anything else, including the OS connectivity probes (generate_204,
  // hotspot-detect.html, connecttest.txt), is redirected to the UI. That is
  // what makes the captive-portal prompt appear.
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->redirect("http://" + WiFi.softAPIP().toString() + "/");
  });

  server.begin();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Main loop
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  dnsServer.processNextRequest();
  handleSerialConsole();
  applyPendingCommand();

  const unsigned long now = millis();

  switch (mode) {
    case MODE_TEXT:
      if (P.displayAnimate() && !textStatic) {
        P.displayReset();     // scrolling text loops; still text just stays
      }
      break;

    case MODE_TIMER: {
      const unsigned long elapsed = now - timerStartMs;
      if (elapsed >= timerDurationMs) {
        // Time is up: bar is full, start flashing.
        mode        = MODE_DONE;
        doneStartMs = now;
        doneLit     = true;
        fillAll(true);
        Serial.printf("[%lu ms] Mode: done (elapsed %lu of %lu ms)\n", now, elapsed, timerDurationMs);
        break;
      }
      // 64-bit math: elapsed * 256 overflows 32 bits for timers over ~4.6 hours.
      const int level = (int)(((uint64_t)elapsed * BAR_LEVELS) / timerDurationMs);
      if (level != lastLevel) {
        drawBar((uint16_t)level);
        lastLevel = level;
      }
      break;
    }

    case MODE_EYES:
      if (now - lastEyeFrameMs >= EYES_FRAME_MS) {
        lastEyeFrameMs = now;
        eyesTick(now);
      }
      break;

    case MODE_ALERT: {
      const unsigned long since = now - alertStartMs;
      if (alertMs && since >= alertMs) {
        clearAlert();
        break;
      }
      if (alertPhase == 0) {
        if (since >= ALERT_FLASH_MS) {
          alertPhase = 1;
          P.displayClear();
          P.displayText(alertText, PA_CENTER, scrollSpeed, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
        } else {
          const bool on = ((since / ALERT_FLASH_PERIOD) % 2) == 0;
          if (on != alertFlashOn) {
            alertFlashOn = on;
            fillAll(on);
          }
        }
      } else if (P.displayAnimate()) {
        P.displayReset();
      }
      break;
    }

    case MODE_DONE: {
      const unsigned long sinceDone = now - doneStartMs;
      if (sinceDone >= DONE_BLINK_MS) {
        startIdle();
        break;
      }
      const bool lit = ((sinceDone / DONE_BLINK_PERIOD) % 2) == 0;
      if (lit != doneLit) {
        doneLit = lit;
        fillAll(lit);
      }
      break;
    }

    case MODE_SELFTEST: {
      if (now - selfStepStart >= SELF_STEPS[selfStep].ms) {
        selfStep++;
        if (selfStep >= SELF_STEP_COUNT) {
          startIdle();
          break;
        }
        selfStepStart = now;
        selfTestDraw(selfStep);
      }
      break;
    }
  }
}
