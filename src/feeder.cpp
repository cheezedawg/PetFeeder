/*
    Feeder class implementation
*/
#include "feeder.h"

#include <pgmspace.h>
#include <stdio.h>
#include <string.h>
#include <memory>
#include <new>

#if defined(ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif

// Commands queued by HTTP handlers and applied on the loop task.
static const uint8_t CMD_NONE = 0;
static const uint8_t CMD_START = 1;
static const uint8_t CMD_CANCEL = 2;

#if defined(ESP32)
static portMUX_TYPE feederMux = portMUX_INITIALIZER_UNLOCKED;
#define FEEDER_LOCK() portENTER_CRITICAL(&feederMux)
#define FEEDER_UNLOCK() portEXIT_CRITICAL(&feederMux)
#else
#define FEEDER_LOCK() noInterrupts()
#define FEEDER_UNLOCK() interrupts()
#endif

/*
    Default constructor
    Feeder::Feeder()
    Members only. Servo and EEPROM setup happen in begin(), which is
    called from setup() after the core has initialized.
*/
Feeder::Feeder()
  : webServer(nullptr),
    state(idle),
    iteration(0),
    cycleIterations(ITERATIONS),
    pendingCommand(CMD_NONE),
    publishedState((uint8_t)idle),
    hardwareReady(false),
    routesReady(false) {
  feedParams.pForward = FORWARD;
  feedParams.pBack = BACK;
  feedParams.pPause = PAUSE;
  feedParams.pRest = REST;
  feedParams.pIterations = ITERATIONS;
  feedParams.check = paramsCheck(feedParams);
}
/*
    Destructor
    Feeder::~Feeder()
*/
Feeder::~Feeder() {

}

/*
    Begin the service
    Feeder::begin(AsyncWebServer *server)
    Parameters:
        *server: Pointer to an AsyncWebServer object
    Returns:
        void
    Set up the servo, EEPROM, and web server handlers for
        /
        /feed
        /cancel
        /updateparams
    Does not replace the server's existing 404 handler.
*/
void Feeder::begin(AsyncWebServer *server) {
  if (server == nullptr) {
    Serial.println("Feeder: begin() needs a server");
    return;
  }
  if (!hardwareReady) {
    startHardware();
    hardwareReady = true;
  }
  if (routesReady) {
    return;
  }
  webServer = server;
  webServer->on("/", HTTP_GET, [&](AsyncWebServerRequest *request) {
    getMainPage(request);
  });
  webServer->on("/feed", HTTP_GET, [&](AsyncWebServerRequest *request) {
    getFeedPage(request);
  });
  webServer->on("/cancel", HTTP_GET, [&](AsyncWebServerRequest *request) {
    getCancelPage(request);
  });
  webServer->on("/updateparams", HTTP_POST, [&](AsyncWebServerRequest *request) {
    postUpdateParamsPage(request);
  });
  routesReady = true;
}

/*
    Check the feeding status
    Feeder::checkFeeding()
    Parameters:
        None
    Returns:
        void
    This is called every loop in the main sketch
    All of the timers and webserver are non-blocking
    so we keep of the current feeding state in the 
    state variable in the class. The feeding flow is:
        1. Servo turns "forward" for the configured forwardTime timer
        2. Servo pauses for the configured forwardPause timer
        3. Servo goes backwards for the configured backTime timer (to clear any jams)
        4. Servo pauses for the configured restTime timer
        5. Repeat steps 1-4 for the configured number of iterations
*/
void Feeder::checkFeeding() {
  if (!hardwareReady) {
    return;
  }
  // Publish the resulting state before clearing the command. A page read
  // then sees either the pending command or the new state, never neither.
  uint8_t cmd = __sync_fetch_and_add(&pendingCommand, 0);
  if (cmd == CMD_START || cmd == CMD_CANCEL) {
    if (cmd == CMD_CANCEL) {
      cancelFeeding();
    } else {
      startFeeding();
    }
    publishState();
    __sync_bool_compare_and_swap(&pendingCommand, cmd, CMD_NONE);
  }
  switch (state) {
    case forward:
      if (forwardTime.update()) {
        Serial.println("Feeder: Forward Done");
        auger.write(SERVO_STOP);
        state = forwardPause;
        pauseTime.start();
      }
      break;
    case forwardPause:
      if (pauseTime.update()) {
        Serial.println("Feeder: Pause Done");
        auger.write(SERVO_BACK);
        state = back;
        backTime.start();
      }
      break;
    case back:
      if (backTime.update()) {
        Serial.println("Feeder: Back Done");
        auger.write(SERVO_STOP);
        iteration++;
        Serial.print("Feeder: Iteration: ");
        Serial.println(iteration);
        state = rest;
        restTime.start();
      }
      break;
    case rest:
      if (restTime.update()) {
        Serial.println("Feeder: Rest Done");
        if (iteration < cycleIterations) {
          auger.write(SERVO_FORWARD);
          state = forward;
          forwardTime.start();
        } else {
          auger.write(SERVO_STOP);
          state = idle;
        }
      }
      break;
    default:
      break;
  }
  publishState();
}

