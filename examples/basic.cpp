#include <Arduino.h>
#include <ESP8266Wifi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncElegantOTA.h>
#include "feeder.h"

//Wifi parameters
const char* ssid = "SSID";
const char* password = "PASSWORD";

AsyncWebServer server(80);  // create the web server
Feeder pigFeeder;  // create the feeder

void setup() {
  // Set to 74880 to match the ESP8266 boot baud rate
  Serial.begin(74880);
  while (! Serial) {
    delay(1);
  }
  Serial.println("Init!");

  //Wifi config
  Serial.print("Connecting to Wifi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected, IP address: ");
  Serial.println(WiFi.localIP());  

  //Set up for OTA updates web interface
  AsyncElegantOTA.begin(&server);

  //Initialize the feeder and feeder web interface
  pigFeeder.begin(&server);

  server.begin();
}

void loop() {
  //Check for async timer events to move the feeding state machine
  pigFeeder.checkFeeding();
}

