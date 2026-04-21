#pragma once

/*
Purpose: Runtime serial configuration menu for the FlightWall.
Provides a simple text UI over the Serial connection that lets the user
update WiFi credentials and the tracked tail number without recompiling.

Usage:
  - Call SerialConfig::begin() once in setup(), after Serial.begin().
  - Call SerialConfig::tick() each iteration of loop().
  - Use SerialConfig::wifiSSID, wifiPassword, tailNumber in place of the
    hardcoded Secrets.h defines everywhere credentials are consumed.

Persistence:
  - ESP32:         values are stored in NVS flash via Preferences and survive
                   power cycles.
  - SAMD (M4):     values are stored in internal flash via FlashStorage_SAMD
                   (lib: khoih-prog/FlashStorage_SAMD) and survive power cycles.
  In both cases stored values override the Secrets.h compile-time defaults.

WiFi credential workflow:
  1. Open the menu (serial 'm' or hardware DOWN x3).
  2. Set the new SSID and/or password.
  3. Press 'r' — this saves all values to flash and reboots immediately.
     The device reconnects using the new credentials on startup.
  Tail number changes take effect on the next fetch cycle; no restart needed.

Activation:
  Open the serial monitor at 115200 baud and type 'm' followed by Enter,
  or press the DOWN button three times within 700 ms.
*/

#include <Arduino.h>

#if defined(ARDUINO_ARCH_ESP32)
  #include <Preferences.h>
#endif

namespace SerialConfig
{
    // Active runtime values — loaded from NVS/defaults in begin().
    // Read these wherever WiFi credentials and the tail number are needed.
    extern String wifiSSID;
    extern String wifiPassword;
    extern String tailNumber;

    // Call once in setup() after Serial.begin().
    void begin();

    // Call each iteration of loop() to process serial input.
    void tick();

    // Force the menu open immediately (used when entering serial-config mode
    // via the hardware button triple-press).
    void openMenu();

    // Returns true while the menu is displayed (i.e. not in IDLE state).
    // main.cpp watches this to automatically exit serial-config mode once the
    // user closes the menu with 'x'.
    bool isMenuOpen();

} // namespace SerialConfig