/*
    Start a feeding cycle
    Feeder::startFeeding()
    Parameters:
        None
    Returns:
        void
    This function starts a feeding cycle by starting the 
    servo forward and starting the forwardTime timer and
    configuring the state to forward.
    The timer is non-blocking, so when it fires the checkfeeding()
    call will move it to the next state 
*/
void Feeder::startFeeding() {
  feedParameters params;
  FEEDER_LOCK();
  params = feedParams;
  FEEDER_UNLOCK();
  // Delays are applied here, at the start of a cycle, not when they are saved.
  applyDelays(params);
  cycleIterations = params.pIterations;
  auger.write(SERVO_FORWARD);
  state = forward;
  iteration = 0;
  forwardTime.start();
}

/*
    Cancel a feeding cycle
    Feeder::cancelFeeding()
    Parameters:
        None
    Returns:
        void
    This function stops the auger and returns the state back to idle
*/
void Feeder::cancelFeeding() {
  auger.write(SERVO_STOP);
  state = idle;
  iteration = 0;
}

/*
    Copy saved phase times into the timers. Call this when a cycle
    starts so a save during an earlier phase does not shorten or
    extend the movement that is already running.
*/
void Feeder::applyDelays(const feedParameters &params) {
  forwardTime.setdelay((unsigned long)params.pForward);
  pauseTime.setdelay((unsigned long)params.pPause);
  backTime.setdelay((unsigned long)params.pBack);
  restTime.setdelay((unsigned long)params.pRest);
}

int Feeder::paramsCheck(feedParameters params) const {
  return params.pForward + params.pBack + params.pPause + params.pRest + params.pIterations;
}

static bool inRange(int value, int minValue, int maxValue) {
  return value >= minValue && value <= maxValue;
}

bool Feeder::paramsAcceptable(const feedParameters &params) const {
  if (params.check != paramsCheck(params)) {
    return false;
  }
  return inRange(params.pForward, MIN_PHASE_MS, MAX_PHASE_MS)
      && inRange(params.pBack, MIN_PHASE_MS, MAX_PHASE_MS)
      && inRange(params.pPause, MIN_PHASE_MS, MAX_PHASE_MS)
      && inRange(params.pRest, MIN_PHASE_MS, MAX_PHASE_MS)
      && inRange(params.pIterations, MIN_ITERATIONS, MAX_ITERATIONS);
}

bool Feeder::commitParams(const feedParameters &params) {
  feedParameters stored = params;
  stored.check = paramsCheck(stored);
  EEPROM.put(0, stored);
  return EEPROM.commit();
}

void Feeder::loadOrInitParams() {
  EEPROM.begin(sizeof(feedParameters));
  feedParameters loaded;
  EEPROM.get(0, loaded);
  if (paramsAcceptable(loaded)) {
    Serial.println("Feeder: Valid parameters found in EEPROM");
    FEEDER_LOCK();
    feedParams = loaded;
    FEEDER_UNLOCK();
    return;
  }
  Serial.println("Feeder: No EEPROM parameters found. Updating...");
  feedParameters defaults;
  defaults.pForward = FORWARD;
  defaults.pBack = BACK;
  defaults.pPause = PAUSE;
  defaults.pRest = REST;
  defaults.pIterations = ITERATIONS;
  defaults.check = paramsCheck(defaults);
  EEPROM.wipe();
  if (commitParams(defaults)) {
    Serial.println("Feeder: EEPROM commit done");
  } else {
    Serial.println("Feeder: EEPROM commit failed");
  }
  // Keep the defaults in RAM either way so the feeder can still run.
  FEEDER_LOCK();
  feedParams = defaults;
  FEEDER_UNLOCK();
}

void Feeder::startHardware() {
  // SERVO_GPIO is the signal pin (GPIO4 unless that macro is changed).
  auger.attach(SERVO_GPIO);
  auger.write(SERVO_STOP);
  loadOrInitParams();
  state = idle;
  iteration = 0;
  __sync_lock_test_and_set(&pendingCommand, CMD_NONE);
  publishState();
}

