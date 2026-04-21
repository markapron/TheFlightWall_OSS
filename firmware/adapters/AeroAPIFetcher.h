#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "interfaces/BaseFlightFetcher.h"
#include "config/APIConfiguration.h"

// On ESP32, ArduinoHttpClient + WiFiClientSecure is used directly.
// On SAMD/AirLift, wifiClientRequest() in HttpUtils handles the TLS connection.
#if defined(ARDUINO_ARCH_ESP32)
  #if defined(FLIGHTWALL_SKIP_TLS)
    #include <WiFi.h>
    using FlightWallTlsClient = WiFiClient;
  #else
    #include <WiFi.h>
    #include <WiFiClientSecure.h>
    using FlightWallTlsClient = WiFiClientSecure;
  #endif
#endif

class AeroAPIFetcher : public BaseFlightFetcher
{
public:
    AeroAPIFetcher() = default;
    ~AeroAPIFetcher() override = default;

    bool fetchFlightInfo(const String &flightIdent, FlightInfo &outInfo) override;
};
