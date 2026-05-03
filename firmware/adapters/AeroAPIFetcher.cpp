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
#include <vector>
#include "utils/HttpUtils.h"
#include "utils/MemoryUtils.h"

static String safeGetString(JsonVariant v, const char *key)
{
    if (v[key].isNull())
        return String("");
    return String(v[key].as<const char *>());
}

static bool tryGetDouble(JsonVariant v, const char *key, double &out)
{
    if (v.isNull() || v[key].isNull())
        return false;
    out = v[key].as<double>();
    return true;
}

static void extractAirportLatLon(JsonObject airportObj, double &outLat, double &outLon)
{
    // AeroAPI field shapes vary by endpoint/version; try the common ones.
    // We intentionally accept 0,0 here; later callers treat that as "not plausible".
    double lat = NAN, lon = NAN;

    // Flat fields
    if (tryGetDouble(airportObj, "latitude", lat))  outLat = lat;
    if (tryGetDouble(airportObj, "longitude", lon)) outLon = lon;
    if (!isnan(outLat) && !isnan(outLon)) return;

    // Alternative flat names
    if (tryGetDouble(airportObj, "lat", lat))  outLat = lat;
    if (tryGetDouble(airportObj, "lon", lon))  outLon = lon;
    if (tryGetDouble(airportObj, "lng", lon))  outLon = lon;
    if (!isnan(outLat) && !isnan(outLon)) return;

    // Nested: airport.position.lat/lon
    if (!airportObj["position"].isNull() && airportObj["position"].is<JsonObject>())
    {
        JsonObject pos = airportObj["position"].as<JsonObject>();
        if (tryGetDouble(pos, "lat", lat)) outLat = lat;
        if (tryGetDouble(pos, "lon", lon)) outLon = lon;
        if (tryGetDouble(pos, "lng", lon)) outLon = lon;
        if (tryGetDouble(pos, "latitude", lat)) outLat = lat;
        if (tryGetDouble(pos, "longitude", lon)) outLon = lon;
        if (!isnan(outLat) && !isnan(outLon)) return;
    }

    // Nested: airport.airport.latitude/longitude (yes, sometimes double-nested)
    if (!airportObj["airport"].isNull() && airportObj["airport"].is<JsonObject>())
    {
        JsonObject inner = airportObj["airport"].as<JsonObject>();
        if (tryGetDouble(inner, "latitude", lat))  outLat = lat;
        if (tryGetDouble(inner, "longitude", lon)) outLon = lon;
        if (tryGetDouble(inner, "lat", lat)) outLat = lat;
        if (tryGetDouble(inner, "lon", lon)) outLon = lon;
        if (tryGetDouble(inner, "lng", lon)) outLon = lon;
    }
}

struct AirportCoordCacheEntry
{
    String code; // ICAO or IATA
    double lat = NAN;
    double lon = NAN;
};

static bool fetchAirportLatLonFromAeroAPI(const String &airportCode, double &outLat, double &outLon)
{
    if (airportCode.length() == 0)
        return false;

    // Small in-memory cache to avoid repeated calls while cycling display.
    static std::vector<AirportCoordCacheEntry> s_cache;
    for (const auto &e : s_cache)
    {
        if (e.code == airportCode)
        {
            outLat = e.lat;
            outLon = e.lon;
            return !(isnan(outLat) || isnan(outLon));
        }
    }

    const String url = String(APIConfiguration::AEROAPI_BASE_URL) + "/airports/" + airportCode;
    bool https = true;
    String host;
    uint16_t port = 443;
    String path;
    if (!parseUrl(url, https, host, port, path))
        return false;

#if defined(FLIGHTWALL_SKIP_TLS)
    https = false;
    port = 80;
#else
    if (!https)
        return false;
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
            return false;
    }
#endif

    if (code != 200)
    {
        flightwallStringDrop(payload);
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    flightwallStringDrop(payload);
    if (err)
    {
        doc.clear();
        return false;
    }

    // Typical fields: latitude/longitude; accept alternates too.
    double lat = NAN, lon = NAN;
    tryGetDouble(doc.as<JsonVariant>(), "latitude", lat);
    tryGetDouble(doc.as<JsonVariant>(), "longitude", lon);
    if (isnan(lat) || isnan(lon))
    {
        tryGetDouble(doc.as<JsonVariant>(), "lat", lat);
        tryGetDouble(doc.as<JsonVariant>(), "lon", lon);
        if (isnan(lon))
            tryGetDouble(doc.as<JsonVariant>(), "lng", lon);
    }

    doc.clear();

    if (s_cache.size() < 48)
    {
        AirportCoordCacheEntry e;
        e.code = airportCode;
        e.lat = lat;
        e.lon = lon;
        s_cache.push_back(e);
    }

    outLat = lat;
    outLon = lon;
    return !(isnan(outLat) || isnan(outLon));
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
    flightwallStringDrop(payload);
    if (err)
    {
        Serial.printf("AeroAPIFetcher: JSON parsing failed for flight %s: %s\n", flightIdent.c_str(), err.c_str());
        doc.clear();
        return false;
    }

    JsonArray flights = doc["flights"].as<JsonArray>();
    if (flights.isNull() || flights.size() == 0)
    {
        Serial.printf("AeroAPIFetcher: No flights found in response for %s\n", flightIdent.c_str());
        doc.clear();
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
        extractAirportLatLon(o, outInfo.origin.latitude, outInfo.origin.longitude);
    }

    if (!f["destination"].isNull() && f["destination"].is<JsonObject>())
    {
        JsonObject d = f["destination"].as<JsonObject>();
        outInfo.destination.code_icao = safeGetString(d, "code_icao");
        outInfo.destination.code_iata = safeGetString(d, "code_iata");
        extractAirportLatLon(d, outInfo.destination.latitude, outInfo.destination.longitude);
    }

    // If the /flights payload did not include airport coordinates, query /airports/{code}
    // so nearby mode can compute progress from origin/destination/current position.
    if ((isnan(outInfo.origin.latitude) || isnan(outInfo.origin.longitude)))
    {
        const String codeToTry = outInfo.origin.code_icao.length() ? outInfo.origin.code_icao
                                                                   : outInfo.origin.code_iata;
        fetchAirportLatLonFromAeroAPI(codeToTry, outInfo.origin.latitude, outInfo.origin.longitude);
    }
    if ((isnan(outInfo.destination.latitude) || isnan(outInfo.destination.longitude)))
    {
        const String codeToTry = outInfo.destination.code_icao.length() ? outInfo.destination.code_icao
                                                                        : outInfo.destination.code_iata;
        fetchAirportLatLonFromAeroAPI(codeToTry, outInfo.destination.latitude, outInfo.destination.longitude);
    }

    outInfo.progress_percent = 0;
    if (!f["progress_percent"].isNull())
        outInfo.progress_percent = f["progress_percent"].as<int>();

    // AeroAPI often omits progress_percent once the flight has landed.
    {
        const String actualOn = safeGetString(f, "actual_on");
        if (actualOn.length() > 0 && outInfo.progress_percent == 0)
            outInfo.progress_percent = 100;
    }

    doc.clear();
    return true;
}