void Feeder::publishState() {
  __sync_lock_test_and_set(&publishedState, (uint8_t)state);
}

void Feeder::issueCommand(uint8_t cmd) {
  __sync_lock_test_and_set(&pendingCommand, cmd);
}

/*
    Self-contained pages for the feeder web UI.
    Markup and CSS live in flash so the device does not fetch assets.
*/
static const char FEEDER_HEAD_OPEN[] PROGMEM = R"FEEDERHTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="theme-color" content="#f3eadf" media="(prefers-color-scheme: light)">
<meta name="theme-color" content="#161310" media="(prefers-color-scheme: dark)">
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'%3E%3Crect width='16' height='16' rx='4' fill='%231d6b45'/%3E%3C/svg%3E">
<title>
)FEEDERHTML";

static const char FEEDER_CSS[] PROGMEM = R"FEEDERCSS(
:root {
  --bg: #f3eadf;
  --glow: #fffaf3;
  --card: #fffdf9;
  --ink: #2b241c;
  --muted: #74685b;
  --line: #eadfce;
  --line-strong: #d9cbb8;
  --input: #faf6f0;
  --accent: #1d6b45;
  --accent-dark: #155437;
  --cancel: #a33b24;
  --cancel-dark: #862f1c;
  --save: #2b241c;
  --save-ink: #fffdf9;
  --idle: #1d6b45;
  --feeding: #c05621;
  --ring: rgba(29, 107, 69, 0.18);
  --shadow: 0 16px 40px rgba(70, 46, 22, 0.08);
  color-scheme: light;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #161310;
    --glow: #2c261f;
    --card: #221e1a;
    --ink: #f6f0e7;
    --muted: #b7ab9e;
    --line: #3a332b;
    --line-strong: #4a4238;
    --input: #1b1714;
    --save: #f3ecdf;
    --save-ink: #241e18;
    --idle: #3dbe7a;
    --feeding: #f0a06a;
    --ring: rgba(29, 107, 69, 0.35);
    --shadow: 0 16px 40px rgba(0, 0, 0, 0.35);
    color-scheme: dark;
  }
}
* { box-sizing: border-box; }
html { -webkit-text-size-adjust: 100%; }
body {
  margin: 0;
  min-height: 100vh;
  color: var(--ink);
  line-height: 1.45;
  font-family: ui-sans-serif, system-ui, -apple-system, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  background: radial-gradient(900px 420px at 50% -80px, var(--glow), transparent 70%), var(--bg);
  padding: env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left);
  -webkit-font-smoothing: antialiased;
}
button, input { font-family: inherit; }
::selection { background: rgba(29, 107, 69, 0.22); }
.page {
  width: min(32rem, calc(100% - 2rem));
  margin: 0 auto;
  padding: 1.75rem 0 3rem;
}
h1, h2, .status-word {
  font-family: Palatino, "Palatino Linotype", "Iowan Old Style", Georgia, "Times New Roman", serif;
  font-weight: 600;
  letter-spacing: -0.02em;
}
.hero {
  display: flex;
  align-items: center;
  gap: 0.9rem;
  margin-bottom: 1.1rem;
}
.hero h1 { margin: 0; font-size: 1.9rem; line-height: 1.05; }
.lede { margin: 0.2rem 0 0; color: var(--muted); font-size: 0.95rem; }
.mark { width: 3.25rem; height: 3.25rem; flex: none; display: block; }
.card {
  background: var(--card);
  border: 1px solid var(--line);
  border-radius: 1.25rem;
  box-shadow: var(--shadow);
  padding: 1.15rem 1.15rem 1.05rem;
}
.card + .card { margin-top: 0.9rem; }
.status-card.is-idle {
  background: linear-gradient(180deg, rgba(29, 107, 69, 0.1), transparent 5.5rem), var(--card);
}
.status-card.is-feeding {
  background: linear-gradient(180deg, rgba(192, 86, 33, 0.14), transparent 5.5rem), var(--card);
}
.status-line { display: flex; align-items: center; gap: 0.75rem; }
.kicker {
  margin: 0;
  font-size: 0.72rem;
  font-weight: 700;
  letter-spacing: 0.08em;
  text-transform: uppercase;
  color: var(--muted);
}
.status-word { margin: 0; font-size: 1.85rem; line-height: 1.1; }
.status-note { margin: 0.55rem 0 0; color: var(--muted); font-size: 0.95rem; }
.lamp {
  width: 0.85rem;
  height: 0.85rem;
  border-radius: 50%;
  flex: none;
  background: var(--idle);
  box-shadow: 0 0 0 0.35rem rgba(29, 107, 69, 0.16);
}
.lamp-feeding {
  background: var(--feeding);
  box-shadow: 0 0 0 0 rgba(192, 86, 33, 0.45);
  animation: pulse 1.4s ease-out infinite;
}
@keyframes pulse {
  to { box-shadow: 0 0 0 0.7rem rgba(192, 86, 33, 0); }
}
.meter {
  height: 0.28rem;
  margin: 0.9rem 0 0;
  border-radius: 99px;
  background: rgba(192, 86, 33, 0.16);
  overflow: hidden;
}
.meter span {
  display: block;
  height: 100%;
  width: 35%;
  border-radius: inherit;
  background: var(--feeding);
  animation: sweep 1s ease-in-out infinite;
}
@keyframes sweep {
  from { transform: translateX(-130%); }
  to { transform: translateX(340%); }
}
.section-head h2 { margin: 0 0 0.25rem; font-size: 1.45rem; }
.section-head p { margin: 0 0 1rem; color: var(--muted); font-size: 0.92rem; }
.fields { display: grid; gap: 0.75rem; }
.field label {
  display: block;
  margin: 0 0 0.35rem;
  font-size: 0.82rem;
  font-weight: 600;
}
.control {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  min-height: 3rem;
  padding: 0 0.85rem;
  background: var(--input);
  border: 1px solid var(--line);
  border-radius: 0.8rem;
}
.control:focus-within {
  border-color: var(--accent);
  box-shadow: 0 0 0 3px var(--ring);
}
.control input {
  flex: 1;
  min-width: 0;
  border: 0;
  margin: 0;
  padding: 0.7rem 0;
  background: transparent;
  color: inherit;
  font: 600 1rem/1.2 ui-monospace, SFMono-Regular, Menlo, Consolas, "Liberation Mono", monospace;
}
.control input:focus { outline: none; }
.control span { color: var(--muted); font-size: 0.8rem; font-weight: 600; }
.actions { display: flex; flex-wrap: wrap; gap: 0.6rem; margin-top: 1rem; }
.btn {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 100%;
  min-height: 3.25rem;
  margin-top: 0.95rem;
  padding: 0.75rem 1rem;
  border: 1px solid transparent;
  border-radius: 0.9rem;
  background: transparent;
  color: inherit;
  font: 600 1rem/1.2 ui-sans-serif, system-ui, -apple-system, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  text-align: center;
  text-decoration: none;
  cursor: pointer;
  -webkit-tap-highlight-color: transparent;
}
.btn:active { transform: translateY(1px); }
.btn-feed { background: var(--accent); color: #fff; }
.btn-feed:hover { background: var(--accent-dark); }
.btn-cancel { background: var(--cancel); color: #fff; }
.btn-cancel:hover { background: var(--cancel-dark); }
.actions .btn { width: auto; flex: 1 1 11rem; margin-top: 0; }
.btn-save { background: var(--save); color: var(--save-ink); }
.btn-save:hover { filter: brightness(1.12); }
.btn-ghost { background: transparent; color: var(--ink); border-color: var(--line-strong); }
.btn-ghost:hover { background: rgba(127, 106, 80, 0.08); }
.btn:focus-visible {
  outline: 2px solid var(--accent);
  outline-offset: 2px;
}
.notice h1 { margin: 0 0 0.4rem; font-size: 2rem; }
.notice p { margin: 0; color: var(--muted); }
@media (min-width: 720px) {
  .page { padding-top: 4rem; }
}
@media (prefers-reduced-motion: reduce) {
  .lamp-feeding, .meter span, .btn:active { animation: none; transform: none; }
}
)FEEDERCSS";

static const char FEEDER_HERO[] PROGMEM = R"FEEDERHTML(
<header class="hero">
  <svg class="mark" viewBox="0 0 52 52" aria-hidden="true">
    <rect width="52" height="52" rx="16" fill="#e7f4ec"/>
    <path fill="#1d6b45" d="M14.5 27.2c.9 7.4 5.5 12.3 11.5 12.3s10.6-4.9 11.5-12.3h-23z"/>
    <ellipse cx="26" cy="27" rx="12.3" ry="3.7" fill="#145436"/>
    <ellipse cx="26" cy="26" rx="7" ry="1.6" fill="#d9f0e2"/>
  </svg>
  <div>
    <h1>Pet Feeder</h1>
    <p class="lede">Dispense a serving or tune the auger cycle.</p>
  </div>
</header>
)FEEDERHTML";

