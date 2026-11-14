#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
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
#define WIFI_CHANNEL     11
#define WIFI_MAX_CLIENTS 4
#define DNS_PORT         53
// Transmit power. Small boards with a PCB-trace antenna (ESP32-C3 SuperMini and
// similar) are often invisible or refuse connections at full power because the
// antenna is mismatched. 8.5 dBm is the widely used fix and still covers a room.
// Boards with a proper or external antenna can go up to WIFI_POWER_19_5dBm.
#define WIFI_TX_POWER    WIFI_POWER_19_5dBm

AsyncWebServer server(80);
DNSServer      dnsServer;   // answers every hostname with our IP (captive portal)

// ─────────────────────────────────────────────────────────────────────────────
//  Display state. Owned by loop(); the web task only reads it for /status.
// ─────────────────────────────────────────────────────────────────────────────
enum DisplayMode : uint8_t { MODE_TEXT, MODE_TIMER, MODE_DONE, MODE_SELFTEST, MODE_EYES, MODE_ALERT, MODE_CRICKET };

volatile DisplayMode mode = MODE_TEXT;
char displayText[TEXT_MAX_LEN + 1] = "WELCOME!";
int  scrollSpeed = 50;

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
//  Cricket scoreboard. tools/cricket_score.py on the PC fetches the live score
//  and sends it as short "pages" of text. The board rotates through the pages
//  and plays an animation for a four, a six, a wicket, a fifty, a hundred and
//  the result. Drawn in the same 32x8 logical frame as the eyes (strip lying
//  flat, connector on the right), with the eyes' frame buffer.
// ─────────────────────────────────────────────────────────────────────────────
#define CRK_FRAME_MS     33         // about 30 fps
#define CRK_PAGE_MS      3500UL     // how long a page that fits the strip stays up
#define CRK_MAIN_PAGE_MS 6000UL     // the first page (the score) stays longer
#define CRK_SCROLL_MS    45UL       // ms per pixel for pages wider than the strip
#define CRK_SLIDE_MS     260UL      // time one column takes to roll from the old page to the new
#define CRK_RIPPLE_MS    7UL        // page change: each column starts this much after its left neighbour
#define CRK_ROLL_STAGGER_MS 70UL    // score change: each changed character starts this much after the last
#define CRK_SHINE_AT_MS  1500UL     // the highlight crosses the score page this long after it comes up
#define CRK_SHINE_MS     650UL
#define CRK_NO_ANIM      0xFFFF     // column delay value: this column does not move
#define CRK_DROP_MS      5UL        // drop-in transition: delay per column away from the centre
#define CRK_WIPE_STEP_MS 12UL       // wipe transition: delay per column
#define CRK_WIPE_BAR_MS  36UL       // wipe transition: how long the bar stays on a column
#define CRK_POP_MS       1400UL     // how long "+N" replaces the team name after runs are scored
#define CRK_GLOW_MS      700UL      // brightness kick after runs are scored
#define CRK_BASE_INTENSITY 5        // normal brightness, 0..15
#define CRK_GLOW_BOOST   7          // added to it at the start of the kick
#define CRK_NARROW_M     '\x05'
#define CRK_NARROW_W     '\x06'
enum CrkKind  : uint8_t { KIND_TEXT, KIND_MARQUEE, KIND_DASH };       // how the current page is drawn
enum CrkTrans : uint8_t { TRANS_UP, TRANS_DOWN, TRANS_WIPE };         // how it came in
#define CRK_ICON_BALL    '\x01'     // extra glyphs in the font
#define CRK_ICON_TARGET  '\x02'
#define CRK_ICON_BAT     '\x03'
#define CRK_ICON_RATE    '\x04'
#define CRK_STALE_MS     180000UL   // no update for this long: the PC script is gone, back to idle
#define CRK_PAGES_MAX    8
#define CRK_PAGE_LEN     39
#define CRK_LINE_MAX     239        // longest "q ..." payload
#define CRK_QUEUE_MAX    4

enum CrkAnim : uint8_t { ANIM_NONE, ANIM_FOUR, ANIM_SIX, ANIM_WICKET, ANIM_FIFTY, ANIM_HUNDRED, ANIM_WIN };
struct CrkEvent { CrkAnim anim; char text[CRK_PAGE_LEN + 1]; };

char          crkPages[CRK_PAGES_MAX][CRK_PAGE_LEN + 1];
uint8_t       crkPageCount    = 0;
int8_t        crkBalls        = -1;      // balls bowled in the current over, -1 = no over dots
bool          crkActive       = false;   // fresh score data is available
unsigned long crkLastUpdateMs = 0;
unsigned long lastCrkFrameMs  = 0;

uint8_t       crkPage         = 0;       // page on the display
char          crkShown[CRK_PAGE_LEN + 1] = "";
char          crkDraw[CRK_PAGE_LEN + 3]  = "";   // crkShown as drawn, with an icon where one fits
uint16_t      crkColDelay[32];           // per column: ms before it starts to move, or CRK_NO_ANIM
unsigned long crkSlideLen     = 0;
CrkKind       crkKind         = KIND_TEXT;
CrkTrans      crkTrans        = TRANS_UP;
uint8_t       crkTransCount   = 0;
char          crkDashTeam[CRK_PAGE_LEN + 1] = "";   // what the score screen shows
int           crkDashRuns     = 0;
int           crkDashWkts     = -1;
int           crkRuns         = -1;      // last score received, for the +N pop
char          crkRunsTeam[CRK_PAGE_LEN + 1] = "";
bool          crkPop          = false;
int           crkPopVal       = 0;
unsigned long crkPopMs        = 0;
bool          crkGlow         = false;
unsigned long crkGlowMs       = 0;
uint8_t       crkIntensity      = CRK_BASE_INTENSITY;   // wanted
uint8_t       crkIntensityShown = CRK_BASE_INTENSITY;   // set on the display
unsigned long crkPageStartMs  = 0;
int           crkPageW        = 0;
bool          crkTight        = false;   // narrow word gaps so the page just fits
bool          crkMarquee      = false;   // page is wider than the strip and scrolls
uint8_t       crkText[32];               // text layer of the current frame
uint8_t       crkOldText[32];            // text layer sliding out
bool          crkSliding      = false;
unsigned long crkSlideStartMs = 0;
bool          crkWiping       = false;   // end of over: the dots sweep away
unsigned long crkWipeMs       = 0;
bool          crkNewBall      = false;   // newest over dot blinks
unsigned long crkNewBallMs    = 0;

CrkAnim       crkAnim         = ANIM_NONE;
unsigned long crkAnimStartMs  = 0;
char          crkAnimText[CRK_PAGE_LEN + 1] = "";
CrkEvent      crkQueue[CRK_QUEUE_MAX];
uint8_t       crkQueueLen     = 0;

// ─────────────────────────────────────────────────────────────────────────────
//  Live scores over Wi-Fi, without a PC. Besides running its own access point
//  the board can join a home network. A background task then looks for a live
//  international match on ESPN's public scoreboard (plain HTTP, no key) and
//  feeds the scoreboard above. It only sees the totals, so a four or a six is
//  inferred from the jump in the score, and there are no batter pages. The PC
//  script (tools/cricket_score.py) has both and wins while it is running.
// ─────────────────────────────────────────────────────────────────────────────
#define NET_POLL_MS    10000UL      // score check while a match is followed
#define NET_FIND_MS    60000UL      // search for a match while none is live
#define NET_RESULT_MS  600000UL     // keep the result up this long
#define NET_HOLD_MS    60000UL      // stay quiet this long after a score from the PC or the page
#define NET_TIMEOUT_MS 12000UL      // one HTTP request
#define NET_HOST_FIND  "site.web.api.espn.com"
#define NET_PATH_FIND  "/apis/v2/scoreboard/header?sport=cricket&lang=en&region=in"
#define NET_HOST_MATCH "site.api.espn.com"

char          wifiSsid[33]  = "";   // home network to join, empty = access point only (saved in flash)
char          wifiPass[65]  = "";
bool          crkAuto       = true; // fetch live scores when Wi-Fi is set (saved in flash)
char          crkTeam[12]   = "";   // only follow this team, e.g. "IND"; empty = any international

// Hand-over from the network task to loop()
portMUX_TYPE  netMux        = portMUX_INITIALIZER_UNLOCKED;
volatile bool netKick       = false;   // settings changed: wake the task
volatile bool netReset      = false;   // settings changed: drop the match being followed
bool          netHasScore   = false;
bool          netWantStop   = false;
char          netPayload[CRK_LINE_MAX + 1] = "";
char          netEvents[3][CRK_PAGE_LEN + 8];
uint8_t       netEventCount = 0;
char          netStatus[48] = "Starting";

bool          crkFromNet      = false;   // the score on the display came from the network task
bool          crkExternalSeen = false;   // a score came from the PC or the page
unsigned long crkExternalMs   = 0;

// ─────────────────────────────────────────────────────────────────────────────
//  Commands from the web server. Handlers run on the AsyncTCP task, not in
//  loop(), so they never touch the display. They queue one command here and
//  loop() applies it on its next pass.
// ─────────────────────────────────────────────────────────────────────────────
enum Command : uint8_t { CMD_NONE, CMD_SET_TEXT, CMD_START_TIMER, CMD_CANCEL_TIMER, CMD_SELFTEST, CMD_SET_MODULE,
                         CMD_SHOW_EYES, CMD_SHOW_TEXT, CMD_ALERT, CMD_CLEAR_ALERT,
                         CMD_CRICKET, CMD_CRICKET_EVENT, CMD_CRICKET_STOP, CMD_SET_WIFI, CMD_SET_AUTO };

