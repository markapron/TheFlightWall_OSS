/*
Purpose: Fetch ADS-B state vectors from OpenSky Network (OAuth-protected API).
Responsibilities:
- Manage OAuth2 client_credentials token lifecycle with early refresh.
- Build geographic bounding box around a center point and query states/all.
- Parse JSON into StateVector objects and compute distance/bearing.
- Filter by radius and bearing using GeoUtils helpers.
Inputs: centerLat, centerLon, radiusKm, min/max bearing; APIConfiguration creds/URLs.
Outputs: Populates outStateVectors with filtered results (distance_km, bearing_deg set).
*/
#include "adapters/OpenSkyFetcher.h"
#include <ArduinoHttpClient.h>
#include "utils/HttpUtils.h"
#include "utils/MemoryUtils.h"

// On ESP32, ArduinoHttpClient + WiFiClientSecure works correctly.
// On SAMD/AirLift, we use WiFiSSLClient directly via wifiClientRequest() in HttpUtils,
// because ArduinoHttpClient sends headers as many small print() fragments that the AirLift's
// SPI transport never flushes as a complete HTTP request, causing persistent -3 timeouts.
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

static String urlEncodeForm(const String &value)
{
    String out;
    const char *hex = "0123456789ABCDEF";
    for (size_t i = 0; i < value.length(); ++i)
    {
        char c = value[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
        {
            out += c;
        }
        else if (c == ' ')
        {
            out += '+';
        }
        else
        {
            out += '%';
            out += hex[(c >> 4) & 0x0F];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

bool OpenSkyFetcher::ensureAccessToken(bool forceRefresh)
{
    const bool oauthConfigured = (strlen(APIConfiguration::OPENSKY_CLIENT_ID) > 0) && (strlen(APIConfiguration::OPENSKY_CLIENT_SECRET) > 0);
    if (!oauthConfigured)
    {
        Serial.println("OpenSkyFetcher: OAuth credentials are required but not configured");
        return false;
    }

    unsigned long nowMs = millis();
    const unsigned long safetySkewMs = 60UL * 1000UL; // refresh 60s early
    if (!forceRefresh && m_accessToken.length() > 0 && nowMs + safetySkewMs < m_tokenExpiryMs)
    {
        Serial.print("OpenSkyFetcher: Using cached token. ms until refresh window: ");
        Serial.println((long)(m_tokenExpiryMs - safetySkewMs - nowMs));
        return true;
    }

    Serial.println(forceRefresh ? "OpenSkyFetcher: Refreshing token (forced)" : "OpenSkyFetcher: Fetching new token");
    String newToken;
    unsigned long newExpiryMs = 0;
    if (!requestAccessToken(newToken, newExpiryMs))
    {
        Serial.println("OpenSkyFetcher: Failed to obtain OAuth access token");
        return false;
    }

    m_accessToken = newToken;
    flightwallStringDrop(newToken);
    m_tokenExpiryMs = newExpiryMs;
    Serial.print("OpenSkyFetcher: Token cached. Expires at ms: ");
    Serial.println((long)m_tokenExpiryMs);
    return true;
}

bool OpenSkyFetcher::ensureAuthenticated(bool forceRefresh)
{
    return ensureAccessToken(forceRefresh);
}

bool OpenSkyFetcher::requestAccessToken(String &outToken, unsigned long &outExpiryMs)
{
    if (strlen(APIConfiguration::OPENSKY_CLIENT_ID) == 0 || strlen(APIConfiguration::OPENSKY_CLIENT_SECRET) == 0)
    {
        Serial.println("OpenSkyFetcher: OAuth credentials not configured");
        return false;
    }

    const String url = String(APIConfiguration::OPENSKY_TOKEN_URL);
    Serial.print("OpenSkyFetcher: Token URL: ");
    Serial.println(url);

    bool https = true;
    String host;
    uint16_t port = 443;
    String path;
    if (!parseUrl(url, https, host, port, path))
    {
        Serial.println("OpenSkyFetcher: Failed to parse token URL");
        return false;
    }
#if defined(FLIGHTWALL_SKIP_TLS)
    https = false;
    port = 80;
    Serial.println("OpenSkyFetcher: SKIP_TLS active — forcing plain HTTP on port 80 for token request");
#else
    if (!https)
    {
        Serial.println("OpenSkyFetcher: Refusing non-HTTPS token URL");
        return false;
    }
#endif

    const String body = String("grant_type=client_credentials&client_id=") + urlEncodeForm(APIConfiguration::OPENSKY_CLIENT_ID) +
                        "&client_secret=" + urlEncodeForm(APIConfiguration::OPENSKY_CLIENT_SECRET);

    Serial.print("OpenSkyFetcher: Using client_id: ");
    Serial.println(APIConfiguration::OPENSKY_CLIENT_ID);
    Serial.print("OpenSkyFetcher: POST body length: ");
    Serial.println((int)body.length());

    int code = -1;
    String payload;

#if defined(ARDUINO_ARCH_ESP32)
    {
        FlightWallTlsClient net;
        #if defined(ARDUINO_ARCH_ESP32) && !defined(FLIGHTWALL_SKIP_TLS)
        // ESP32: no cert validation needed for development
        #endif
        HttpClient http(net, host.c_str(), port);
        http.setHttpResponseTimeout(30000);
        http.beginRequest();
        http.post(path);
        http.sendHeader("Content-Type", "application/x-www-form-urlencoded");
        http.sendHeader("Accept", "application/json");
        http.sendHeader("Content-Length", body.length());
        http.beginBody();
        http.print(body);
        http.endRequest();
        code = http.responseStatusCode();
        payload = http.responseBody();
    }
#else
    {
        const String extraHeaders = "Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n";
        if (!wifiClientRequest("POST", host, port, path, extraHeaders, body, code, payload))
        {
            Serial.println("OpenSkyFetcher: Token request (wifiClientRequest) failed");
            return false;
        }
    }
#endif

    if (code != 200)
    {
        Serial.print("OpenSkyFetcher: Token request failed, code: ");
        Serial.println(code);
        Serial.print("OpenSkyFetcher: Error payload: ");
        Serial.println(payload.length() > 0 ? payload : String("<empty>"));
        return false;
    }

    DynamicJsonDocument doc(12288);
    DeserializationError err = deserializeJson(doc, payload);
    if (err)
    {
        Serial.print("OpenSkyFetcher: Token JSON parse error: ");
        Serial.println(err.c_str());
        doc.clear();
        flightwallStringDrop(payload);
        return false;
    }
    flightwallStringDrop(payload);

    String tokenStr = doc["access_token"].as<String>();
    int expiresIn = doc["expires_in"] | 1800; // seconds; default 30min
    if (tokenStr.length() == 0)
    {
        Serial.println("OpenSkyFetcher: access_token missing in response");
        if (doc.is<JsonObject>())
        {
            Serial.println("OpenSkyFetcher: Response keys:");
            for (JsonPair kv : doc.as<JsonObject>())
            {
                Serial.print(" - ");
                Serial.println(kv.key().c_str());
            }
        }
        doc.clear();
        return false;
    }

    outToken = tokenStr;
    outExpiryMs = millis() + (unsigned long)expiresIn * 1000UL;
    doc.clear();
    flightwallStringDrop(tokenStr);
    Serial.print("OpenSkyFetcher: Obtained access token, length: ");
    Serial.println((int)outToken.length());
    Serial.print("OpenSkyFetcher: Token expires in (s): ");
    Serial.println(expiresIn);
    return true;
}

bool OpenSkyFetcher::fetchStateVectors(double centerLat,
                                       double centerLon,
                                       double radiusKm,
                                       std::vector<StateVector> &outStateVectors)
{
    // Ensure OAuth token if configured
    if (!ensureAccessToken(false))
    {
        Serial.println("OpenSkyFetcher: ensureAccessToken failed before GET");
        return false;
    }

    double latMin, latMax, lonMin, lonMax;
    centeredBoundingBox(centerLat, centerLon, radiusKm, latMin, latMax, lonMin, lonMax);

    const String url = String(APIConfiguration::OPENSKY_BASE_URL) + "/api/states/all?lamin=" + String(latMin, 6) +
                 "&lamax=" + String(latMax, 6) +
                 "&lomin=" + String(lonMin, 6) +
                 "&lomax=" + String(lonMax, 6);

    bool https = true;
    String host;
    uint16_t port = 443;
    String path;
    if (!parseUrl(url, https, host, port, path))
    {
        Serial.println("OpenSkyFetcher: Failed to parse states URL");
        return false;
    }
#if defined(FLIGHTWALL_SKIP_TLS)
    https = false;
    port = 80;
    Serial.println("OpenSkyFetcher: SKIP_TLS active — forcing plain HTTP on port 80 for states request");
#else
    if (!https)
    {
        Serial.println("OpenSkyFetcher: Refusing non-HTTPS states URL");
        return false;
    }
#endif

    int code = -1;
    String payload;

    // Helper that performs the states GET with the current access token.
    // Called once normally, then again after a 401 token refresh.
    auto doStatesGet = [&]() -> bool {
#if defined(ARDUINO_ARCH_ESP32)
        FlightWallTlsClient net;
        HttpClient http(net, host.c_str(), port);
        http.setHttpResponseTimeout(30000);
        http.beginRequest();
        http.get(path);
        http.sendHeader("Authorization", String("Bearer ") + m_accessToken);
        http.endRequest();
        code = http.responseStatusCode();
        payload = http.responseBody();
        return true;
#else
        const String extraHeaders = String("Authorization: Bearer ") + m_accessToken + "\r\nAccept: application/json\r\n";
        return wifiClientRequest("GET", host, port, path, extraHeaders, "", code, payload);
#endif
    };

    if (!doStatesGet())
    {
        Serial.println("OpenSkyFetcher: States GET failed");
        return false;
    }

    // 401: token may have expired — refresh and retry once
    if (code == 401 && m_accessToken.length() > 0 && ensureAccessToken(true))
    {
        flightwallStringDrop(payload);
        if (!doStatesGet())
        {
            Serial.println("OpenSkyFetcher: States GET retry failed after token refresh");
            return false;
        }
    }

    if (code != 200)
    {
        Serial.print("OpenSkyFetcher: HTTP request failed with code: ");
        Serial.println(code);
        return false;
    }

    // The OpenSky states response is ~20-25 KB of JSON. ArduinoJson needs roughly
    // 1.5x the raw JSON size for its internal representation. 48 KB is safe on the
    // SAMD51's 192 KB of RAM, provided we free the raw payload string first.
    const size_t payloadBytes = payload.length();
    DynamicJsonDocument doc(49152);
    DeserializationError err = deserializeJson(doc, payload);
    flightwallStringDrop(payload);
    if (err)
    {
        Serial.print("OpenSkyFetcher: JSON deserialization error: ");
        Serial.print(err.c_str());
        Serial.print(" (payload length: ");
        Serial.print((unsigned)payloadBytes);
        Serial.println(")");
        doc.clear();
        return false;
    }

    JsonArray states = doc["states"].as<JsonArray>();
    if (states.isNull())
    {
        doc.clear();
        return true; // no states is not an error
    }

    for (JsonVariant v : states)
    {
        if (!v.is<JsonArray>())
        {
            Serial.println("OpenSkyFetcher: Expected array element in states");
            continue;
        }
        JsonArray a = v.as<JsonArray>();
        if (a.size() < 17)
        {
            Serial.println("OpenSkyFetcher: State vector array has insufficient elements");
            continue;
        }

        StateVector s;
        s.icao24 = a[0].as<const char *>();
        s.callsign = a[1].isNull() ? String("") : String(a[1].as<const char *>());
        s.callsign.trim();
        s.origin_country = a[2].isNull() ? String("") : String(a[2].as<const char *>());
        s.time_position = a[3].isNull() ? 0 : a[3].as<long>();
        s.last_contact = a[4].isNull() ? 0 : a[4].as<long>();
        s.lon = a[5].isNull() ? NAN : a[5].as<double>();
        s.lat = a[6].isNull() ? NAN : a[6].as<double>();
        s.baro_altitude = a[7].isNull() ? NAN : a[7].as<double>();
        s.on_ground = a[8].isNull() ? false : a[8].as<bool>();
        s.velocity = a[9].isNull() ? NAN : a[9].as<double>();
        s.heading = a[10].isNull() ? NAN : a[10].as<double>();
        s.vertical_rate = a[11].isNull() ? NAN : a[11].as<double>();
        s.sensors = a[12].isNull() ? 0 : a[12].as<long>();
        s.geo_altitude = a[13].isNull() ? NAN : a[13].as<double>();
        s.squawk = a[14].isNull() ? String("") : String(a[14].as<const char *>());
        s.spi = a[15].isNull() ? false : a[15].as<bool>();
        s.position_source = a[16].isNull() ? 0 : a[16].as<int>();

        if (isnan(s.lat) || isnan(s.lon))
        {
            Serial.println("OpenSkyFetcher: Skipping state vector with invalid coordinates");
            continue;
        }

        s.distance_km = haversineKm(centerLat, centerLon, s.lat, s.lon);
        if (s.distance_km > radiusKm)
            continue;
        s.bearing_deg = computeBearingDeg(centerLat, centerLon, s.lat, s.lon);

        outStateVectors.push_back(s);
    }

    doc.clear();
    return true;
}