static const char FEEDER_STATUS_IDLE[] PROGMEM = R"FEEDERHTML(
<section class="card status-card is-idle" aria-live="polite">
  <div class="status-line">
    <span class="lamp" aria-hidden="true"></span>
    <div>
      <p class="kicker">Status</p>
      <p class="status-word">Idle</p>
    </div>
  </div>
  <p class="status-note">Ready for the next serving.</p>
  <a class="btn btn-feed" href="feed">Feed Now</a>
</section>
)FEEDERHTML";

static const char FEEDER_STATUS_FEEDING[] PROGMEM = R"FEEDERHTML(
<section class="card status-card is-feeding" aria-live="polite">
  <div class="status-line">
    <span class="lamp lamp-feeding" aria-hidden="true"></span>
    <div>
      <p class="kicker">Status</p>
      <p class="status-word">Feeding</p>
    </div>
  </div>
  <p class="status-note">The auger is running. This page refreshes until the cycle finishes. Leaving the page does not stop it.</p>
  <div class="meter" aria-hidden="true"><span></span></div>
  <a class="btn btn-cancel" href="cancel">Cancel Feeding</a>
</section>
)FEEDERHTML";

static const char FEEDER_FORM_HEAD[] PROGMEM = R"FEEDERHTML(
<section class="card">
  <div class="section-head">
    <h2>Parameters</h2>
    <p>
)FEEDERHTML";