portMUX_TYPE  cmdMux         = portMUX_INITIALIZER_UNLOCKED;
Command       pendingCmd     = CMD_NONE;
char          pendingText[TEXT_MAX_LEN + 1] = "";
int           pendingSpeed   = 50;
unsigned long pendingTimerMs = 0;
bool          pendingFlip    = false;
uint8_t       pendingModule  = MODULE_TYPE_DEFAULT;
unsigned long pendingAlertMs = 0;
char          pendingCricket[CRK_LINE_MAX + 1] = "";
char          pendingPass[sizeof(wifiPass)] = "";
bool          pendingKeepPass = false;

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
        <h1>Cricket Score</h1>
        <p class="hint">
          Run tools/cricket_score.py on your PC during an international match. The matrix shows
          the live score and plays an animation for every four, six and wicket. Lay the strip
          flat with the connector on the right. The buttons below preview it without a match.
        </p>
        <div class="timer-state" id="cricketState" style="text-align: center; margin-bottom: 1.5rem;">Not running</div>
        <div class="btn-row" style="margin-bottom: 0.75rem;">
          <button type="button" class="submit-btn" id="crkDemoBtn">Demo Score</button>
          <button type="button" class="submit-btn secondary" id="crkStopBtn" disabled>Stop</button>
        </div>
        <div class="btn-row" style="margin-bottom: 0.75rem;">
          <button type="button" class="submit-btn secondary crk-ev" data-e="4">Four</button>
          <button type="button" class="submit-btn secondary crk-ev" data-e="6">Six</button>
          <button type="button" class="submit-btn secondary crk-ev" data-e="W">Wicket</button>
        </div>
        <div class="btn-row">
          <button type="button" class="submit-btn secondary crk-ev" data-e="50" data-t="KOHLI 50*">Fifty</button>
          <button type="button" class="submit-btn secondary crk-ev" data-e="100" data-t="KOHLI 100*">Hundred</button>
          <button type="button" class="submit-btn secondary crk-ev" data-e="WIN" data-t="INDIA WON BY 5 WICKETS">Win</button>
        </div>
        <div class="status" id="cricketStatus"></div>
      </div>

      <div class="container">
        <h1>Live Scores over Wi-Fi</h1>
        <p class="hint">
          Let the board join your home Wi-Fi and fetch international scores by itself, with no PC.
          This page travels over an open network, so enter the password only at home.
        </p>
        <div class="form-group">
          <label for="wifiSsid">Home Wi-Fi name (2.4 GHz)</label>
          <input type="text" id="wifiSsid" maxlength="32" placeholder="Network name" autocomplete="off">
        </div>
        <div class="form-group">
          <label for="wifiPass">Password</label>
          <input type="text" id="wifiPass" maxlength="64" placeholder="Empty keeps the saved password" autocomplete="off">
        </div>
        <div class="btn-row" style="margin-bottom: 1.5rem;">
          <button type="button" class="submit-btn" id="wifiSaveBtn">Save Wi-Fi</button>
          <button type="button" class="submit-btn secondary" id="wifiForgetBtn">Forget</button>
        </div>
        <div class="form-group">
          <label for="crkTeam">Only this team (optional)</label>
          <input type="text" id="crkTeam" maxlength="11" placeholder="IND, AUS, ENG... empty = any international" autocomplete="off">
        </div>
        <label class="check">
          <input type="checkbox" id="crkAuto">
          Show live international scores automatically
        </label>
        <div class="btn-row">
          <button type="button" class="submit-btn" id="autoSaveBtn">Save</button>
        </div>
        <div class="timer-state" id="netState" style="margin-top: 1rem; text-align: center;"></div>
        <div class="status" id="netStatus"></div>
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
      cancelBtn.disabled = (st.mode === 'text' || st.mode === 'eyes' || st.mode === 'cricket');
      $('cricketState').textContent = !st.cricket ? 'Not running'
        : (st.mode === 'cricket' ? 'Showing: ' : 'Ready, shows when the display is free: ') + st.cricket;
      $('crkStopBtn').disabled = !st.cricket;
      autoOn = !!st.auto;
      $('netState').textContent = (st.wifi ? 'Wi-Fi "' + st.wifi + '": ' + st.wifiState : 'No home Wi-Fi set')
        + (st.wifi ? ' - ' + st.net : '');
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
          $('wifiSsid').value = st.wifi;
          $('crkTeam').value  = st.team;
          $('crkAuto').checked = !!st.auto;
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

    // ── Cricket ────────────────────────────────────────────────────────────
    const CRK_DEMO = '/cricket?d=' + encodeURIComponent('2|IND 187/4|OV 18.2|NEED 24|10 BALLS|KOHLI 72*|PANDYA 31*');

    $('crkDemoBtn').addEventListener('click', () =>
      simpleCall(CRK_DEMO, $('cricketStatus'), 'Demo score on the matrix for 3 minutes', 'Failed to show the score'));

    let autoOn = false;
    const autoUrl = (on) => '/cricketAuto?on=' + (on ? 1 : 0) + '&team=' + encodeURIComponent($('crkTeam').value.trim());

    $('crkStopBtn').addEventListener('click', async () => {
      if (autoOn) {
        // Otherwise the board would bring the live score straight back
        try { await fetch(autoUrl(false)); $('crkAuto').checked = false; } catch (err) { console.error(err); }
        await new Promise((r) => setTimeout(r, 300));
      }
      simpleCall('/cricketStop', $('cricketStatus'), 'Scoreboard stopped', 'Failed to stop');
    });

    // ── Live scores over Wi-Fi ─────────────────────────────────────────────
    const refreshSoon = () => [2000, 8000, 15000].forEach((ms) => setTimeout(() => refresh(false), ms));

    $('wifiSaveBtn').addEventListener('click', () => {
      const ssid = $('wifiSsid').value.trim();
      const pass = $('wifiPass').value;
      if (!ssid) { showStatus($('netStatus'), 'Enter the Wi-Fi name', 'error'); return; }
      if (pass && pass.length < 8) { showStatus($('netStatus'), 'A Wi-Fi password has at least 8 characters', 'error'); return; }
      simpleCall('/wifi?ssid=' + encodeURIComponent(ssid) + (pass ? '&pass=' + encodeURIComponent(pass) : ''),
                 $('netStatus'), 'Saved. Joining the network...', 'Failed to save the Wi-Fi settings');
      $('wifiPass').value = '';
      refreshSoon();
    });

    $('wifiForgetBtn').addEventListener('click', () => {
      $('wifiSsid').value = '';
      simpleCall('/wifi?ssid=', $('netStatus'), 'Home Wi-Fi forgotten', 'Failed to forget the Wi-Fi settings');
    });

    $('autoSaveBtn').addEventListener('click', () => {
      simpleCall(autoUrl($('crkAuto').checked), $('netStatus'), 'Saved', 'Failed to save');
      refreshSoon();
    });

    document.querySelectorAll('.crk-ev').forEach((btn) => {
      btn.addEventListener('click', async () => {
        if (currentMode !== 'cricket') {
          // Animations only play on the scoreboard, so bring up the demo score first
          try { await fetch(CRK_DEMO); } catch (err) { console.error(err); }
          await new Promise((r) => setTimeout(r, 300));
        }
        const text = btn.dataset.t ? '&text=' + encodeURIComponent(btn.dataset.t) : '';
        simpleCall('/cricketEvent?e=' + btn.dataset.e + text, $('cricketStatus'),
                   btn.textContent + ' animation playing', 'Failed to play the animation');
      });
    });

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

