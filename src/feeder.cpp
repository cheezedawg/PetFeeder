/*
    Feeder class implementation
*/
#include "feeder.h"

/*
    Default constructor
    Feeder::Feeder()
    Configure the servo, initialize EEPROM and timers
*/
Feeder::Feeder() {
  // Initialize the Servo
  auger.attach(SERVO_GPIO);  // attaches the servo on GPIO2 to the servo object
  auger.write(SERVO_STOP);

  // Initialize the feed parameters and EEPROM
  EEPROM.begin(sizeof(feedParameters));
  // First check if we have valid parameters in EEPROM
  EEPROM.get(0,feedParams);
  if ((feedParams.check == paramsCheck(feedParams)) && (feedParams.check > 0)) {
    Serial.println("Feeder: Valid parameters found in EEPROM");
  } else {
    Serial.println("Feeder: No EEPROM parameters found. Updating...");
    EEPROM.wipe();
    feedParams.pForward = FORWARD;
    feedParams.pPause = PAUSE;
    feedParams.pBack = BACK;
    feedParams.pPause = PAUSE;
    feedParams.pRest = REST;
    feedParams.pIterations = ITERATIONS;
    feedParams.check = paramsCheck(feedParams);
    EEPROM.put(0,feedParams);
    Serial.println(EEPROM.commit() ? "Feeder: EEPROM commit done" : "Feeder: EEPROM commmit failed");
  }
  // Initialize the timers
  initializeTimers();

  state = idle;
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
    Set up the web server handlers for 
        / 
        /feed
        /cancel
        /updateparams
        /404 error
*/
void Feeder::begin(AsyncWebServer *server) {
  webServer= server;
  //Web server config
  //Main page
  webServer->on("/", HTTP_GET, [&](AsyncWebServerRequest *request) {
    getMainPage(request);
  });
  
  //Feed
  webServer->on("/feed", HTTP_GET, [&](AsyncWebServerRequest *request) {
    getFeedPage(request);
  });

  //Cancel
  webServer->on("/cancel", HTTP_GET, [&](AsyncWebServerRequest *request) {
    getCancelPage(request);
  });
  
  //Update params
  webServer->on("/updateparams", HTTP_POST, [&](AsyncWebServerRequest *request) {
    postUpdateParamsPage(request);
  });
  
  //404 error
  webServer->onNotFound([&](AsyncWebServerRequest *request) {
    notFound(request);
  });

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
        if (iteration < feedParams.pIterations) {
          state = rest;
          restTime.start();
        } else {
          state = idle;
          auger.write(SERVO_STOP);
        }
      }
      break;
    case rest:
      if (restTime.update()) {
        Serial.println("Feeder: Rest Done");
        auger.write(SERVO_FORWARD);
        state = forward;
        forwardTime.start();
      }
      break;
    default:
      break;
  }
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
    Initialize the timers
    Feeder::intitializeTimers()
    Parameters:
        None
    Returns:
        void
    This function updates the timer objects with the current
    configured timer parameters in the feedParams member 
    variable
*/
void Feeder::initializeTimers() {
  forwardTime.setdelay(feedParams.pForward);
  pauseTime.setdelay(feedParams.pPause);
  backTime.setdelay(feedParams.pBack);
  restTime.setdelay(feedParams.pRest);
}