static const char FEEDER_FORM_OPEN[] PROGMEM = R"FEEDERHTML(
    </p>
  </div>
  <form action="updateparams" method="post" autocomplete="off">
    <div class="fields">
)FEEDERHTML";

static const char FEEDER_FORM_CLOSE[] PROGMEM = R"FEEDERHTML(
    </div>
    <div class="actions">
      <input class="btn btn-save" type="submit" value="Update">
      <button class="btn btn-ghost" type="button" onclick="loadDefaults();">Reset to Defaults</button>
    </div>
  </form>
</section>
</main>
)FEEDERHTML";

static const char TITLE_MAIN[] PROGMEM = "Pet Feeder";
static const char TITLE_FAIL[] PROGMEM = "Update failed";
static const char TITLE_BAD[] PROGMEM = "Invalid parameters";
static const char TITLE_END[] PROGMEM = "</title>";
static const char STYLE_OPEN[] PROGMEM = "<style>";
static const char BODY_OPEN[] PROGMEM = "</style></head><body><main class=\"page\">";
static const char REFRESH_META[] PROGMEM = "<meta http-equiv=\"refresh\" content=\"1\">";
static const char SAVE_FAIL_MSG[] PROGMEM = "The new parameters could not be saved.";
static const char NOTICE_OPEN[] PROGMEM = "<section class=\"card notice\"><h1>";
static const char NOTICE_MID[] PROGMEM = "</h1><p>";
static const char NOTICE_END[] PROGMEM =
    "</p><a class=\"btn btn-feed\" href=\"/\">Back to Pet Feeder</a></section></main></body></html>";

static const char FIELD_FORWARD_PRE[] PROGMEM =
    "<div class=\"field\"><label for=\"forward\">Forward time</label><div class=\"control\"><input type=\"text\" id=\"forward\" name=\"forward\" inputmode=\"numeric\" spellcheck=\"false\" value=\"";
static const char FIELD_BACK_PRE[] PROGMEM =
    "<div class=\"field\"><label for=\"back\">Backward time</label><div class=\"control\"><input type=\"text\" id=\"back\" name=\"back\" inputmode=\"numeric\" spellcheck=\"false\" value=\"";
static const char FIELD_PAUSE_PRE[] PROGMEM =
    "<div class=\"field\"><label for=\"pause\">Pause time</label><div class=\"control\"><input type=\"text\" id=\"pause\" name=\"pause\" inputmode=\"numeric\" spellcheck=\"false\" value=\"";
static const char FIELD_REST_PRE[] PROGMEM =
    "<div class=\"field\"><label for=\"rest\">Rest time</label><div class=\"control\"><input type=\"text\" id=\"rest\" name=\"rest\" inputmode=\"numeric\" spellcheck=\"false\" value=\"";