// Return to normal scrolling text.
static void startTextMode() {
  mode = MODE_TEXT;
  P.displayClear();
  P.displayText(displayText, PA_CENTER, scrollSpeed, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  Serial.println("Mode: text");
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
    case MODE_CRICKET:  P.displayClear(); eyesDirty = true; break;
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
  if (mode == MODE_TEXT || mode == MODE_EYES || mode == MODE_ALERT || mode == MODE_CRICKET) startAlert();
  else alertQueued = true;
}

static void startCricket();

// Whatever the user chose as the resting screen: text or eyes. A queued alert
// wins, and so does the cricket scoreboard while the PC keeps sending a score.
static void startIdle() {
  if (alertQueued)        startAlert();
  else if (crkActive)     startCricket();
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

// ─────────────────────────────────────────────────────────────────────────────
//  Cricket scoreboard engine
// ─────────────────────────────────────────────────────────────────────────────

// 5-row font, 1 to 5 columns wide. Each row is `w` bits, leftmost pixel first.
// Entry 0 is the space and stands in for any character that has no glyph.
struct CrkGlyph { char c; uint8_t w; uint8_t rows[5]; };
static const CrkGlyph CRK_FONT[] = {
  {' ', 1, {0b0, 0b0, 0b0, 0b0, 0b0}},
  {'0', 3, {0b111, 0b101, 0b101, 0b101, 0b111}},
  {'1', 3, {0b010, 0b110, 0b010, 0b010, 0b111}},
  {'2', 3, {0b111, 0b001, 0b111, 0b100, 0b111}},
  {'3', 3, {0b111, 0b001, 0b111, 0b001, 0b111}},
  {'4', 3, {0b101, 0b101, 0b111, 0b001, 0b001}},
  {'5', 3, {0b111, 0b100, 0b111, 0b001, 0b111}},
  {'6', 3, {0b111, 0b100, 0b111, 0b101, 0b111}},
  {'7', 3, {0b111, 0b001, 0b001, 0b010, 0b010}},
  {'8', 3, {0b111, 0b101, 0b111, 0b101, 0b111}},
  {'9', 3, {0b111, 0b101, 0b111, 0b001, 0b111}},
  {'A', 3, {0b010, 0b101, 0b111, 0b101, 0b101}},
  {'B', 3, {0b110, 0b101, 0b110, 0b101, 0b110}},
  {'C', 3, {0b011, 0b100, 0b100, 0b100, 0b011}},
  {'D', 3, {0b110, 0b101, 0b101, 0b101, 0b110}},
  {'E', 3, {0b111, 0b100, 0b110, 0b100, 0b111}},
  {'F', 3, {0b111, 0b100, 0b110, 0b100, 0b100}},
  {'G', 3, {0b011, 0b100, 0b101, 0b101, 0b011}},
  {'H', 3, {0b101, 0b101, 0b111, 0b101, 0b101}},
  {'I', 3, {0b111, 0b010, 0b010, 0b010, 0b111}},
  {'J', 3, {0b001, 0b001, 0b001, 0b101, 0b010}},
  {'K', 3, {0b101, 0b101, 0b110, 0b101, 0b101}},
  {'L', 3, {0b100, 0b100, 0b100, 0b100, 0b111}},
  {'M', 5, {0b10001, 0b11011, 0b10101, 0b10001, 0b10001}},
  {'N', 3, {0b101, 0b111, 0b111, 0b111, 0b101}},
  {'O', 3, {0b010, 0b101, 0b101, 0b101, 0b010}},
  {'P', 3, {0b110, 0b101, 0b110, 0b100, 0b100}},
  {'Q', 3, {0b010, 0b101, 0b101, 0b111, 0b011}},
  {'R', 3, {0b110, 0b101, 0b110, 0b101, 0b101}},
  {'S', 3, {0b011, 0b100, 0b010, 0b001, 0b110}},
  {'T', 3, {0b111, 0b010, 0b010, 0b010, 0b010}},
  {'U', 3, {0b101, 0b101, 0b101, 0b101, 0b111}},
  {'V', 3, {0b101, 0b101, 0b101, 0b101, 0b010}},
  {'W', 5, {0b10001, 0b10001, 0b10101, 0b10101, 0b01010}},
  {'X', 3, {0b101, 0b101, 0b010, 0b101, 0b101}},
  {'Y', 3, {0b101, 0b101, 0b010, 0b010, 0b010}},
  {'Z', 3, {0b111, 0b001, 0b010, 0b100, 0b111}},
  {'/', 3, {0b001, 0b001, 0b010, 0b100, 0b100}},
  {'-', 3, {0b000, 0b000, 0b111, 0b000, 0b000}},
  {'+', 3, {0b000, 0b010, 0b111, 0b010, 0b000}},
  {'*', 3, {0b101, 0b010, 0b101, 0b000, 0b000}},
  {'.', 1, {0b0, 0b0, 0b0, 0b0, 0b1}},
  {'!', 1, {0b1, 0b1, 0b1, 0b0, 0b1}},
  {':', 1, {0b0, 0b1, 0b0, 0b1, 0b0}},
  {CRK_ICON_BALL,   5, {0b01110, 0b11011, 0b11011, 0b11011, 0b01110}},   // ball with its seam
  {CRK_ICON_TARGET, 5, {0b01110, 0b10001, 0b10101, 0b10001, 0b01110}},
  {CRK_ICON_BAT,    5, {0b00001, 0b00010, 0b01100, 0b11100, 0b11000}},
  {CRK_ICON_RATE,   5, {0b00001, 0b00001, 0b00101, 0b00101, 0b10101}},   // run rate bars
  {CRK_NARROW_M,    3, {0b101, 0b111, 0b111, 0b101, 0b101}},             // for a team block that is too wide
  {CRK_NARROW_W,    3, {0b101, 0b101, 0b111, 0b111, 0b101}},
};

static const CrkGlyph *crkGlyph(char c) {
  if (c >= 'a' && c <= 'z') c -= 32;
  for (const CrkGlyph &g : CRK_FONT) {
    if (g.c == c) return &g;
  }
  return &CRK_FONT[0];
}

// Pixel width of a string. `tight` shrinks the gap between words from 3 to 2.
static int crkTextWidth(const char *s, bool tight) {
  int w = 0;
  for (; *s; s++) {
    const CrkGlyph *g = crkGlyph(*s);
    w += ((g->c == ' ') ? (tight ? 0 : 1) : g->w) + 1;
  }
  return (w > 0) ? w - 1 : 0;
}

// Draw a string into a 32-column layer with its top-left corner at (x, y).
static void crkDrawText(uint8_t *layer, const char *s, int x, int y, bool tight) {
  for (; *s; s++) {
    const CrkGlyph *g = crkGlyph(*s);
    if (g->c == ' ') {
      x += (tight ? 0 : 1) + 1;
      continue;
    }
    for (int r = 0; r < 5; r++) {
      for (int c = 0; c < g->w; c++) {
        if (!((g->rows[r] >> (g->w - 1 - c)) & 1)) continue;
        const int px = x + c, py = y + r;
        if (px >= 0 && px < 32 && py >= 0 && py < 8) layer[px] |= (uint8_t)(1 << py);
      }
    }
    x += g->w + 1;
  }
}

static void crkFill()   { memset(eyesFb, 0xFF, sizeof(eyesFb)); }
static void crkInvert() { for (int x = 0; x < 32; x++) eyesFb[x] = (uint8_t)~eyesFb[x]; }

static void crkBall(float x, float y) {
  const int bx = (int)roundf(x), by = (int)roundf(y);
  fbSet(bx, by); fbSet(bx + 1, by); fbSet(bx, by + 1); fbSet(bx + 1, by + 1);
}

// Scroll `text` through once, right to left. True when it has left the strip.
static bool crkAnimScroll(unsigned long t, const char *text) {
  if (!text[0]) return true;
  const int x = 32 - (int)(t / CRK_SCROLL_MS);
  crkDrawText(eyesFb, text, x, 1, false);
  return x + crkTextWidth(text, false) < 0;
}

// FOUR: the ball races along the ground to the rope, a flash, then the word.
static bool crkAnimFour(unsigned long t) {
  if (t < 900) {
    const float bx = -2.0f + 36.0f * (t / 900.0f);
    for (int x = 0; x < 32; x++) {
      if (((x + (int)(t / 30)) & 3) == 0) fbSet(x, 7);          // grass streaming past
    }
    crkBall(bx, 4);
    for (int i = 1; i <= 4; i++) fbSet((int)bx - 2 * i, 5 - (i & 1));
  } else if (t < 1020) {
    crkFill();
  } else if (t < 1100) {
    // dark beat
  } else if (t < 2900) {
    const unsigned long s  = t - 1100;
    const int           cx = (32 - crkTextWidth("FOUR!", false)) / 2;
    const int           x  = (s < 220) ? 32 - (int)((32 - cx) * (s / 220.0f)) : cx;
    crkDrawText(eyesFb, "FOUR!", x, 1, false);
    if ((s >= 500 && s < 650) || (s >= 850 && s < 1000)) crkInvert();
  } else {
    return true;
  }
  return false;
}

// SIX: the ball sails up and out of the ground, fireworks, then the word.
static bool crkAnimSix(unsigned long t) {
  if (t < 1000) {
    for (int i = 0; i < 4; i++) {
      const float u = ((float)t - i * 55.0f) / 1000.0f;
      if (u < 0.0f) continue;
      const float x = 31.0f * u;
      const float y = 6.0f - 7.5f * u * (2.0f - u);
      if (i == 0) crkBall(x, y);
      else        fbSet((int)roundf(x), (int)roundf(y) + 1);
    }
  } else if (t < 2300) {
    static const int8_t CX[3] = {6, 17, 26};
    static const int8_t CY[3] = {3, 4, 3};
    for (int b = 0; b < 3; b++) {
      const long age = (long)t - 1000 - b * 300;
      if (age < 0 || age > 650) continue;
      const float r = 0.6f + age / 110.0f;
      if (age < 120) fbSet(CX[b], CY[b]);
      for (int d = 0; d < 8; d++) {
        if (age > 400 && (d & 1)) continue;                      // thins out as it fades
        const float a = d * 0.7853982f;
        fbSet((int)roundf(CX[b] + cosf(a) * r * 1.3f), (int)roundf(CY[b] + sinf(a) * r * 0.8f));
        if (age > 150 && age < 400) {
          fbSet((int)roundf(CX[b] + cosf(a) * r * 0.6f), (int)roundf(CY[b] + sinf(a) * r * 0.4f));
        }
      }
    }
  } else if (t < 3900) {
    const unsigned long s  = t - 2300;
    const int           w  = crkTextWidth("SIX!", false);
    const int           x0 = (32 - w) / 2;
    crkDrawText(eyesFb, "SIX!", x0, 1, false);
    for (int i = 0; i < 5; i++) {                                // twinkles either side of the word
      const int x = random(0, 32);
      if (x >= x0 - 1 && x <= x0 + w) continue;
      fbSet(x, random(0, 8));
    }
    if ((s >= 450 && s < 600) || (s >= 800 && s < 950)) crkInvert();
  } else {
    return true;
  }
  return false;
}

// WICKET: the ball hops in and tips the bails off, then OUT! drops in letter
// by letter, each one landing with a little hop.
static bool crkAnimWicket(unsigned long t) {
  static const int8_t STUMP_X[3] = {24, 26, 28};
  if (t < 1500) {
    const bool hit    = (t >= 900);
    const int  wobble = (hit && t < 1300) ? ((((t - 900) / 70) & 1) ? 1 : -1) : 0;        // the stumps shiver
    for (int i = 0; i < 3; i++) {
      for (int y = 2; y < 8; y++) fbSet(STUMP_X[i] + wobble, y);
    }
    if (!hit) {
      fbSet(25, 1); fbSet(27, 1);                                                 // bails
      const float u = t / 900.0f;
      crkBall(-2.0f + 23.0f * u, 6.0f - 4.0f * fabsf(sinf(u * 3.0f * (float)PI)));   // three hops
    } else {
      const float s = (t - 900) / 600.0f;
      const int   y = (int)roundf(1.0f - 12.0f * s + 18.0f * s * s);
      fbSet((int)roundf(25.0f - 5.0f * s), y);                                    // bails pop up and tumble away
      fbSet((int)roundf(27.0f + 4.0f * s), y);
      crkBall(21.0f - 3.0f * s, 6.0f);
    }
  } else if (t < 3000) {
    static const char WORD[] = "OUT!";
    int x = (32 - crkTextWidth(WORD, false)) / 2;
    for (int i = 0; WORD[i]; i++) {
      const long lt = (long)t - 1500 - i * 150;                                   // each letter drops in turn
      const char letter[2] = { WORD[i], '\0' };
      if (lt >= 0) {
        const int y = (lt < 200) ? -5 + (int)(lt * 7 / 200) : (lt < 280) ? 2 : (lt < 360) ? 0 : 1;   // fall, squash, hop, settle
        crkDrawText(eyesFb, letter, x, y, false);
      }
      x += crkGlyph(WORD[i])->w + 1;
    }
  } else {
    return true;
  }
  return false;
}

// FIFTY / HUNDRED: sparkles, the number between dancing bars, then the name.
static bool crkAnimMilestone(unsigned long t, bool hundred) {
  const char *label = hundred ? "100!" : "50!";
  if (t < 900) {
    const int n = 4 + (int)(t / 60);
    for (int i = 0; i < n; i++) fbSet(random(0, 32), random(0, 8));
  } else if (t < 2900) {
    const unsigned long s  = t - 900;
    const int           w  = crkTextWidth(label, false);
    const int           x0 = (32 - w) / 2;
    crkDrawText(eyesFb, label, x0, 1, false);
    const unsigned long beat = s / 90;
    for (int x = 0; x < 32; x += 2) {
      if (x >= x0 - 2 && x <= x0 + w + 1) continue;
      const int h = 1 + (int)((x * 37UL + beat * 53UL + (unsigned long)x * x * 11UL + beat * beat * 7UL) % 7UL);
      for (int y = 8 - h; y < 8; y++) fbSet(x, y);
    }
    if (hundred && ((s >= 600 && s < 750) || (s >= 1000 && s < 1150))) crkInvert();
  } else {
    return crkAnimScroll(t - 2900, crkAnimText);
  }
  return false;
}

// WIN: a wave runs along the strip, then the result scrolls through.
static bool crkAnimWin(unsigned long t) {
  if (t < 2200) {
    for (int x = 0; x < 32; x++) {
      const int y = (int)roundf(3.0f + 3.0f * sinf(x * 0.45f - t * 0.009f));
      fbSet(x, y); fbSet(x, y + 1);
    }
    return false;
  }
  return crkAnimScroll(t - 2200, crkAnimText);
}

static bool crkRunAnim(unsigned long now) {
  const unsigned long t = now - crkAnimStartMs;
  switch (crkAnim) {
    case ANIM_FOUR:    return crkAnimFour(t);
    case ANIM_SIX:     return crkAnimSix(t);
    case ANIM_WICKET:  return crkAnimWicket(t);
    case ANIM_FIFTY:   return crkAnimMilestone(t, false);
    case ANIM_HUNDRED: return crkAnimMilestone(t, true);
    case ANIM_WIN:     return crkAnimWin(t);
    default:           return true;
  }
}

// Swap the label of a page for a small icon where that reads better.
static void crkDecorate(const char *text, char *out, size_t outLen) {
  char         tmp[sizeof(crkDraw)];
  const size_t n = strlen(text);
  if (!strncmp(text, "OV ", 3)) {
    snprintf(out, outLen, "%c%s", CRK_ICON_BALL, text + 2);
    return;
  }
  if (!strncmp(text, "CRR ", 4)) {
    snprintf(out, outLen, "%c%s", CRK_ICON_RATE, text + 3);
    return;
  }
  if (!strncmp(text, "NEED ", 5) || (n && text[n - 1] == '*')) {
    snprintf(tmp, sizeof(tmp), "%c %s", (text[n - 1] == '*') ? CRK_ICON_BAT : CRK_ICON_TARGET, text);
    if (crkTextWidth(tmp, true) <= 32) {
      strlcpy(out, tmp, outLen);
      return;
    }
  }
  strlcpy(out, text, outLen);
}

// Tall digits for the score: 4 columns by 7 rows, leftmost pixel first
static const uint8_t CRK_BIG[10][7] = {
  {0b0110, 0b1001, 0b1001, 0b1001, 0b1001, 0b1001, 0b0110},
  {0b0010, 0b0110, 0b0010, 0b0010, 0b0010, 0b0010, 0b0111},
  {0b0110, 0b1001, 0b0001, 0b0010, 0b0100, 0b1000, 0b1111},
  {0b1110, 0b0001, 0b0001, 0b0110, 0b0001, 0b0001, 0b1110},
  {0b0010, 0b0110, 0b1010, 0b1010, 0b1111, 0b0010, 0b0010},
  {0b1111, 0b1000, 0b1110, 0b0001, 0b0001, 0b1001, 0b0110},
  {0b0110, 0b1000, 0b1000, 0b1110, 0b1001, 0b1001, 0b0110},
  {0b1111, 0b0001, 0b0010, 0b0010, 0b0100, 0b0100, 0b0100},
  {0b0110, 0b1001, 0b1001, 0b0110, 0b1001, 0b1001, 0b0110},
  {0b0110, 0b1001, 0b1001, 0b0111, 0b0001, 0b0001, 0b0110},
};

// "IND 187/4" or "IND 187" -> team, runs, wickets (-1 = none given). False if
// the page is not a score, or the runs do not fit three tall digits.
static bool crkParseScore(const char *s, char *team, size_t teamLen, int &runs, int &wkts) {
  const char *sp = strrchr(s, ' ');
  if (!sp || sp == s) return false;
  const char *r = sp + 1;
  int n = 0;
  runs = 0;
  wkts = -1;
  while (r[n] >= '0' && r[n] <= '9') {
    runs = runs * 10 + (r[n] - '0');
    if (++n > 3) return false;
  }
  if (n == 0) return false;
  if (r[n]) {
    if (r[n] != '/' || r[n + 1] < '0' || r[n + 1] > '9' || r[n + 2]) return false;
    wkts = r[n + 1] - '0';
  }
  size_t len = (size_t)(sp - s);
  if (len >= teamLen) len = teamLen - 1;
  memcpy(team, s, len);
  team[len] = '\0';
  return true;
}

// The same digits 3 columns wide, for a three digit score that has to leave
// room for "/wickets"
static const uint8_t CRK_BIG3[10][7] = {
  {0b111, 0b101, 0b101, 0b101, 0b101, 0b101, 0b111},
  {0b010, 0b110, 0b010, 0b010, 0b010, 0b010, 0b111},
  {0b111, 0b001, 0b001, 0b111, 0b100, 0b100, 0b111},
  {0b111, 0b001, 0b001, 0b111, 0b001, 0b001, 0b111},
  {0b101, 0b101, 0b101, 0b111, 0b001, 0b001, 0b001},
  {0b111, 0b100, 0b100, 0b111, 0b001, 0b001, 0b111},
  {0b111, 0b100, 0b100, 0b111, 0b101, 0b101, 0b111},
  {0b111, 0b001, 0b001, 0b010, 0b010, 0b010, 0b010},
  {0b111, 0b101, 0b101, 0b111, 0b101, 0b101, 0b111},
  {0b111, 0b101, 0b101, 0b111, 0b001, 0b001, 0b111},
};

static void crkDrawBig(uint8_t *layer, int digit, int x, int y, bool narrow) {
  const int      w    = narrow ? 3 : 4;
  const uint8_t *rows = narrow ? CRK_BIG3[digit] : CRK_BIG[digit];
  for (int r = 0; r < 7; r++) {
    for (int c = 0; c < w; c++) {
      if (!((rows[r] >> (w - 1 - c)) & 1)) continue;
      const int px = x + c, py = y + r;
      if (px >= 0 && px < 32 && py >= 0 && py < 8) layer[px] |= (uint8_t)(1 << py);
    }
  }
}

// The score screen: team name top left (the over dots go under it), runs in
// tall digits, then a tall slash and the wickets as a small digit: "187/4".
//   with wickets:    x 0..10 team | ..23 runs | 25..27 slash | 29..31 wickets
//   without wickets: x 0..10 team | ..25 runs
// Three digits next to a slash are drawn 3 columns wide so that all of it fits.
static void crkDrawDash(uint8_t *layer) {
  char team[sizeof(crkDashTeam)];
  strlcpy(team, crkDashTeam, sizeof(team));
  if (crkTextWidth(team, true) > 11) {
    for (char *c = team; *c; c++) {
      if (*c == 'M') *c = CRK_NARROW_M;
      else if (*c == 'W') *c = CRK_NARROW_W;
    }
  }
  size_t len = strlen(team);
  while (len > 1 && crkTextWidth(team, true) > 11) team[--len] = '\0';
  crkDrawText(layer, team, (11 - crkTextWidth(team, true)) / 2, 0, true);

  char digits[8];
  snprintf(digits, sizeof(digits), "%d", crkDashRuns);
  const int  count   = (int)strlen(digits);
  const bool hasWkts = (crkDashWkts >= 0);
  const bool narrow  = hasWkts && count >= 3;
  const int  step    = narrow ? 4 : 5;
  int x = (hasWkts ? 24 : 26) - (count * step - 1);
  for (const char *d = digits; *d; d++) {
    crkDrawBig(layer, *d - '0', x, 0, narrow);
    x += step;
  }
  if (hasWkts) {
    layer[27] |= 0b00000011;   // slash: rows 0..1, 2..4 and 5..6, one column each
    layer[26] |= 0b00011100;
    layer[25] |= 0b01100000;
    const char w[2] = { (char)('0' + crkDashWkts), '\0' };
    crkDrawText(layer, w, 29, 2, false);
  }
}

// Everything on the current page that does not move by itself.
static void crkDrawStatic(uint8_t *layer) {
  if (crkKind == KIND_DASH) crkDrawDash(layer);
  else crkDrawText(layer, crkDraw, (32 - crkPageW) / 2, 1, crkTight);
}

// Put page `crkPage` on the display. how: 0 = just show it, 1 = page change
// (one of three transitions, in turn), 2 = same page with new content (only
// the parts that changed roll over, one after another, like an odometer).
static void crkBeginPage(unsigned long now, uint8_t how) {
  strncpy(crkShown, crkPages[crkPage], CRK_PAGE_LEN);
  crkShown[CRK_PAGE_LEN] = '\0';
  crkPageStartMs = now;
  crkTight       = false;
  if (crkPage == 0 && crkParseScore(crkShown, crkDashTeam, sizeof(crkDashTeam), crkDashRuns, crkDashWkts)) {
    crkKind    = KIND_DASH;
    crkMarquee = false;
  } else {
    crkDecorate(crkShown, crkDraw, sizeof(crkDraw));
    crkPageW = crkTextWidth(crkDraw, false);
    if (crkPageW > 32) {
      const int tightW = crkTextWidth(crkDraw, true);
      if (tightW <= 32) { crkTight = true; crkPageW = tightW; }
    }
    crkMarquee = (crkPageW > 32);
    crkKind    = crkMarquee ? KIND_MARQUEE : KIND_TEXT;
  }
  crkSliding = false;
  if (how == 0 || crkMarquee) return;

  uint8_t fresh[32];
  memset(fresh, 0, sizeof(fresh));
  crkDrawStatic(fresh);
  memcpy(crkOldText, crkText, sizeof(crkOldText));
  crkSlideStartMs = now;
  if (how == 2) {
    crkTrans = TRANS_UP;
    int  group      = -1;
    bool wasChanged = false;
    for (int x = 0; x < 32; x++) {
      const bool changed = fresh[x] != crkText[x];
      if (changed && !wasChanged) group++;
      crkColDelay[x] = changed ? (uint16_t)(group * CRK_ROLL_STAGGER_MS) : CRK_NO_ANIM;
      wasChanged = changed;
    }
    if (group < 0) return;
    crkSlideLen = group * CRK_ROLL_STAGGER_MS + CRK_SLIDE_MS;
  } else {
    crkTrans = (CrkTrans)(crkTransCount++ % 3);
    for (int x = 0; x < 32; x++) {
      crkColDelay[x] = (crkTrans == TRANS_UP)   ? (uint16_t)(x * CRK_RIPPLE_MS)                  // ripple, left to right
                     : (crkTrans == TRANS_DOWN) ? (uint16_t)(abs(2 * x - 31) * CRK_DROP_MS)      // drops in, centre first
                     :                            (uint16_t)(x * CRK_WIPE_STEP_MS);              // a bar wipes across
    }
    crkSlideLen = (crkTrans == TRANS_UP)   ? 31 * CRK_RIPPLE_MS + CRK_SLIDE_MS
                : (crkTrans == TRANS_DOWN) ? 31 * CRK_DROP_MS + CRK_SLIDE_MS
                :                            31 * CRK_WIPE_STEP_MS + CRK_WIPE_BAR_MS;
  }
  crkSliding = true;
}

static void crkRenderPage(unsigned long now) {
  if (crkPageCount == 0) return;
  if (crkPage >= crkPageCount) {
    crkPage = 0;
    crkBeginPage(now, 1);
  } else if (strcmp(crkPages[crkPage], crkShown) != 0) {
    crkBeginPage(now, 2);                          // the score changed while this page was up
  }

  const unsigned long elapsed = now - crkPageStartMs;
  bool                done;

  memset(crkText, 0, sizeof(crkText));
  if (crkMarquee) {
    const int x = 32 - (int)(elapsed / CRK_SCROLL_MS);
    crkDrawText(crkText, crkDraw, x, 1, false);
    done = (x + crkPageW < 0);
  } else {
    crkDrawStatic(crkText);
    done = elapsed >= ((crkPage == 0) ? CRK_MAIN_PAGE_MS : CRK_PAGE_MS);
  }

  if (crkSliding && now - crkSlideStartMs >= crkSlideLen) crkSliding = false;
  for (int x = 0; x < 32; x++) {
    uint8_t        v = crkText[x];
    const uint16_t d = crkColDelay[x];
    if (crkSliding && d != CRK_NO_ANIM) {
      const unsigned long se = now - crkSlideStartMs;
      if (se < d) {
        v = crkOldText[x];                                         // this column has not started yet
      } else if (crkTrans == TRANS_WIPE) {
        if (se - d < CRK_WIPE_BAR_MS) v = 0x7F;                    // the bar itself
      } else if (se - d < CRK_SLIDE_MS) {
        const int k = (int)((se - d) * 7UL / CRK_SLIDE_MS);
        v = (crkTrans == TRANS_DOWN) ? (uint8_t)((crkOldText[x] << k) | (crkText[x] >> (7 - k)))
                                     : (uint8_t)((crkOldText[x] >> k) | (crkText[x] << (7 - k)));
      }
    }
    eyesFb[x] = v & 0x7F;
  }

  // A slanted highlight sweeps across the score page once each time it is up
  if (crkPage == 0 && !crkMarquee && !crkSliding && elapsed >= CRK_SHINE_AT_MS && elapsed < CRK_SHINE_AT_MS + CRK_SHINE_MS) {
    const int pos = -4 + (int)((elapsed - CRK_SHINE_AT_MS) * 40UL / CRK_SHINE_MS);
    for (int y = 0; y < 7; y++) {
      for (int dx = 0; dx < 2; dx++) {
        const int x = pos + dx + ((6 - y) >> 1);
        if (x >= 0 && x < 32) eyesFb[x] ^= (uint8_t)(1 << y);
      }
    }
  }

  if (crkKind == KIND_DASH) {
    // Runs just scored: "+N" takes the place of the team name for a moment
    if (crkPop && now - crkPopMs >= CRK_POP_MS) crkPop = false;
    if (crkPop) {
      const unsigned long t = now - crkPopMs;
      for (int x = 0; x < 11; x++) eyesFb[x] &= (uint8_t)~0x1F;
      if (t >= 480 || ((t / 120) & 1) == 0) {                      // flickers in, then holds
        uint8_t pop[32];
        char    text[8];
        memset(pop, 0, sizeof(pop));
        snprintf(text, sizeof(text), "+%d", crkPopVal);
        crkDrawText(pop, text, (11 - crkTextWidth(text, false)) / 2, (t < 150) ? 5 - (int)(t * 5 / 150) : 0, false);
        for (int x = 0; x < 11; x++) eyesFb[x] |= pop[x] & 0x1F;
      }
    }

    // Over dots under the team name: one per ball bowled this over
    if (crkBalls >= 0) {
      if (crkWiping  && now - crkWipeMs    >= 700)  crkWiping  = false;
      if (crkNewBall && now - crkNewBallMs >= 1500) crkNewBall = false;
      for (int x = 0; x < 11; x++) eyesFb[x] &= 0x3F;
      for (int i = 0; i < 6; i++) {
        bool lit = (i < crkBalls);
        if (crkWiping) {
          lit = (i >= (int)((now - crkWipeMs) / 100));              // the finished over sweeps away
        } else if (lit && crkNewBall && i == crkBalls - 1) {
          lit = (((now - crkNewBallMs) / 150) & 1) == 0;            // the latest ball blinks
        }
        if (lit) { fbSet(2 * i, 6); fbSet(2 * i, 7); }
      }
      if (!crkWiping && crkBalls < 6 && ((now / 150) & 3) == 0) fbSet(2 * crkBalls, 7);   // next ball
    }
  }

  // Brightness kicks up when runs are scored and settles back
  crkIntensity = CRK_BASE_INTENSITY;
  if (crkGlow && now - crkGlowMs >= CRK_GLOW_MS) crkGlow = false;
  if (crkGlow) crkIntensity += (uint8_t)(CRK_GLOW_BOOST * (CRK_GLOW_MS - (now - crkGlowMs)) / CRK_GLOW_MS);

  if (done) {
    const uint8_t next = (uint8_t)((crkPage + 1) % crkPageCount);
    if (next == crkPage && !crkMarquee) {
      crkPageStartMs = now;                        // a single page that fits just stays up
    } else {
      crkPage = next;
      crkBeginPage(now, 1);
    }
  }
}

static void cricketTick(unsigned long now) {
  memset(eyesFb, 0, sizeof(eyesFb));
  if (crkAnim != ANIM_NONE || crkQueueLen > 0) crkIntensity = CRK_BASE_INTENSITY;

  if (crkAnim != ANIM_NONE && crkRunAnim(now)) {
    crkAnim = ANIM_NONE;                           // finished: back to the score page
    memset(eyesFb, 0, sizeof(eyesFb));
    memset(crkText, 0, sizeof(crkText));
    crkPage = 0;
    crkBeginPage(now, 0);
  }
  if (crkAnim == ANIM_NONE && crkQueueLen > 0) {
    crkAnim        = crkQueue[0].anim;
    crkAnimStartMs = now;
    memcpy(crkAnimText, crkQueue[0].text, sizeof(crkAnimText));
    crkQueueLen--;
    memmove(&crkQueue[0], &crkQueue[1], crkQueueLen * sizeof(CrkEvent));
    memset(eyesFb, 0, sizeof(eyesFb));
    crkRunAnim(now);
  }
  if (crkAnim == ANIM_NONE) crkRenderPage(now);

  if (crkIntensity != crkIntensityShown) {
    mx->control(MD_MAX72XX::INTENSITY, crkIntensity);
    crkIntensityShown = crkIntensity;
  }

  if (eyesDirty || memcmp(eyesFb, eyesLastFb, sizeof(eyesFb)) != 0) eyesPushFrame();
}

static void startCricket() {
  mode = MODE_CRICKET;
  P.displayClear();
  eyesDirty      = true;
  lastCrkFrameMs = 0;
  crkAnim        = ANIM_NONE;
  crkQueueLen    = 0;
  crkPage        = 0;
  memset(crkText, 0, sizeof(crkText));
  crkBeginPage(millis(), 0);
  Serial.println("Mode: cricket");
}

static void stopCricket() {
  crkActive    = false;
  crkPageCount = 0;
  crkRuns      = -1;
  crkQueueLen  = 0;
  crkAnim      = ANIM_NONE;
  if (mode == MODE_CRICKET) {
    Serial.println("Cricket stopped");
    mode = MODE_TEXT;      // so startIdle() picks the resting screen
    startIdle();
  }
}

// New score from the PC: "<balls>|PAGE|PAGE|...". <balls> is the number of balls
// bowled in the current over (0..6), or "-" to hide the over dots. The first
// page is the score itself. Returns false if the payload is malformed.
static bool cricketSet(const char *payload, bool fromNet = false) {
  char buf[CRK_LINE_MAX + 1];
  strncpy(buf, payload, CRK_LINE_MAX);
  buf[CRK_LINE_MAX] = '\0';

  char *bar = strchr(buf, '|');
  if (!bar) return false;
  *bar = '\0';
  const char *b = buf;
  while (*b == ' ') b++;
  const int balls = (*b >= '0' && *b <= '9') ? constrain(atoi(b), 0, 6) : -1;

  char    pages[CRK_PAGES_MAX][CRK_PAGE_LEN + 1];
  uint8_t count = 0;
  memset(pages, 0, sizeof(pages));
  char *p = bar + 1;
  while (p && count < CRK_PAGES_MAX) {
    char *next = strchr(p, '|');
    if (next) *next++ = '\0';
    while (*p == ' ') p++;
    size_t n = strlen(p);
    while (n && p[n - 1] == ' ') n--;
    if (n > CRK_PAGE_LEN) n = CRK_PAGE_LEN;
    if (n) {
      for (size_t i = 0; i < n; i++) pages[count][i] = (char)toupper((unsigned char)p[i]);
      count++;
    }
    p = next;
  }
  if (count == 0) return false;

  const unsigned long now = millis();
  if (crkActive && crkBalls > 0 && balls == 0) {
    crkWiping = true;  crkWipeMs = now;
  } else if (crkActive && crkBalls >= 0 && balls > crkBalls) {
    crkNewBall = true; crkNewBallMs = now;
  }
  memcpy(crkPages, pages, sizeof(crkPages));
  char scoreTeam[CRK_PAGE_LEN + 1];
  int  scoreRuns, scoreWkts;
  const bool isScore = crkParseScore(pages[0], scoreTeam, sizeof(scoreTeam), scoreRuns, scoreWkts);
  if (isScore && crkRuns >= 0 && !strcmp(scoreTeam, crkRunsTeam) && scoreRuns > crkRuns && scoreRuns - crkRuns <= 9) {
    crkPop  = true; crkPopVal = scoreRuns - crkRuns; crkPopMs = now;   // "+N" and a brightness kick
    crkGlow = true; crkGlowMs = now;
  }
  crkRuns = isScore ? scoreRuns : -1;
  strlcpy(crkRunsTeam, isScore ? scoreTeam : "", sizeof(crkRunsTeam));
  crkPageCount    = count;
  crkBalls        = (int8_t)balls;
  crkLastUpdateMs = now;
  crkActive       = true;
  crkFromNet      = fromNet;
  if (!fromNet) {
    crkExternalSeen = true;
    crkExternalMs   = now;
  }
  if (mode == MODE_TEXT || mode == MODE_EYES) startCricket();
  return true;
}

// Queue an animation: "<code> [text]" with code 4, 6, W, 50, 100 or WIN. The
// text scrolls after a 50, a 100 and a WIN (a name or the result).
static bool cricketEvent(const char *arg) {
  while (*arg == ' ') arg++;
  char   code[8];
  size_t n = 0;
  while (*arg && *arg != ' ' && n < sizeof(code) - 1) code[n++] = (char)toupper((unsigned char)*arg++);
  code[n] = '\0';
  while (*arg == ' ') arg++;

  CrkAnim a = ANIM_NONE;
  if      (!strcmp(code, "4"))   a = ANIM_FOUR;
  else if (!strcmp(code, "6"))   a = ANIM_SIX;
  else if (!strcmp(code, "W"))   a = ANIM_WICKET;
  else if (!strcmp(code, "50"))  a = ANIM_FIFTY;
  else if (!strcmp(code, "100")) a = ANIM_HUNDRED;
  else if (!strcmp(code, "WIN")) a = ANIM_WIN;
  if (a == ANIM_NONE) return false;

  if (mode != MODE_CRICKET) {
    Serial.println("Cricket event ignored: the scoreboard is not showing");
    return true;
  }
  if (crkQueueLen >= CRK_QUEUE_MAX) {
    Serial.println("Cricket event dropped: queue full");
    return true;
  }
  CrkEvent &ev = crkQueue[crkQueueLen++];
  ev.anim = a;
  size_t i = 0;
  for (; arg[i] && i < CRK_PAGE_LEN; i++) ev.text[i] = (char)toupper((unsigned char)arg[i]);
  ev.text[i] = '\0';
  Serial.printf("Cricket event: %s %s\n", code, ev.text);
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Live scores over Wi-Fi: network task
// ─────────────────────────────────────────────────────────────────────────────

// Join the saved home network next to our own access point, or drop back to
// access point only. Called from setup() and loop().
static void wifiStartStation() {
  if (wifiSsid[0]) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifiSsid, wifiPass);
    Serial.printf("WiFi: joining \"%s\"\n", wifiSsid);
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    Serial.println("WiFi: no home network set, access point only");
  }
  WiFi.setTxPower(WIFI_TX_POWER);
}

static void setWifi(const char *ssid, const char *pass) {
  portENTER_CRITICAL(&netMux);
  strlcpy(wifiSsid, ssid, sizeof(wifiSsid));
  strlcpy(wifiPass, pass, sizeof(wifiPass));
  portEXIT_CRITICAL(&netMux);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  wifiStartStation();
  netReset = true;
  netKick  = true;
}

static void setCricketAuto(bool on, const char *team) {
  portENTER_CRITICAL(&netMux);
  crkAuto = on;
  size_t i = 0;
  for (; team[i] && i < sizeof(crkTeam) - 1; i++) crkTeam[i] = (char)toupper((unsigned char)team[i]);
  crkTeam[i] = '\0';
  portEXIT_CRITICAL(&netMux);
  prefs.putBool("auto", crkAuto);
  prefs.putString("team", crkTeam);
  Serial.printf("Cricket: live scores over Wi-Fi %s, team \"%s\"\n", on ? "on" : "off", crkTeam);
  netReset = true;
  netKick  = true;
}

static void netSetStatus(const char *s) {
  portENTER_CRITICAL(&netMux);
  strlcpy(netStatus, s, sizeof(netStatus));
  portEXIT_CRITICAL(&netMux);
}

// Sleep, but wake at once when the settings change.
static void netWait(unsigned long ms) {
  const unsigned long start = millis();
  while (millis() - start < ms && !netKick) vTaskDelay(pdMS_TO_TICKS(200));
  netKick = false;
}

static void netPublish(const String &payload, const String *events, uint8_t count) {
  portENTER_CRITICAL(&netMux);
  strlcpy(netPayload, payload.c_str(), sizeof(netPayload));
  netEventCount = count;
  for (uint8_t i = 0; i < count; i++) strlcpy(netEvents[i], events[i].c_str(), sizeof(netEvents[0]));
  netHasScore = true;
  netWantStop = false;
  portEXIT_CRITICAL(&netMux);
}

static void netRequestStop() {
  portENTER_CRITICAL(&netMux);
  netHasScore = false;
  netWantStop = true;
  portEXIT_CRITICAL(&netMux);
}

// Socket reader with a deadline, so a stalled server cannot hang the task. Also
// what ArduinoJson parses from, which keeps the reply out of RAM.
struct NetReader {
  WiFiClient   &client;
  unsigned long deadline;
  size_t        count = 0;
  uint8_t       buf[512];             // one socket read per 512 bytes: a read per byte is ~1 ms each
  size_t        pos = 0, len = 0;
  unsigned      fills = 0, idles = 0;   // diagnostics

  int read() {
    while (pos >= len) {
      const int avail = client.available();
      if (avail > 0) {
        const int got = client.read(buf, avail < (int)sizeof(buf) ? avail : (int)sizeof(buf));
        if (got > 0) {
          pos = 0;
          len = (size_t)got;
          fills++;
          break;
        }
      }
      if (!client.connected() || (long)(millis() - deadline) >= 0) return -1;
      idles++;
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    count++;
    return buf[pos++];
  }
  size_t readBytes(char *buf, size_t len) {
    size_t n = 0;
    while (n < len) {
      const int c = read();
      if (c < 0) break;
      buf[n++] = (char)c;
    }
    return n;
  }
};

// GET http://host/path and parse the JSON reply, keeping only what `filter` names.
static bool netGetJson(const char *host, const String &path, JsonDocument &doc, const JsonDocument &filter) {
  WiFiClient client;
  if (!client.connect(host, 80, 6000)) {
    Serial.printf("Net: connect to %s failed\n", host);
    return false;
  }
  client.print(String("GET ") + path + " HTTP/1.0\r\nHost: " + host +
               "\r\nUser-Agent: Mozilla/5.0\r\nAccept: application/json\r\nConnection: close\r\n\r\n");

  NetReader reader{client, millis() + NET_TIMEOUT_MS};
  char   statusLine[16];
  size_t n = 0;
  int    c;
  while ((c = reader.read()) >= 0 && c != '\n') {
    if (n < sizeof(statusLine) - 1) statusLine[n++] = (char)c;
  }
  statusLine[n] = '\0';
  bool ok = (n >= 12 && strncmp(statusLine, "HTTP/1.", 7) == 0 && strncmp(statusLine + 9, "200", 3) == 0);
  if (!ok) Serial.printf("Net: bad status line \"%s\" fills=%u idles=%u connected=%d timed out=%d\n", statusLine,
                         reader.fills, reader.idles, client.connected() ? 1 : 0,
                         (long)(millis() - reader.deadline) >= 0 ? 1 : 0);

  int lineLen = 0;                                  // skip the headers, up to the blank line
  while (ok) {
    c = reader.read();
    if (c < 0) ok = false;
    else if (c == '\n') { if (lineLen == 0) break; lineLen = 0; }
    else if (c != '\r') lineLen++;
  }
  if (ok) {
    const DeserializationError err = deserializeJson(doc, reader, DeserializationOption::Filter(filter),
                                                     DeserializationOption::NestingLimit(16));
    ok = !err;
    if (err) Serial.printf("Net: JSON error %s after %u body bytes, fills=%u idles=%u, connected=%d, timed out=%d\n", err.c_str(),
                           (unsigned)reader.count, reader.fills, reader.idles, client.connected() ? 1 : 0,
                           (long)(millis() - reader.deadline) >= 0 ? 1 : 0);
  }
  client.stop();
  return ok;
}

// "404/4 (102 ov)" -> "404/4", "144 & 178" -> "144 + 178"
static String netCleanScore(const char *s) {
  String out;
  int depth = 0;
  for (; *s; s++) {
    if (*s == '(') depth++;
    else if (*s == ')') { if (depth > 0) depth--; }
    else if (depth == 0) out += (*s == '&') ? '+' : *s;
  }
  out.trim();
  return out;
}

static bool netContainsNoCase(const char *hay, const char *needle) {
  const size_t n = strlen(needle);
  if (n == 0) return true;
  for (; *hay; hay++) {
    if (strncasecmp(hay, needle, n) == 0) return true;
  }
  return false;
}

// Find a live international match. 1 = found (ids filled in), 0 = none, -1 = could not ask.
static int netFindMatch(const char *team, const char *skipEvent, char *league, char *event, size_t idLen) {
  JsonDocument filter;
  JsonObject   fe = filter["sports"][0]["leagues"][0]["events"][0].to<JsonObject>();
  filter["sports"][0]["leagues"][0]["id"] = true;
  fe["id"]     = true;
  fe["status"] = true;
  fe["class"]["internationalClassId"]    = true;
  fe["competitors"][0]["abbreviation"]   = true;
  fe["competitors"][0]["displayName"]    = true;

  JsonDocument doc;
  if (!netGetJson(NET_HOST_FIND, NET_PATH_FIND, doc, filter)) return -1;

  // Men's Tests, ODIs and T20Is (classes 1 to 3) first, then the other international classes
  JsonArray leagues = doc["sports"][0]["leagues"];
  for (int pass = 0; pass < 2; pass++) {
    for (JsonObject lg : leagues) {
      JsonArray evs = lg["events"];
      for (JsonObject ev : evs) {
        if (strcmp(ev["status"] | "", "in") != 0) continue;
        const char *cls = ev["class"]["internationalClassId"] | "0";
        if (!cls[0] || !strcmp(cls, "0")) continue;
        const bool mainClass = !strcmp(cls, "1") || !strcmp(cls, "2") || !strcmp(cls, "3");
        if ((pass == 0) != mainClass) continue;
        const char *id = ev["id"] | "";
        if (!id[0] || !strcmp(id, skipEvent)) continue;
        if (team[0]) {
          bool hit = false;
          JsonArray sides = ev["competitors"];
          for (JsonObject c : sides) {
            if (!strcasecmp(c["abbreviation"] | "", team) || netContainsNoCase(c["displayName"] | "", team)) hit = true;
          }
          if (!hit) continue;
        }
        strlcpy(league, lg["id"] | "", idLen);
        strlcpy(event, id, idLen);
        return league[0] ? 1 : 0;
      }
    }
  }
  return 0;
}

struct NetTrack {
  char          league[12];
  char          event[12];
  char          prevKey[40];     // innings the previous score belongs to
  int           prevRuns;
  int           prevWkts;
  bool          hasPrev;
  bool          sawLive;
  unsigned long resultSince;     // 0 until the match is over
  uint8_t       misses;
};

// Fetch the followed match and publish its pages. 1 = live, 2 = finished,
// 0 = the match is not listed, -1 = could not ask.
static int netPollMatch(NetTrack &t) {
  JsonDocument filter;
  filter["events"][0]["id"] = true;
  JsonObject fc = filter["events"][0]["competitions"][0].to<JsonObject>();
  fc["class"]["eventType"]             = true;
  fc["status"]["summary"]              = true;
  fc["status"]["type"]["description"]  = true;
  fc["status"]["type"]["state"]        = true;
  JsonObject ft = fc["competitors"][0].to<JsonObject>();
  ft["score"]                 = true;
  ft["team"]["abbreviation"]  = true;
  ft["team"]["displayName"]   = true;
  JsonObject fl = ft["linescores"][0].to<JsonObject>();
  fl["runs"] = true; fl["wickets"] = true; fl["overs"] = true;
  fl["isBatting"] = true; fl["isCurrent"] = true; fl["period"] = true;

  JsonDocument doc;
  if (!netGetJson(NET_HOST_MATCH, String("/apis/site/v2/sports/cricket/") + t.league + "/scoreboard", doc, filter)) return -1;

  JsonObject comp;
  JsonArray evs = doc["events"];
  for (JsonObject ev : evs) {
    if (!strcmp(ev["id"] | "", t.event)) comp = ev["competitions"][0];
  }
  JsonArray teams = comp["competitors"];
  if (comp.isNull() || teams.size() != 2) return 0;

  const char *state = comp["status"]["type"]["state"] | "";
  const char *desc  = comp["status"]["type"]["description"] | "";
  String      summary = comp["status"]["summary"] | "";
  for (JsonObject tm : teams) {                          // "Sri Lanka trail by 30 runs" -> "SL trail by 30 runs"
    const char *full = tm["team"]["displayName"] | "";
    const char *abbr = tm["team"]["abbreviation"] | "";
    if (full[0] && abbr[0]) summary.replace(full, abbr);
  }
  summary = netCleanScore(summary.c_str());
  const String versus = String(teams[0]["team"]["abbreviation"] | "?") + " V " + String(teams[1]["team"]["abbreviation"] | "?");
  netSetStatus((String("Following ") + versus).c_str());

  String  payload;
  String  events[3];
  uint8_t eventCount = 0;

  if (!strcmp(state, "post")) {
    if (!t.resultSince) {
      t.resultSince = millis() | 1;
      if (t.sawLive) events[eventCount++] = String("WIN ") + summary;
      Serial.printf("Cricket: result - %s\n", summary.c_str());
    }
    payload = String("-|") + versus + "|" + summary;
    for (JsonObject tm : teams) {
      const char *score = tm["score"] | "";
      if (score[0]) payload += String("|") + (tm["team"]["abbreviation"] | "?") + " " + netCleanScore(score);
    }
    netPublish(payload, events, eventCount);
    return 2;
  }

  JsonObject bat, innings;
  for (JsonObject tm : teams) {
    JsonArray lines = tm["linescores"];
    for (JsonObject ls : lines) {
      if ((ls["isCurrent"] | 0) && (ls["isBatting"] | false)) { bat = tm; innings = ls; }
    }
  }
  if (strcmp(state, "in") != 0 || bat.isNull()) {         // not started yet, or between innings with no batting side
    payload = String("-|") + versus;
    if (desc[0]) payload += String("|") + desc;
    netPublish(payload, events, 0);
    return 1;
  }

  const String batAbbr = bat["team"]["abbreviation"] | "?";
  const int    runs    = innings["runs"] | 0;
  const int    wkts    = innings["wickets"] | 0;
  const int    period  = innings["period"] | 0;
  const int    tenths  = (int)lroundf((innings["overs"] | 0.0f) * 10.0f);
  const int    whole   = tenths / 10;
  const int    part    = tenths % 10;                     // balls bowled in this over
  const int    bowled  = whole * 6 + part;

  // The score text carries what the numbers do not: "(18.3/20 ov, target 160)"
  const char *score  = bat["score"] | "";
  int         target = 0, limit = 0;
  const char *tg = strstr(score, "target ");
  if (tg) target = atoi(tg + 7);
  const char *open = strrchr(score, '(');
  if (open) {
    const char *slash = strchr(open, '/');
    const char *ov    = strstr(open, " ov");
    if (slash && ov && slash < ov) limit = atoi(slash + 1);
  }
  if (!limit) {
    const char *type = comp["class"]["eventType"] | "";
    if (!strcmp(type, "T20")) limit = 20;
    else if (!strcmp(type, "ODI")) limit = 50;
  }
  const bool chasing = target > runs;

  char buf[40];
  payload = String(part > 6 ? 6 : part) + "|" + batAbbr + " " + runs;
  if (wkts < 10) payload += String("/") + wkts;
  snprintf(buf, sizeof(buf), "|OV %d.%d", whole, part);
  payload += buf;
  if (chasing) {
    payload += String("|NEED ") + (target - runs);
    if (limit * 6 > bowled) payload += String("|") + (limit * 6 - bowled) + " BALLS";
  } else if (limit && bowled) {
    snprintf(buf, sizeof(buf), "|CRR %.2f", runs * 6.0f / bowled);
    payload += buf;
  }
  for (JsonObject tm : teams) {
    const char *other = tm["score"] | "";
    if (tm != bat && other[0]) payload += String("|") + (tm["team"]["abbreviation"] | "?") + " " + netCleanScore(other);
  }
  if (desc[0] && strcasecmp(desc, "Live") != 0) payload += String("|") + desc;     // STUMPS, INNINGS BREAK...
  if (summary.length() && !chasing) payload += String("|") + summary;

  // No boundary counts in this feed: read a four or a six from the jump in the total
  snprintf(buf, sizeof(buf), "%s/%s/%d", t.event, batAbbr.c_str(), period);
  if (t.hasPrev && !strcmp(buf, t.prevKey)) {
    const int gained = runs - t.prevRuns;
    if (gained == 4 || gained == 5)      events[eventCount++] = "4";
    else if (gained == 6 || gained == 7) events[eventCount++] = "6";
    if (wkts > t.prevWkts)               events[eventCount++] = "W";
  }
  strlcpy(t.prevKey, buf, sizeof(t.prevKey));
  t.prevRuns = runs;
  t.prevWkts = wkts;
  t.hasPrev  = true;
  t.sawLive  = true;

  netPublish(payload, events, eventCount);
  return 1;
}

static void netTask(void *) {
  NetTrack t;
  char     skip[sizeof(t.event)] = "";      // finished match we have stopped showing
  bool     wasOn = false;
  memset(&t, 0, sizeof(t));

  for (;;) {
    char team[sizeof(crkTeam)];
    bool on, haveWifi;
    portENTER_CRITICAL(&netMux);
    haveWifi = wifiSsid[0] != '\0';
    on       = crkAuto && haveWifi;
    memcpy(team, crkTeam, sizeof(team));
    portEXIT_CRITICAL(&netMux);

    if (netReset) {
      netReset = false;
      memset(&t, 0, sizeof(t));
      skip[0] = '\0';
    }
    if (!on) {
      if (wasOn) netRequestStop();
      wasOn = false;
      netSetStatus(haveWifi ? "Live scores are switched off" : "No home Wi-Fi set");
      netWait(2000);
      continue;
    }
    wasOn = true;
    if (WiFi.status() != WL_CONNECTED) {
      netSetStatus("Connecting to Wi-Fi");
      netWait(2000);
      continue;
    }

    if (!t.event[0]) {
      const int found = netFindMatch(team, skip, t.league, t.event, sizeof(t.event));
      if (found < 0) {
        netSetStatus("Could not reach the score server");
        netWait(15000);
        continue;
      }
      if (found == 0) {
        memset(&t, 0, sizeof(t));
        netRequestStop();
        netSetStatus("No live international match");
        netWait(NET_FIND_MS);
        continue;
      }
      Serial.printf("Cricket: following match %s (series %s)\n", t.event, t.league);
    }

    const int r = netPollMatch(t);
    if (r < 0) {
      netSetStatus("Could not reach the score server");
    } else if (r == 0) {
      if (++t.misses >= 3) {                 // the match dropped off the scoreboard
        memset(&t, 0, sizeof(t));
        netRequestStop();
      }
    } else {
      t.misses = 0;
      if (r == 2 && millis() - t.resultSince > NET_RESULT_MS) {
        strlcpy(skip, t.event, sizeof(skip));
        memset(&t, 0, sizeof(t));
        netRequestStop();
      }
    }
    netWait(NET_POLL_MS);
  }
}

// Take what the network task found and put it on the scoreboard.
static void applyNetResult() {
  bool    has, stop;
  uint8_t count = 0;
  char    payload[sizeof(netPayload)];
  char    events[3][sizeof(netEvents[0])];

  portENTER_CRITICAL(&netMux);
  has  = netHasScore;
  stop = netWantStop;
  if (has) {
    memcpy(payload, netPayload, sizeof(payload));
    memcpy(events, netEvents, sizeof(events));
    count = netEventCount;
  }
  netHasScore   = false;
  netWantStop   = false;
  netEventCount = 0;
  portEXIT_CRITICAL(&netMux);

  if (stop) {
    if (crkActive && crkFromNet) stopCricket();
    return;
  }
  if (!has) return;
  // The PC script or the page is feeding the board: theirs is the better score
  if (crkExternalSeen && millis() - crkExternalMs < NET_HOLD_MS) return;
  if (!cricketSet(payload, true)) return;
  for (uint8_t i = 0; i < count; i++) cricketEvent(events[i]);
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
  char          cricket[CRK_LINE_MAX + 1];
  char          pass[sizeof(pendingPass)];
  bool          keepPass;

  portENTER_CRITICAL(&cmdMux);
  cmd        = pendingCmd;
  pendingCmd = CMD_NONE;
  memcpy(text, pendingText, sizeof(text));
  memcpy(cricket, pendingCricket, sizeof(cricket));
  memcpy(pass, pendingPass, sizeof(pass));
  keepPass = pendingKeepPass;
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

    case CMD_CRICKET:
      cricketSet(cricket);
      break;

    case CMD_CRICKET_EVENT:
      cricketEvent(cricket);
      break;

    case CMD_CRICKET_STOP:
      stopCricket();
      break;

    case CMD_SET_WIFI:
      // No password sent for the network we already know: keep the saved one
      if (keepPass && !strcmp(text, wifiSsid)) strlcpy(pass, wifiPass, sizeof(pass));
      setWifi(text, pass);
      break;

    case CMD_SET_AUTO:
      setCricketAuto(flip, text);
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
//   q<score>        cricket score, e.g. "q 2|IND 187/4|OV 18.2|NEED 24" (see cricketSet)
//   v<event>        cricket animation: "v 4", "v 6", "v W", "v 50 KOHLI 50*", "v 100 ...", "v WIN ..."
//   Q               stop the cricket scoreboard
static void handleSerialConsole() {
  static char     line[CRK_LINE_MAX + 8];
  static uint16_t len = 0;

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
      case 'q':
        if (!cricketSet(p + 1)) Serial.println("Cricket: expected q <balls>|PAGE|PAGE...  e.g. q 2|IND 187/4|OV 18.2");
        else if (mode != MODE_CRICKET) Serial.println("Cricket score saved; shows when the display is free");
        break;
      case 'v':
        if (!cricketEvent(p + 1)) Serial.println("Cricket: expected v 4 | v 6 | v W | v 50 TEXT | v 100 TEXT | v WIN TEXT");
        break;
      case 'Q':
        stopCricket();
        break;
      case 'w': {                      // w SSID|PASSWORD  join a home network, "w -" forget it, "w" status
        char *q = p + 1;
        while (*q == ' ') q++;
        if (!*q) {
          Serial.printf("WiFi: home network \"%s\" %s, IP %s | live scores %s, team \"%s\" | %s\n",
                        wifiSsid, !wifiSsid[0] ? "not set" : (WiFi.status() == WL_CONNECTED) ? "connected" : "not connected",
                        WiFi.localIP().toString().c_str(), crkAuto ? "on" : "off", crkTeam, netStatus);
        } else if (!strcmp(q, "-")) {
          setWifi("", "");
        } else {
          char *bar = strchr(q, '|');
          if (bar) *bar++ = '\0';
          if (strlen(q) >= sizeof(wifiSsid) || (bar && strlen(bar) >= sizeof(wifiPass))) Serial.println("WiFi: name or password too long");
          else setWifi(q, bar ? bar : "");
        }
        break;
      }
      case 'l': {                      // l 1 [TEAM]  live scores over Wi-Fi on, "l 0" off
        const char *q = p + 1;
        while (*q == ' ') q++;
        const bool on = (*q != '0');
        if (*q) q++;
        while (*q == ' ') q++;
        setCricketAuto(on, q);
        break;
      }
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
        Serial.println("Commands: t<sec>[f] start timer | c cancel | x<text> set text | e eyes | a<text> alert until cleared | k clear alert | f dump the eyes/cricket frame | s status | d self-test | m<0-7> module type | q<balls>|PAGE|.. cricket score | v<4|6|W|50|100|WIN> [text] cricket animation | Q stop cricket | w<ssid>|<password> join home Wi-Fi, w - forget, w status | l<0|1> [team] live scores over Wi-Fi");
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
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("WiFi: joined the home network, IP %s (the control page is there too)\n",
                    IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("WiFi: home network not connected (reason %d), retrying\n", (int)info.wifi_sta_disconnected.reason);
      break;
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
  strlcpy(wifiSsid, prefs.getString("ssid", "").c_str(), sizeof(wifiSsid));
  strlcpy(wifiPass, prefs.getString("pass", "").c_str(), sizeof(wifiPass));
  strlcpy(crkTeam,  prefs.getString("team", "").c_str(), sizeof(crkTeam));
  crkAuto = prefs.getBool("auto", true);

  // LED matrix
  P.begin();
  P.setIntensity(CRK_BASE_INTENSITY);
  mx = P.getGraphicObject();
  eyeL.init(EYE_GAP_LEFT - EYE_W, (8 - EYE_H) / 2, EYE_W, EYE_H);
  eyeR.init(EYE_GAP_RIGHT,        (8 - EYE_H) / 2, EYE_W, EYE_H);
  applyModuleType(moduleTypeIdx, false);   // sets the mapping and draws the text
  startIdle();                              // switch to eyes if that was the last choice

  // Access point
  WiFi.persistent(false);
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(wifiSsid[0] ? WIFI_AP_STA : WIFI_AP);
  const bool apOk = WiFi.softAP(ssid, nullptr, WIFI_CHANNEL, 0, WIFI_MAX_CLIENTS);
  WiFi.setTxPower(WIFI_TX_POWER);   // must come after softAP; see WIFI_TX_POWER note
  WiFi.setSleep(false);             // no modem power-save: keeps beacons and replies prompt
  Serial.printf("AP %s. SSID: %s\n", apOk ? "started" : "FAILED", ssid);
  Serial.printf("Channel %d, TX power code %d, MAC %s\n", WIFI_CHANNEL, (int)WiFi.getTxPower(), WiFi.softAPmacAddress().c_str());
  Serial.print("Open http://");
  Serial.println(WiFi.softAPIP());

  // Home network for live scores, if one is saved. The access point stays up
  // next to it, but moves to the home network's channel.
  if (wifiSsid[0]) wifiStartStation();
  xTaskCreate(netTask, "net", 8192, nullptr, 1, nullptr);

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

  // Home network for live scores: /wifi?ssid=NAME&pass=PASSWORD. An empty ssid
  // forgets it. Without pass the saved password is kept for the same network.
  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("ssid")) {
      request->send(400, "text/plain", "Missing ssid parameter");
      return;
    }
    const String ssid    = request->getParam("ssid")->value();
    const bool   hasPass = request->hasParam("pass");
    const String pass    = hasPass ? request->getParam("pass")->value() : String();
    if (ssid.length() >= sizeof(wifiSsid) || pass.length() >= sizeof(wifiPass) || (pass.length() > 0 && pass.length() < 8)) {
      request->send(400, "text/plain", "Name up to 32 characters, password 8 to 64 characters");
      return;
    }

    portENTER_CRITICAL(&cmdMux);
    strlcpy(pendingText, ssid.c_str(), sizeof(pendingText));
    strlcpy(pendingPass, pass.c_str(), sizeof(pendingPass));
    pendingKeepPass = !hasPass;
    pendingCmd      = CMD_SET_WIFI;
    portEXIT_CRITICAL(&cmdMux);

    request->send(200, "text/plain", ssid.length() ? "Joining the network" : "Network forgotten");
  });

  // Live scores over Wi-Fi: /cricketAuto?on=0|1&team=IND (team optional)
  server.on("/cricketAuto", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("on")) {
      request->send(400, "text/plain", "Missing on parameter");
      return;
    }
    String team = request->hasParam("team") ? request->getParam("team")->value() : String();
    team.trim();
    if (team.length() >= sizeof(crkTeam)) {
      request->send(400, "text/plain", "Team is too long");
      return;
    }

    portENTER_CRITICAL(&cmdMux);
    strlcpy(pendingText, team.c_str(), sizeof(pendingText));
    pendingFlip = request->getParam("on")->value() == "1";
    pendingCmd  = CMD_SET_AUTO;
    portEXIT_CRITICAL(&cmdMux);

    request->send(200, "text/plain", "Saved");
  });

  // Cricket animation: /cricketEvent?e=4|6|W|50|100|WIN&text=...
  server.on("/cricketEvent", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("e")) {
      request->send(400, "text/plain", "Missing e parameter");
      return;
    }
    String arg = request->getParam("e")->value();
    arg.trim();
    if (arg != "4" && arg != "6" && arg != "W" && arg != "50" && arg != "100" && arg != "WIN") {
      request->send(400, "text/plain", "e must be 4, 6, W, 50, 100 or WIN");
      return;
    }
    if (request->hasParam("text")) {
      arg += ' ';
      arg += request->getParam("text")->value();
    }

    portENTER_CRITICAL(&cmdMux);
    strncpy(pendingCricket, arg.c_str(), CRK_LINE_MAX);
    pendingCricket[CRK_LINE_MAX] = '\0';
    pendingCmd = CMD_CRICKET_EVENT;
    portEXIT_CRITICAL(&cmdMux);

    request->send(200, "text/plain", "Animation queued");
  });

  server.on("/cricketStop", HTTP_GET, [](AsyncWebServerRequest *request) {
    portENTER_CRITICAL(&cmdMux);
    pendingCmd = CMD_CRICKET_STOP;
    portEXIT_CRITICAL(&cmdMux);
    request->send(200, "text/plain", "Scoreboard stopped");
  });

  // Cricket score: /cricket?d=<balls>|PAGE|PAGE...  e.g. d=2|IND 187/4|OV 18.2
  server.on("/cricket", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("d")) {
      request->send(400, "text/plain", "Missing d parameter");
      return;
    }
    const String d = request->getParam("d")->value();
    const int bar = d.indexOf('|');
    if (bar < 0 || bar == (int)d.length() - 1) {
      request->send(400, "text/plain", "d must be <balls>|PAGE|PAGE...");
      return;
    }

    portENTER_CRITICAL(&cmdMux);
    strncpy(pendingCricket, d.c_str(), CRK_LINE_MAX);
    pendingCricket[CRK_LINE_MAX] = '\0';
    pendingCmd = CMD_CRICKET;
    portEXIT_CRITICAL(&cmdMux);

    request->send(200, "text/plain", "Score updated");
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
    json.reserve(900);
    json += "{\"mode\":\"";
    json += (m == MODE_TEXT) ? "text" : (m == MODE_TIMER) ? "timer" : (m == MODE_DONE) ? "done"
          : (m == MODE_EYES) ? "eyes" : (m == MODE_ALERT) ? "alert" : (m == MODE_CRICKET) ? "cricket" : "selftest";
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
    json += "\",\"cricket\":\"";
    if (crkActive) {
      const uint8_t pages = crkPageCount;
      for (uint8_t i = 0; i < pages; i++) {
        if (i) json += " | ";
        json += jsonEscape(crkPages[i]);
      }
    }
    char net[sizeof(netStatus)];
    portENTER_CRITICAL(&netMux);
    memcpy(net, netStatus, sizeof(net));
    portEXIT_CRITICAL(&netMux);
    json += "\",\"wifi\":\"";
    json += jsonEscape(wifiSsid);
    json += "\",\"wifiState\":\"";
    if (!wifiSsid[0])                       json += "off";
    else if (WiFi.status() == WL_CONNECTED) json += "connected, " + WiFi.localIP().toString();
    else                                    json += "not connected";
    json += "\",\"auto\":";
    json += crkAuto ? "true" : "false";
    json += ",\"team\":\"";
    json += jsonEscape(crkTeam);
    json += "\",\"net\":\"";
    json += jsonEscape(net);
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
  applyNetResult();

  const unsigned long now = millis();

  // The PC script re-sends the score every few seconds. If it goes quiet, drop
  // the scoreboard instead of showing a stale score for ever.
  if (crkActive && now - crkLastUpdateMs > CRK_STALE_MS) {
    Serial.println("Cricket: no update from the PC, leaving the scoreboard");
    stopCricket();
  }

  // The scoreboard plays with the brightness; put it back for everything else
  if (mode != MODE_CRICKET && crkIntensityShown != CRK_BASE_INTENSITY) {
    mx->control(MD_MAX72XX::INTENSITY, CRK_BASE_INTENSITY);
    crkIntensity = crkIntensityShown = CRK_BASE_INTENSITY;
  }

  switch (mode) {
    case MODE_CRICKET:
      if (now - lastCrkFrameMs >= CRK_FRAME_MS) {
        lastCrkFrameMs = now;
        cricketTick(now);
      }
      break;

    case MODE_TEXT:
      if (P.displayAnimate()) {
        P.displayReset();
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
