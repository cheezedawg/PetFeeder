#ifndef FEEDER_H
#define FEEDER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ESP_EEPROM.h>
#include <Servo.h>
#include <NoDelay.h>
#include <ESP_EEPROM.h>

//Servo parameters
#define SERVO_GPIO 4
#define SERVO_STOP 90
#define SERVO_FORWARD 0
#define SERVO_BACK 180

//Default feed timing parameters
#define FORWARD 8500
#define PAUSE 500
#define BACK 500
#define REST 500
#define ITERATIONS 5

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

        feedParameters feedParams;

        void startFeeding();
        void cancelFeeding();
        void initializeTimers();
        int paramsCheck(feedParameters params);

        //Web handlers
        void getMainPage(AsyncWebServerRequest *request);
        void getFeedPage(AsyncWebServerRequest *request);
        void getCancelPage(AsyncWebServerRequest *request);
        void postUpdateParamsPage(AsyncWebServerRequest *request);
        void notFound(AsyncWebServerRequest *request);
        
};

#endif