static const char FIELD_ITER_PRE[] PROGMEM =
    "<div class=\"field\"><label for=\"iterations\">Number of iterations</label><div class=\"control\"><input type=\"text\" id=\"iterations\" name=\"iterations\" inputmode=\"numeric\" spellcheck=\"false\" value=\"";
static const char FIELD_MS_END[] PROGMEM = "\"><span>ms</span></div></div>";
static const char FIELD_NUM_END[] PROGMEM = "\"></div></div>";

static const char SCRIPT_FWD[] PROGMEM =
    "<script>function loadDefaults() {document.getElementById(\"forward\").value = \"";
static const char SCRIPT_BACK[] PROGMEM =
    "\";document.getElementById(\"back\").value = \"";
static const char SCRIPT_PAUSE[] PROGMEM =
    "\";document.getElementById(\"pause\").value = \"";
static const char SCRIPT_REST[] PROGMEM =
    "\";document.getElementById(\"rest\").value = \"";
static const char SCRIPT_ITER[] PROGMEM =
    "\";document.getElementById(\"iterations\").value = \"";
static const char SCRIPT_END[] PROGMEM = "\";}</script></body></html>";

struct Segment {
  const char *data;
  uint16_t length;
  bool flash;
};

struct PageBody {
  enum { MAX_SEGMENTS = 48, MAX_NUMBERS = 10 };
  Segment segs[MAX_SEGMENTS];
  uint8_t count;
  uint8_t numberSlot;
  char numbers[MAX_NUMBERS][12];
  char blurb[240];
  char note[160];
  PageBody() : count(0), numberSlot(0) {
    blurb[0] = 0;
    note[0] = 0;
  }
};

static void addFlash(PageBody &page, const char *progmem) {
  if (progmem == nullptr || page.count >= PageBody::MAX_SEGMENTS) {
    return;
  }
  size_t n = strlen_P(progmem);
  page.segs[page.count].data = progmem;
  page.segs[page.count].length = (uint16_t)n;
  page.segs[page.count].flash = true;
  page.count++;
}

static void addRam(PageBody &page, const char *ram) {
  if (ram == nullptr || page.count >= PageBody::MAX_SEGMENTS) {
    return;
  }
  size_t n = strlen(ram);
  page.segs[page.count].data = ram;
  page.segs[page.count].length = (uint16_t)n;
  page.segs[page.count].flash = false;
  page.count++;
}

static void appendNumber(PageBody &page, int value) {
  if (page.numberSlot >= PageBody::MAX_NUMBERS) {
    return;
  }
  char *slot = page.numbers[page.numberSlot++];
  snprintf(slot, 12, "%d", value);
  addRam(page, slot);
}

static void addField(PageBody &page, const char *pre, int value, bool milliseconds) {
  addFlash(page, pre);
  appendNumber(page, value);
  addFlash(page, milliseconds ? FIELD_MS_END : FIELD_NUM_END);
}

// Copies the page straight from flash in small chunks. Returning 0 means
// there are no further bytes (end of a chunked response).
static size_t fillPage(const PageBody *page, uint8_t *data, size_t len, size_t index) {
  if (page == nullptr || data == nullptr || len == 0) {
    return 0;
  }
  size_t skip = index;
  size_t written = 0;
  for (uint8_t i = 0; i < page->count; i++) {
    uint16_t seglen = page->segs[i].length;
    if (seglen == 0) {
      continue;
    }
    if (skip >= seglen) {
      skip -= seglen;
      continue;
    }
    size_t n = (size_t)(seglen - skip);
    if (n > len - written) {
      n = len - written;
    }
    if (page->segs[i].flash) {
      memcpy_P(data + written, page->segs[i].data + skip, n);
    } else {
      memcpy(data + written, page->segs[i].data + skip, n);
    }
    written += n;
    skip = 0;
    if (written == len) {
      return written;
    }
  }
  return written;
}

static void addDocumentHead(PageBody &page, const char *titleFlash, bool refresh) {
  addFlash(page, FEEDER_HEAD_OPEN);
  addFlash(page, titleFlash);
  addFlash(page, TITLE_END);
  if (refresh) {
    addFlash(page, REFRESH_META);
  }
  addFlash(page, STYLE_OPEN);
  addFlash(page, FEEDER_CSS);
  addFlash(page, BODY_OPEN);
}

static void addLoadDefaults(PageBody &page) {
  addFlash(page, SCRIPT_FWD);
  appendNumber(page, FORWARD);
  addFlash(page, SCRIPT_BACK);
  appendNumber(page, BACK);
  addFlash(page, SCRIPT_PAUSE);
  appendNumber(page, PAUSE);
  addFlash(page, SCRIPT_REST);
  appendNumber(page, REST);
  addFlash(page, SCRIPT_ITER);
  appendNumber(page, ITERATIONS);
  addFlash(page, SCRIPT_END);
}

