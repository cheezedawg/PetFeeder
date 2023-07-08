# PetFeeder

This project creates a PetFeeder class to control a servo-driven auger on a pet feeder.  It makes use of the ESP AsyncWebServer and NoDelay libraries for non-blocking calls, and the configurable feed parameters are stored in EEPROM using the ESP_EEPROM library. The web interface is ugly and utilitarian because I control this with home automation and I'm not good at web design.

[ESP AsyncWebServer](https://github.com/esphome/ESPAsyncWebServer) @me-no-dev

[NoDelay](https://github.com/M-tech-Creations/NoDelay) @M-tech-Creations

[ESP_EEPROM](https://github.com/jwrw/ESP_EEPROM) @jwrw

## Usage
Instantiate a Feeder object. In `setup()`, call `Feeder.begin()` with a pointer to an `AsyncWebServer` object. Then in `loop()`, call `feeder.checkFeeding()`.  The servo should be on GPIO2 (this is currently not configurable). 
```
#include <ESPAsyncWebServer.h>
#include <feeder.h>

AsyncWebServer server(80);
Feeder feeder;

void setup() {
  feeder.begin(&server);
  server.begin();
}

void loop() {
  feeder.checkFeeding();
}
```
## Function

The landing page allows you to initiate or cancel a feeding cycle as well as update the feeding cycle parameters. A feeding cycle is executed as follows:
1. Servo turns "forward" for the configured amount of forward time
2. Servo pauses for the configured pause time
3. Servo goes backwards for the configured amount of backwards time (this is to clear any jams)
4. Servo pauses for the configured amount of rest time
5. Repeat steps 1-4 for the configured number of iterations

You can initiate a feeding cycle by accessing `http://ip_address/feed`, or cancel a feeding cycle by accessing `http://ip_address/cancel`.

## Needed Supplies
You will need a Continuous Rotation servo, such as:

https://www.sparkfun.com/products/9347

https://www.amazon.com/gp/product/B07Z3VGZNP

Also, you will need an auger, such as this 3D printed auger from [kitlaan](https://www.thingiverse.com/kitlaan/designs)

https://www.thingiverse.com/thing:27854