/*
    Calculate the check value for the parameters
    Feeder::paramsCheck()
    Parameters:
        params: A feedParameters object with parameters
    returns:
        integer sum of the timer values
    This function is used to check whether we have valid parameters
    in EEPROM or not. The check value is the sum of the timer values
*/
int Feeder::paramsCheck(feedParameters params) {
  return params.pForward + params.pBack + params.pPause + params.pRest + params.pIterations;
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

static const char FEEDER_FORM_OPEN[] PROGMEM = R"FEEDERHTML(
<section class="card">
  <div class="section-head">
    <h2>Parameters</h2>
    <p>Forward, pause, reverse, then rest. Times are milliseconds. Reset to Defaults fills the form; Update saves it on the device.</p>
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

static void printDocumentHead(Print &out, const char *title, bool refresh) {
  out.print(FPSTR(FEEDER_HEAD_OPEN));
  out.print(title);
  out.print("</title>");
  if (refresh) {
    out.print("<meta http-equiv=\"refresh\" content=\"1\">");
  }
  out.print("<style>");
  out.print(FPSTR(FEEDER_CSS));
  out.print("</style></head><body><main class=\"page\">");
}

static void printField(Print &out, const char *id, const char *label, int value, bool milliseconds) {
  out.print("<div class=\"field\"><label for=\"");
  out.print(id);
  out.print("\">");
  out.print(label);
  out.print("</label><div class=\"control\"><input type=\"text\" id=\"");
  out.print(id);
  out.print("\" name=\"");
  out.print(id);
  out.print("\" inputmode=\"numeric\" spellcheck=\"false\" value=\"");
  out.print(value);
  out.print("\">");
  if (milliseconds) {
    out.print("<span>ms</span>");
  }
  out.print("</div></div>");
}

static void printLoadDefaults(Print &out) {
  out.print("<script>function loadDefaults() {");
  out.print("document.getElementById(\"forward\").value = \"");
  out.print(FORWARD);
  out.print("\";");
  out.print("document.getElementById(\"back\").value = \"");
  out.print(BACK);
  out.print("\";");
  out.print("document.getElementById(\"pause\").value = \"");
  out.print(PAUSE);
  out.print("\";");
  out.print("document.getElementById(\"rest\").value = \"");
  out.print(REST);
  out.print("\";");
  out.print("document.getElementById(\"iterations\").value = \"");
  out.print(ITERATIONS);
  out.print("\";}</script>");
}

static void sendNoticePage(AsyncWebServerRequest *request, int code, const char *heading, const char *detail) {
  AsyncResponseStream *response = request->beginResponseStream("text/html");
  response->setCode(code);
  response->addHeader("Cache-Control", "no-store");
  printDocumentHead(*response, heading, false);
  response->print("<section class=\"card notice\"><h1>");
  response->print(heading);
  response->print("</h1><p>");
  response->print(detail);
  response->print("</p><a class=\"btn btn-feed\" href=\"/\">Back to Pet Feeder</a></section></main></body></html>");
  request->send(response);
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
  AsyncResponseStream *response = request->beginResponseStream("text/html");
  response->addHeader("Cache-Control", "no-store");
  const bool feeding = (state != idle);
  printDocumentHead(*response, "Pet Feeder", feeding);
  response->print(FPSTR(FEEDER_HERO));
  if (feeding) {
    response->print(FPSTR(FEEDER_STATUS_FEEDING));
  } else {
    response->print(FPSTR(FEEDER_STATUS_IDLE));
  }
  response->print(FPSTR(FEEDER_FORM_OPEN));
  printField(*response, "forward", "Forward time", feedParams.pForward, true);
  printField(*response, "back", "Backward time", feedParams.pBack, true);
  printField(*response, "pause", "Pause time", feedParams.pPause, true);
  printField(*response, "rest", "Rest time", feedParams.pRest, true);
  printField(*response, "iterations", "Number of iterations", feedParams.pIterations, false);
  response->print(FPSTR(FEEDER_FORM_CLOSE));
  printLoadDefaults(*response);
  response->print("</body></html>");
  request->send(response);
}

/*
    Start a feeding through a GET request
    Feeder::getFeedPage()
    Parameters:
        request: a pointer to the AsyncWebServerRequest object
    Returns:
        void
    This calls the startFeeding method and redirects the request back to the 
    home page.
*/
void Feeder::getFeedPage(AsyncWebServerRequest *request) {
  // Start a feeding and redirect to the home page
  Serial.println("Feeder: Feeding initiated");
  startFeeding();
  request->redirect("/");
}

/*
    Cancel a feeding through a GET request
    Feeder::getCancelPage()
    Parameters:
        request: a pointer to the AsyncWebServerRequest object
    Returns:
        void
    This calls the cancelFeeding method and redirects the request back to the 
    home page.
*/
void Feeder::getCancelPage(AsyncWebServerRequest *request) {
  // Start a feeding and redirect to the home page
  Serial.println("Feeder: Feeding Cancelled");
  cancelFeeding();
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
  Serial.println("Feeder: Updating parameters");
  int buf = 0;
  bool changed = false;
  //Forward
  if (request->hasParam("forward", true)) {
    buf = request->getParam("forward", true)->value().toInt();
    if (buf != feedParams.pForward) {
        changed = true;
        feedParams.pForward = buf;
    }   
  }
  //Back
  if (request->hasParam("back", true)) {
    buf = request->getParam("back", true)->value().toInt();
    if (buf != feedParams.pBack) {
        changed = true;
        feedParams.pBack = buf;
    }   
  }
  //Pause
  if (request->hasParam("pause", true)) {
    buf = request->getParam("pause", true)->value().toInt();
    if (buf != feedParams.pPause) {
        changed = true;
        feedParams.pPause = buf;
    }   
  }
  //Rest
  if (request->hasParam("rest", true)) {
      buf = request->getParam("rest", true)->value().toInt();
    if (buf != feedParams.pRest) {
        changed = true;
        feedParams.pRest = buf;
    }     
  }
  if (request->hasParam("iterations", true)) {
    buf = request->getParam("iterations", true)->value().toInt();
    if (buf != feedParams.pIterations) {
        changed = true;
        feedParams.pIterations = buf;
    }   
  }
  if (changed) {
    feedParams.check = paramsCheck(feedParams);
    EEPROM.put(0,feedParams);

    if (EEPROM.commit()) {
      Serial.println("Feeder: Paramter update success");
      initializeTimers();
      request->redirect("/");
    } else
      sendNoticePage(request, 200, "Update failed", "The new parameters could not be saved.");
  } else {
    Serial.println("Feeder: No parameters changed");
    request->redirect("/");
  }
}

/*
    404 page
    Feeder::notFound()
    Parameters:
        request: a pointer to the AsyncWebServerRequest object
    Returns:
        void
    404 page
*/
void Feeder::notFound(AsyncWebServerRequest *request) {
  sendNoticePage(request, 404, "Not found", "That address is not part of the feeder.");
}