static void buildMainPage(PageBody &page, bool feeding, const feedParameters &params) {
  addDocumentHead(page, TITLE_MAIN, feeding);
  addFlash(page, FEEDER_HERO);
  addFlash(page, feeding ? FEEDER_STATUS_FEEDING : FEEDER_STATUS_IDLE);
  snprintf(page.blurb, sizeof(page.blurb),
      "Forward, pause, reverse, then rest, including a rest after the last iteration. "
      "Times are milliseconds from %d to %d. Iterations are from %d to %d. "
      "Reset to Defaults fills the form; Update saves it for the next cycle.",
      MIN_PHASE_MS, MAX_PHASE_MS, MIN_ITERATIONS, MAX_ITERATIONS);
  addFlash(page, FEEDER_FORM_HEAD);
  addRam(page, page.blurb);
  addFlash(page, FEEDER_FORM_OPEN);
  addField(page, FIELD_FORWARD_PRE, params.pForward, true);
  addField(page, FIELD_BACK_PRE, params.pBack, true);
  addField(page, FIELD_PAUSE_PRE, params.pPause, true);
  addField(page, FIELD_REST_PRE, params.pRest, true);
  addField(page, FIELD_ITER_PRE, params.pIterations, false);
  addFlash(page, FEEDER_FORM_CLOSE);
  addLoadDefaults(page);
}

static void buildNoticePage(PageBody &page, const char *titleFlash, const char *detailFlash, const char *detailRam) {
  addDocumentHead(page, titleFlash, false);
  addFlash(page, NOTICE_OPEN);
  addFlash(page, titleFlash);
  addFlash(page, NOTICE_MID);
  if (detailFlash != nullptr) {
    addFlash(page, detailFlash);
  } else if (detailRam != nullptr) {
    addRam(page, detailRam);
  }
  addFlash(page, NOTICE_END);
}

static void sendPage(AsyncWebServerRequest *request, int code, const std::shared_ptr<PageBody> &page) {
  if (request == nullptr || !page) {
    return;
  }
  AsyncWebServerResponse *response = request->beginChunkedResponse(
      "text/html",
      [page](uint8_t *data, size_t len, size_t index) -> size_t {
        return fillPage(page.get(), data, len, index);
      });
  if (response == nullptr) {
    request->send(500, "text/plain", "Out of memory");
    return;
  }
  response->setCode(code);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

static std::shared_ptr<PageBody> newPage() {
  return std::shared_ptr<PageBody>(new (std::nothrow) PageBody());
}

static bool parseWholeNumber(const String &raw, long &parsed) {
  const char *s = raw.c_str();
  while (*s == ' ' || *s == '\t') {
    s++;
  }
  const char *end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
    end--;
  }
  size_t n = (size_t)(end - s);
  if (n == 0 || n > 9) {
    return false;
  }
  long value = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
    value = value * 10 + (s[i] - '0');
  }
  parsed = value;
  return true;
}

static bool applyBounded(AsyncWebServerRequest *request, const char *name, int minValue, int maxValue, int &dest, bool &changed) {
  if (!request->hasParam(name, true)) {
    return true;
  }
  const AsyncWebParameter *param = request->getParam(name, true);
  if (param == nullptr) {
    return true;
  }
  long parsed = 0;
  if (!parseWholeNumber(param->value(), parsed) || parsed < minValue || parsed > maxValue) {
    return false;
  }
  int asInt = (int)parsed;
  if (asInt != dest) {
    dest = asInt;
    changed = true;
  }
  return true;
}

static void sendInvalid(AsyncWebServerRequest *request) {
  std::shared_ptr<PageBody> page = newPage();
  if (!page) {
    request->send(500, "text/plain", "Out of memory");
    return;
  }
  snprintf(page->note, sizeof(page->note),
      "Times must be from %d to %d ms. Iterations must be from %d to %d.",
      MIN_PHASE_MS, MAX_PHASE_MS, MIN_ITERATIONS, MAX_ITERATIONS);
  buildNoticePage(*page, TITLE_BAD, nullptr, page->note);
  sendPage(request, 400, page);
}

