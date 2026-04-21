/*
Purpose: Retrieve detailed flight metadata from AeroAPI over HTTPS.
Responsibilities:
- Perform authenticated GET to /flights/{ident} using API key.
- Parse minimal fields into FlightInfo (ident/operator/aircraft and ICAO codes).
- Handle TLS (optionally insecure for dev) and JSON errors gracefully.
Input: flight ident (e.g., callsign).
Output: Populates FlightInfo on success and returns true.
*/
#include "adapters/AeroAPIFetcher.h"
#include <ArduinoHttpClient.h>
#include "utils/HttpUtils.h"

static String safeGetString(JsonVariant v, const char *key)
{
    if (v[key].isNull())
        return String("");
    return String(v[key].as<const char *>());
}

bool AeroAPIFetcher::fetchFlightInfo(const String &flightIdent, FlightInfo &outInfo)
{
    if (strlen(APIConfiguration::AEROAPI_KEY) == 0)
    {
        Serial.println("AeroAPIFetcher: No API key configured");
        return false;
    }

    const String url = String(APIConfiguration::AEROAPI_BASE_URL) + "/flights/" + flightIdent;
    bool https = true;
    String host;
    uint16_t port = 443;
    String path;
    if (!parseUrl(url, https, host, port, path))
    {
        Serial.println("AeroAPIFetcher: Failed to parse URL");
        return false;
    }
#if defined(FLIGHTWALL_SKIP_TLS)
    https = false;
    port = 80;
    Serial.println("AeroAPIFetcher: SKIP_TLS active — forcing plain HTTP on port 80");
#else
    if (!https)
    {
        Serial.println("AeroAPIFetcher: Refusing non-HTTPS URL");
        return false;
    }
#endif

    int code = -1;
    String payload;

#if defined(ARDUINO_ARCH_ESP32)
    {
        FlightWallTlsClient net;
        #if !defined(FLIGHTWALL_SKIP_TLS)
        if (APIConfiguration::AEROAPI_INSECURE_TLS)
            net.setInsecure();
        #endif
        HttpClient http(net, host.c_str(), port);
        http.setHttpResponseTimeout(30000);
        http.beginRequest();
        http.get(path);
        http.sendHeader("x-apikey", APIConfiguration::AEROAPI_KEY);
        http.sendHeader("Accept", "application/json");
        http.endRequest();
        code = http.responseStatusCode();
        payload = http.responseBody();
    }
#else
    {
        const String extraHeaders = String("x-apikey: ") + APIConfiguration::AEROAPI_KEY + "\r\nAccept: application/json\r\n";
        if (!wifiClientRequest("GET", host, port, path, extraHeaders, "", code, payload))
        {
            Serial.printf("AeroAPIFetcher: wifiClientRequest failed for flight %s\n", flightIdent.c_str());
            return false;
        }
    }
#endif

    if (code != 200)
    {
        Serial.printf("AeroAPIFetcher: HTTP request failed with code %d for flight %s\n", code, flightIdent.c_str());
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    payload = String(); // free ~25 KB before extracting fields from doc
    if (err)
    {
        Serial.printf("AeroAPIFetcher: JSON parsing failed for flight %s: %s\n", flightIdent.c_str(), err.c_str());
        return false;
    }

    JsonArray flights = doc["flights"].as<JsonArray>();
    if (flights.isNull() || flights.size() == 0)
    {
        Serial.printf("AeroAPIFetcher: No flights found in response for %s\n", flightIdent.c_str());
        return false;
    }

    JsonObject f = flights[0].as<JsonObject>();
    outInfo.ident = safeGetString(f, "ident");
    outInfo.ident_icao = safeGetString(f, "ident_icao");
    outInfo.ident_iata = safeGetString(f, "ident_iata");
    outInfo.operator_code = safeGetString(f, "operator");
    outInfo.operator_icao = safeGetString(f, "operator_icao");
    outInfo.operator_iata = safeGetString(f, "operator_iata");
    outInfo.aircraft_code = safeGetString(f, "aircraft_type");

    if (!f["origin"].isNull() && f["origin"].is<JsonObject>())
    {
        JsonObject o = f["origin"].as<JsonObject>();
        outInfo.origin.code_icao = safeGetString(o, "code_icao");
        outInfo.origin.code_iata = safeGetString(o, "code_iata");
    }

    if (!f["destination"].isNull() && f["destination"].is<JsonObject>())
    {
        JsonObject d = f["destination"].as<JsonObject>();
        outInfo.destination.code_icao = safeGetString(d, "code_icao");
        outInfo.destination.code_iata = safeGetString(d, "code_iata");
    }

    return true;
}
