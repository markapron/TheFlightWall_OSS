/*
Purpose: Look up human-friendly airline and aircraft names from FlightWall CDN.
Responsibilities:
- HTTPS GET small JSON blobs for airline/aircraft codes and parse display names.
- Provide helpers used by FlightDataFetcher for user-facing labels.
Inputs: Airline ICAO code or aircraft ICAO type.
Outputs: Display name strings (short/full) via out parameters.
*/
#include "adapters/FlightWallFetcher.h"
#include <ArduinoHttpClient.h>
#include "utils/HttpUtils.h"
#include "utils/MemoryUtils.h"

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

bool FlightWallFetcher::httpGetJson(const String &url, String &outPayload)
{
    bool https = true;
    String host;
    uint16_t port = 443;
    String path;
    if (!parseUrl(url, https, host, port, path))
        return false;

#if defined(FLIGHTWALL_SKIP_TLS)
    https = false;
    port = 80;
    Serial.println("FlightWallFetcher: SKIP_TLS active — forcing plain HTTP on port 80");
#else
    if (!https)
        return false;
#endif

    int code = -1;

#if defined(ARDUINO_ARCH_ESP32)
    {
        FlightWallTlsClient net;
        #if !defined(FLIGHTWALL_SKIP_TLS)
        if (APIConfiguration::FLIGHTWALL_INSECURE_TLS)
            net.setInsecure();
        #endif
        HttpClient http(net, host.c_str(), port);
        http.setHttpResponseTimeout(30000);
        http.beginRequest();
        http.get(path);
        http.sendHeader("Accept", "application/json");
        http.endRequest();
        code = http.responseStatusCode();
        outPayload = http.responseBody();
    }
#else
    {
        const String extraHeaders = "Accept: application/json\r\n";
        if (!wifiClientRequest("GET", host, port, path, extraHeaders, "", code, outPayload))
            return false;
    }
#endif

    return code == 200;
}

bool FlightWallFetcher::getAirlineName(const String &airlineIcao, String &outDisplayNameFull)
{
    outDisplayNameFull = String("");
    if (airlineIcao.length() == 0)
        return false;

    String url = String(APIConfiguration::FLIGHTWALL_CDN_BASE_URL) + "/oss/lookup/airline/" + airlineIcao + ".json";
    String payload;
    if (!httpGetJson(url, payload))
        return false;

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err)
    {
        flightwallStringDrop(payload);
        return false;
    }
    flightwallStringDrop(payload);

    if (doc.containsKey("display_name_full"))
    {
        outDisplayNameFull = String(doc["display_name_full"].as<const char *>());
        return outDisplayNameFull.length() > 0;
    }
    return false;
}

bool FlightWallFetcher::getAircraftName(const String &aircraftIcao,
                                        String &outDisplayNameShort,
                                        String &outDisplayNameFull)
{
    outDisplayNameShort = String("");
    outDisplayNameFull = String("");
    if (aircraftIcao.length() == 0)
        return false;

    String url = String(APIConfiguration::FLIGHTWALL_CDN_BASE_URL) + "/oss/lookup/aircraft/" + aircraftIcao + ".json";
    String payload;
    if (!httpGetJson(url, payload))
        return false;

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err)
    {
        flightwallStringDrop(payload);
        return false;
    }
    flightwallStringDrop(payload);

    if (doc.containsKey("display_name_short"))
    {
        outDisplayNameShort = String(doc["display_name_short"].as<const char *>());
    }
    if (doc.containsKey("display_name_full"))
    {
        outDisplayNameFull = String(doc["display_name_full"].as<const char *>());
    }
    return outDisplayNameShort.length() > 0 || outDisplayNameFull.length() > 0;
}