static void sendSaveFailed(AsyncWebServerRequest *request) {
  std::shared_ptr<PageBody> page = newPage();
  if (!page) {
    request->send(500, "text/plain", "Out of memory");
    return;
  }
  buildNoticePage(*page, TITLE_FAIL, SAVE_FAIL_MSG, nullptr);
  sendPage(request, 500, page);
}

//Web handlers
/*
    Serve the main web page
    Feeder::getMainPage()
    Parameters:
        request: a pointer to an AsyncWebServerRequest object
    Returns:
        void
    This is the handler for page loads to the home page.
    It also checks if a feeding cycle is active or not. If a
    feeding is active, the button cancels it and the page refreshes
    once a second. If a feeding is not active, the button starts one.
    It also loads the current parameters into the form. loadDefaults()
    restores the compiled defaults in the form without saving them.
*/
void Feeder::getMainPage(AsyncWebServerRequest *request) {
  if (request == nullptr) {
    return;
  }
  uint8_t cmd = __sync_fetch_and_add(&pendingCommand, 0);
  uint8_t st = __sync_fetch_and_add(&publishedState, 0);
  bool feeding = false;
  if (cmd == CMD_START) {
    feeding = true;
  } else if (cmd == CMD_CANCEL) {
    feeding = false;
  } else {
    feeding = (st != (uint8_t)idle);
  }
  feedParameters params;
  FEEDER_LOCK();
  params = feedParams;
  FEEDER_UNLOCK();
  std::shared_ptr<PageBody> page = newPage();
  if (!page) {
    request->send(500, "text/plain", "Out of memory");
    return;
  }
  buildMainPage(*page, feeding, params);
  sendPage(request, 200, page);
}

/*
    Start a feeding through a GET request
    Feeder::getFeedPage()
    Parameters:
        request: a pointer to the AsyncWebServerRequest object
    Returns:
        void
    Queues a feed. checkFeeding() starts the auger on the loop task.
    Redirects back to the home page.
*/
void Feeder::getFeedPage(AsyncWebServerRequest *request) {
  if (request == nullptr) {
    return;
  }
  Serial.println("Feeder: Feeding initiated");
  issueCommand(CMD_START);
  request->redirect("/");
}

/*
    Cancel a feeding through a GET request
    Feeder::getCancelPage()
    Parameters:
        request: a pointer to the AsyncWebServerRequest object
    Returns:
        void
    Queues a cancel. checkFeeding() stops the auger on the loop task.
    Redirects back to the home page.
*/
void Feeder::getCancelPage(AsyncWebServerRequest *request) {
  if (request == nullptr) {
    return;
  }
  Serial.println("Feeder: Feeding Cancelled");
  issueCommand(CMD_CANCEL);
  request->redirect("/");
}

/*
    Update the feeding parameters to EEPROM
    Feeder::postUpdateParameters
    Parameters:
        request: a pointer to the AsyncWebServerRequest object
    Returns:
        void
    This method gets the new feed parameters from the POST web
    request, updates the feedParams, and writes them to EEPROM
    It first checks if anything actually changed
*/
void Feeder::postUpdateParamsPage(AsyncWebServerRequest *request) {
  if (request == nullptr) {
    return;
  }
  Serial.println("Feeder: Updating parameters");
  feedParameters next;
  FEEDER_LOCK();
  next = feedParams;
  FEEDER_UNLOCK();
  bool changed = false;
  bool ok = applyBounded(request, "forward", MIN_PHASE_MS, MAX_PHASE_MS, next.pForward, changed)
      && applyBounded(request, "back", MIN_PHASE_MS, MAX_PHASE_MS, next.pBack, changed)
      && applyBounded(request, "pause", MIN_PHASE_MS, MAX_PHASE_MS, next.pPause, changed)
      && applyBounded(request, "rest", MIN_PHASE_MS, MAX_PHASE_MS, next.pRest, changed)
      && applyBounded(request, "iterations", MIN_ITERATIONS, MAX_ITERATIONS, next.pIterations, changed);
  if (!ok) {
    Serial.println("Feeder: Rejected parameter update");
    sendInvalid(request);
    return;
  }
  if (!changed) {
    Serial.println("Feeder: No parameters changed");
    request->redirect("/");
    return;
  }
  next.check = paramsCheck(next);
  // Commit before publishing so a failed write leaves RAM unchanged.
  if (!commitParams(next)) {
    Serial.println("Feeder: EEPROM commit failed");
    sendSaveFailed(request);
    return;
  }
  FEEDER_LOCK();
  feedParams = next;
  FEEDER_UNLOCK();
  Serial.println("Feeder: Parameter update success");
  request->redirect("/");
}
