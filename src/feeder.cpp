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

//Web handlers
/*
    Serve the main web page
    Feeder::getMainPAge()
    Parameters:
        request: a pointer to an AsyncWebServerRequest object
    Returns:
        void
    This is the handler for page loads to the home page.
    It also checks if a feeding cycle is active or not. If a 
    feeding is active, the button cancels it. If a feeding is 
    not active, the button starts one. It also loads the 
    current paramers into the form and has a function to 
    reset the form to the default parameters 
*/
void Feeder::getMainPage(AsyncWebServerRequest *request) {
  AsyncResponseStream * response = request->beginResponseStream("text/html");
  response->print("<!DOCTYPE html><html><head><title>Pig Feeder</title>");
  if (state != idle) {
    response->print("<meta http-equiv=\"refresh\" content=\"1\" />");
  }
  response->print("</head><body>");
  response->print("<h1>Pig Feader</h1>");
  response->print("<p>Status: ");
  if (state == idle) {
    response->print("Idle</p>");
    response->print("<button onclick=\"window.location.href = 'feed';\">Feed Now</button>");
  } else {
    response->print("Feeding</p>");
    response->print("<button onclick=\"window.location.href = 'cancel';\">Cancel Feeding</button>");
  }
  
  response->print("<button onclick=\"window.location.href = 'update';\">Firmware Update</button>");
  
  response->print("<br><br><p><h2>Parameter Update</h2><br><form action=\"updateparams\" method=\"post\">");
  response->print("<label for=\"forward\">Forward time:</label>");
  response->printf("<input type=\"text\" id=\"forward\" name=\"forward\" value=\"%d\">",feedParams.pForward);
  response->print("<label for=\"back\">Backward time:</label>");
  response->printf("<input type=\"text\" id=\"back\" name=\"back\" value=\"%d\">",feedParams.pBack);
  response->print("<label for=\"pause\">Pause time:</label>");
  response->printf("<input type=\"text\" id=\"pause\" name=\"pause\" value=\"%d\">",feedParams.pPause);
  response->print("<label for=\"rest\">Rest time:</label>");
  response->printf("<input type=\"text\" id=\"rest\" name=\"rest\" value=\"%d\">",feedParams.pRest);
  response->print("<label for=\"iterations\">Number of iterations:</label>");
  response->printf("<input type=\"text\" id=\"iterations\" name=\"iterations\" value=\"%d\">",feedParams.pIterations);
  response->print("<br><br><input type=\"submit\" value=\"Update\">");
  response->print("</form>");
  response->print("<button onclick=\"loadDefaults();\">Reset to Defaults</button>");
  response->print("<script>function loadDefaults() {");
  response->printf("document.getElementById(\"forward\").value = \"%d\";",FORWARD);
  response->printf("document.getElementById(\"back\").value = \"%d\";",BACK);
  response->printf("document.getElementById(\"pause\").value = \"%d\";",PAUSE);
  response->printf("document.getElementById(\"rest\").value = \"%d\";",REST);
  response->printf("document.getElementById(\"iterations\").value = \"%d\";}",ITERATIONS);
  response->print("</script>");
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
      request->send(200, "text/plain", "Update failed");
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
  request->send(404, "text/plain", "Not found");
}
