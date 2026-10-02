#ifndef FEEDER_H
#define FEEDER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ESP_EEPROM.h>
#include <Servo.h>
#include <NoDelay.h>
#include <ESP_EEPROM.h>

// Servo signal pin. This macro is the pin the firmware drives (GPIO4).
#define SERVO_GPIO 4
#define SERVO_STOP 90
#define SERVO_FORWARD 0
#define SERVO_BACK 180

// Default feed timing parameters
#define FORWARD 8500
#define PAUSE 500
#define BACK 500
#define REST 500
#define ITERATIONS 5

// Values accepted from the web form and from EEPROM.
#define MIN_PHASE_MS 1
#define MAX_PHASE_MS 120000
#define MIN_ITERATIONS 1
#define MAX_ITERATIONS 30

struct feedParameters {
  int pForward;
  int pBack;
  int pPause;
  int pRest;
  int pIterations;
  int check;
};

// This tracks the state of the feeder state machine
typedef enum {
  forward,
  forwardPause,
  back,
  rest,
  idle,
} feedState;

class Feeder {
    public:
        Feeder();
        ~Feeder();
        void begin(AsyncWebServer *server);
        void checkFeeding();
    private:
        Servo auger;  // create servo object to control a servo
        // Create some nonblocking delays
        noDelay forwardTime;
        noDelay pauseTime;
        noDelay backTime;
        noDelay restTime;

        AsyncWebServer * webServer;

        feedState state;
        int iteration;
        // Iteration count captured when the current cycle starts.
        int cycleIterations;

        feedParameters feedParams;

        // Written by web handlers, applied at the start of checkFeeding().
        volatile uint8_t pendingCommand;
        // Latest state published for the web page. Not idle while a cycle runs.
        volatile uint8_t publishedState;
        bool hardwareReady;
        bool routesReady;

        void startHardware();
        void loadOrInitParams();
        bool commitParams(const feedParameters &params);
        bool paramsAcceptable(const feedParameters &params) const;
        void applyDelays(const feedParameters &params);
        void startFeeding();
        void cancelFeeding();
        void publishState();
        void issueCommand(uint8_t cmd);
        int paramsCheck(feedParameters params) const;

        //Web handlers
        void getMainPage(AsyncWebServerRequest *request);
        void getFeedPage(AsyncWebServerRequest *request);
        void getCancelPage(AsyncWebServerRequest *request);
        void postUpdateParamsPage(AsyncWebServerRequest *request);
        
};

#endif